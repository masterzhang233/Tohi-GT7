#pragma once

#include <atomic>
#include <cstdint>

namespace Libs::Graphics {

// Observation metadata only; neither counter synchronizes image contents.
class ImageExternalWriteState {
public:
    [[nodiscard]] uint64_t Version() const noexcept { return m_version.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t Epoch() const noexcept { return m_epoch.load(std::memory_order_relaxed); }

    void NoteWrite() noexcept {
        // A version acquire observes the preceding notification epoch. This
        // orders these two metadata counters, not the image's pixel contents.
        m_epoch.fetch_add(1, std::memory_order_relaxed);
        m_version.fetch_add(1, std::memory_order_release);
    }
    void NoteRefresh() noexcept { m_version.fetch_add(1, std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> m_version {1};
    std::atomic<uint64_t> m_epoch {1};
};

} // namespace Libs::Graphics
