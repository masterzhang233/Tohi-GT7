#ifndef KYTY_COMMON_SAMPLER_H_
#define KYTY_COMMON_SAMPLER_H_

// Diagnostics: KYTY_SAMPLE=<name>[,<name>...] samples the threads with those names (a name ending
// in '*' takes every thread whose name starts with the rest, as Job#*; KYTY_SAMPLE=1 takes
// Thread_Gpu, Job#0, Job#1, GpuRecord, Thread_DrawPrep and GpuPriority) KYTY_SAMPLE_HZ times a
// second (default 1000). Every KYTY_SAMPLE_REPORT seconds (default 10) it logs, for each:
//   - "self": the functions its samples fell in;
//   - "total": the emulator's functions on its stack (each once a sample), the stack walked with
//     the Windows unwind data on a copy taken while the thread was suspended;
//   - "paths": the leaf and the five frames below it, joined by "<" ("called from");
//   - "waits": the samples whose leaf waits in the kernel (a lock, a condition variable, an event,
//     a fence, a sleep), by the emulator's first four frames below the wait;
//   - "lines": the source lines its samples fell on in the emulator's busiest functions;
//   - "depth": how deep the walks went.
// Functions are named from kyty_emulator.pdb next to the exe; code without symbols (the game's) is
// named by address, in 256-byte steps, and the game's hottest code is listed with its bytes
// ("Sample <thread> guest"). Each sample suspends the thread for a few microseconds. Windows only.
//
// KYTY_SAMPLE_SOURCE=1 together with KYTY_GT7_SOURCE_TRACE=1 selects bounded timed records
// instead of aggregate/guest-byte reports. Requires the source trace reporter to be enabled.
// It follows the latest actual flip-submit caller as well as KYTY_SAMPLE patterns, beginning
// after that caller is first observed; an unnamed caller is eligible. Defaults are 50 Hz and
// a 1 s report. The interval is a capture-time uncertainty bracket, not duration coverage:
// begin_ns/end_ns surround CaptureThread (including resume and copy cleanup), unwind_end_ns
// follows the copied-stack walk. Raw path/chain addresses retain exact identity; the bounded
// symbol dictionary is resolved after resume. All counters in summaries are cumulative.
// status bits: 1 suspend failed, 2 context failed, 4 resume failed, 8 empty stack copy. Failed
// attempts have no stack, still consume the 120000-attempt lifetime limit, and remain in output.
// A resume failure enters kernel-call-only recovery until resumed or the handle is signaled;
// that recovery remains inside the capture bracket, after which source sampling stops.
// Records are bounded to 256/target, 64 targets and 4096 symbol names. Full buffers skip capture;
// budget exhaustion drains the partial window and stops the sampler. Ordinary process exit
// can lose the last unreported window: reports explicitly mark this tail and its watermark.
// KYTY_SAMPLE_SOURCE_NOTIFIER=1 additionally selects only the latest observed notifier of
// the actual producer. It requires both source-mode env opt-ins and the enabled reporter;
// KYTY_SAMPLE name patterns are ignored and there is no producer/worker fallback. Unknown,
// self-notifying, inaccessible or ended targets are not sampled. An inaccessible/ended
// numeric producer/notifier pair is not reopened until selection changes, avoiding stale
// TID reuse at the cost of conservative coverage loss. Names/creation times are queried
// through live handles outside suspension; notifier and observed_*_tid fields describe
// one identity snapshot just before capture, not proof of a causal dependency. The
// notifier_unavailable summary field is current selection status, not a cumulative count.
namespace Common::Sampler {

// Starts the sampler when KYTY_SAMPLE or the gated source mode asks for it (first call only).
void Start();

} // namespace Common::Sampler

#endif // KYTY_COMMON_SAMPLER_H_
