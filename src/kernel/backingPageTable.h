#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace Libs::LibKernel::Memory {

// One writer (the address-space operation lock), any number of readers. Buckets and the
// backing alias remain alive until all guest/render threads have stopped. Clearing a page
// never frees a bucket, so a read racing an unmap may safely finish using the old backing.
template <uint64_t AddressLimit>
class BackingPageTable {
public:
	// Host-page precision preserves partial mappings; guest maps still use 16 KiB pages.
	static constexpr uint64_t PageBits = 12;
	static constexpr uint64_t PageSize = uint64_t {1} << PageBits;
	static constexpr uint64_t PageMask = PageSize - 1;
	static constexpr uint64_t BucketPages = uint64_t {1} << 16;
	static constexpr uint64_t BucketCount = (AddressLimit + PageSize * BucketPages - 1) /
	                                        (PageSize * BucketPages);
	static_assert(std::atomic<uint32_t>::is_always_lock_free);
	static_assert(std::atomic<std::atomic<uint32_t>*>::is_always_lock_free);

	~BackingPageTable() {
		for (auto& bucket: m_buckets) delete[] bucket.load(std::memory_order_relaxed);
	}
	BackingPageTable() = default;
	BackingPageTable(const BackingPageTable&) = delete;
	BackingPageTable& operator=(const BackingPageTable&) = delete;

	[[nodiscard]] uint32_t Load(uint64_t page) const {
		if (page / BucketPages >= BucketCount) return 0;
		const auto* bucket = m_buckets[page / BucketPages].load(std::memory_order_acquire);
		return bucket != nullptr ? bucket[page % BucketPages].load(std::memory_order_acquire) : 0;
	}

	[[nodiscard]] bool Contains(uint64_t address, uint64_t size) const {
		if (!ValidRange(address, size)) return false;
		for (auto page = address >> PageBits; page <= (address + size - 1) >> PageBits; ++page) {
			if (Load(page) == 0) return false;
		}
		return true;
	}

	// Called only after a host map succeeds; rollback republishes through this same path.
	bool Store(uint64_t address, uint64_t size, uint64_t offset) {
		if (!ValidMapping(address, size, offset)) return false;
		for (auto page = address >> PageBits; page < (address + size) >> PageBits; ++page) {
			auto& slot = m_buckets[page / BucketPages];
			auto* bucket = slot.load(std::memory_order_relaxed);
			if (bucket == nullptr) {
				bucket = new std::atomic<uint32_t>[BucketPages] {};
				slot.store(bucket, std::memory_order_release);
			}
			bucket[page % BucketPages].store(static_cast<uint32_t>(offset >> PageBits) + 1,
			                                std::memory_order_release);
			offset += PageSize;
		}
		return true;
	}

	bool Clear(uint64_t address, uint64_t size) {
		if (!ValidMapping(address, size, 0)) return false;
		for (auto page = address >> PageBits; page < (address + size) >> PageBits; ++page) {
			auto* bucket = m_buckets[page / BucketPages].load(std::memory_order_relaxed);
			if (bucket != nullptr) bucket[page % BucketPages].store(0, std::memory_order_release);
		}
		return true;
	}

	template <typename Region, typename Gap>
	void ForEachRun(uint64_t address, uint64_t size, Region&& region, Gap&& gap) const {
		if (!ValidRange(address, size)) return;
		const auto end = address + size;
		for (auto start = address; start < end;) {
			const auto entry = Load(start >> PageBits);
			const uint32_t step = entry != 0 ? 1 : 0;
			uint64_t expected = uint64_t {entry} + step;
			auto stop = (start | PageMask) + 1;
			while (stop < end && Load(stop >> PageBits) == expected) {
				stop += PageSize;
				expected += step;
			}
			if (stop > end) stop = end;
			if (entry != 0) {
				region((uint64_t {entry - 1} << PageBits) + (start & PageMask), start, stop - start);
			} else {
				gap(start, stop - start);
			}
			start = stop;
		}
	}

	static constexpr bool ValidMapping(uint64_t address, uint64_t size, uint64_t offset) {
		constexpr uint64_t BackingLimit = uint64_t {UINT32_MAX} * PageSize;
		return ValidRange(address, size) && ((address | size | offset) & PageMask) == 0 &&
		       offset < BackingLimit && size <= BackingLimit - offset;
	}

	static constexpr bool ValidRange(uint64_t address, uint64_t size) {
		return size != 0 && address < AddressLimit && size <= AddressLimit - address;
	}

private:
	std::array<std::atomic<std::atomic<uint32_t>*>, BucketCount> m_buckets {};
};

} // namespace Libs::LibKernel::Memory
