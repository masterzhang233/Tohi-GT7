#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace Libs::Graphics::Gt7FrameGenAssociation {

// This is only the association between a completed scene and its output. The
// caller owns GPU resources, synchronization, and validation of scene inputs.
struct OutputIdentity {
    uint64_t image_id = 0;
    uint64_t backing = 0;
    uint64_t address = 0;
    uint64_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    bool operator==(const OutputIdentity&) const = default;
};

struct Candidate {
    OutputIdentity output {};
    uint64_t scene_serial = 0;
    bool reserved = false;
    bool ready = false;
    bool consumed = false;
};

class CandidateState {
public:
    static constexpr size_t Capacity = 4;

    // A writer supersedes any older result for the same resource or overlapping
    // guest memory. Other pending outputs must survive an intervening scene.
    // Reserve/record private copies/Commit is one synchronous owner transaction.
    // Slot indices must not be retained for an asynchronous Commit after another
    // mutating call: invalidated slots can be reused by a newer transaction.
    [[nodiscard]] std::optional<size_t> Reserve(const OutputIdentity& output, uint64_t scene_serial) {
        Invalidate(output);
        if (!Valid(output) || scene_serial == 0) return std::nullopt;
        for (size_t i = 0; i < Capacity; ++i) {
            auto& candidate = m_candidates[i];
            if (candidate.reserved && !candidate.consumed) continue;
            candidate = {output, scene_serial, true, false, false};
            return i;
        }
        // Never evict an unrelated completed output that has not been flipped.
        return std::nullopt;
    }

    bool Commit(size_t slot, bool complete_and_single_taa) {
        if (slot >= Capacity) return false;
        auto& candidate = m_candidates[slot];
        if (!candidate.reserved || candidate.consumed) return false;
        if (!complete_and_single_taa) {
            InvalidateSlot(slot);
            return false;
        }
        candidate.ready = true;
        return true;
    }

    [[nodiscard]] std::optional<size_t> Find(const OutputIdentity& output) const {
        if (!Valid(output)) return std::nullopt;
        std::optional<size_t> found;
        for (size_t i = 0; i < Capacity; ++i) {
            const auto& candidate = m_candidates[i];
            if (!candidate.reserved || !candidate.ready || candidate.consumed || candidate.output != output) continue;
            if (found) return std::nullopt;
            found = i;
        }
        return found;
    }

    bool Consume(size_t slot) {
        if (slot >= Capacity) return false;
        auto& candidate = m_candidates[slot];
        if (!candidate.reserved || !candidate.ready || candidate.consumed) return false;
        candidate.consumed = true;
        return true;
    }

    void InvalidateSlot(size_t slot) {
        if (slot >= Capacity) return;
        // Retain identity/serial as a tombstone: a failed old-output flip must
        // not be mistaken for the new scene that the guest is still recording.
        m_candidates[slot].reserved = false;
        m_candidates[slot].ready = false;
    }

    void Invalidate(const OutputIdentity& output) {
        if (!ValidRange(output.address, output.size)) {
            InvalidateAll();
            return;
        }
        for (size_t i = 0; i < Capacity; ++i) {
            const auto& candidate = m_candidates[i];
            if (!candidate.reserved) continue;
            if (candidate.output.image_id == output.image_id ||
                (output.backing != 0 && candidate.output.backing == output.backing) ||
                Overlaps(candidate.output.address, candidate.output.size, output.address, output.size)) {
                InvalidateSlot(i);
            }
        }
    }

    void InvalidateRange(uint64_t address, uint64_t size) {
        if (!ValidRange(address, size)) {
            InvalidateAll();
            return;
        }
        for (size_t i = 0; i < Capacity; ++i) {
            const auto& candidate = m_candidates[i];
            if (candidate.reserved && Overlaps(candidate.output.address, candidate.output.size, address, size)) {
                InvalidateSlot(i);
            }
        }
    }

    void InvalidateAll() {
        for (size_t i = 0; i < Capacity; ++i) InvalidateSlot(i);
    }

    [[nodiscard]] const Candidate& Get(size_t slot) const { return m_candidates[slot]; }

private:
    static bool ValidRange(uint64_t address, uint64_t size) {
        return address != 0 && size != 0 && size <= std::numeric_limits<uint64_t>::max() - address;
    }
    static bool Valid(const OutputIdentity& output) {
        return output.backing != 0 && ValidRange(output.address, output.size) &&
               output.width != 0 && output.height != 0 && output.format != 0;
    }
    static bool Overlaps(uint64_t a, uint64_t a_size, uint64_t b, uint64_t b_size) {
        // Subtraction avoids wrapping an end address even for invalidated input.
        return a <= b ? b - a < a_size : a - b < b_size;
    }

    std::array<Candidate, Capacity> m_candidates {};
};

} // namespace Libs::Graphics::Gt7FrameGenAssociation
