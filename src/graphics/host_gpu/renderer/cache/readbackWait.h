#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKWAIT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKWAIT_H_

#include <cstdint>

namespace Libs::Graphics::ReadbackWait {

// Values are also used by the existing per-instruction readback diagnostics.
enum class Outcome : uint8_t {
	None,
	Direct,     // no work for a write to memory without a registered buffer
	Bitmap,     // wait for the GPU writer before reading/refining its bitmap
	Lifted,     // pages held no GPU-written bytes and were lifted
	LiftWaited, // wait for an existing download's publication before lifting
	Clean,      // another read already made the pages current
	Fast,       // copied on the readback queue
	WaitWriter, // wait for a submitted writer, then try a fast readback again
	Download,   // download behind the renderer queue; wait for publication
	Nothing,    // nothing remained to download
	FastWait,   // wait for another read's fast ticket, then revalidate
	Count
};

enum class Dependency : uint8_t { None, FastTicket, GpuWriter, Publication };

constexpr Dependency ForStep(Outcome outcome, uint64_t tick, bool has_fast_ticket) {
	if (has_fast_ticket) return Dependency::FastTicket;
	if (tick == 0) return Dependency::None;
	// Bitmap refinement reads GPU-written bits; it has no publication callback of
	// its own. An empty priority queue therefore says nothing about its writer.
	if (outcome == Outcome::Bitmap || outcome == Outcome::WaitWriter) {
		return Dependency::GpuWriter;
	}
	return Dependency::Publication;
}

// Keep the actual dispatch shared with the offline regression. Ticket completion
// has a separate lifetime/publication path in ReadMemory and never comes here.
template <typename WaitGpuWriter, typename WaitPublication>
void WaitForTick(Dependency dependency, uint64_t tick,
                 WaitGpuWriter&& wait_gpu_writer, WaitPublication&& wait_publication) {
	if (dependency == Dependency::GpuWriter) {
		wait_gpu_writer(tick);
	} else if (dependency == Dependency::Publication) {
		wait_publication(tick);
	}
}

} // namespace Libs::Graphics::ReadbackWait

#endif
