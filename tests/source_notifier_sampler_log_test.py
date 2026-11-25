"""Check real CPU fixture output; no emulator/game or invented sample events."""
import argparse
import json
from pathlib import Path
import re
import subprocess


def fields(line):
    result = {}
    for key, value in re.findall(r"(\w+)=([^\s]+)", line):
        try:
            result[key] = int(value, 0)
        except ValueError:
            result[key] = value
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable")
    parser.add_argument("--case", required=True, choices=["selected", "unknown", "disabled", "trace_off", "source_off"])
    parser.add_argument("--chain", action="store_true", help="also validate opted-in chain reporter metadata")
    args = parser.parse_args()
    fixture = "--unknown-fixture" if args.case == "unknown" else "--notifier-fixture"
    run = subprocess.run([args.executable, fixture], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                         errors="replace", timeout=25)
    log = Path.cwd() / f"source_notifier_{args.case}{'_chain' if args.chain else ''}.log"
    log.write_text(run.stdout, encoding="utf-8")
    lines = run.stdout.splitlines()
    errors = []

    def check(ok, message):
        if not ok:
            errors.append(message)

    def records(prefix):
        return [fields(line) for line in lines if line.startswith(prefix)]

    check(run.returncode == 0, f"fixture exit {run.returncode}")
    check(any(line.startswith("Notifier fixture PASS:") for line in lines), "fixture completion/progress marker absent")
    identities = records("Notifier fixture identities:")
    phases = records("Notifier fixture phase:")
    headers = records("GT7 source sample start:")
    targets = records("GT7 source sample target:")
    samples = records("GT7 source sample:")
    summaries = records("GT7 source sample summary:")
    check(not any(line.startswith("Sample:") for line in lines), "notifier mode fell back to legacy named sampling")
    if args.case in ("disabled", "trace_off", "source_off"):
        check(not headers and not targets and not samples and not summaries,
              "missing source/trace prerequisite still activated timed sampling")
    else:
        check(len(headers) == 1 and headers[0].get("notifier_only") == 1,
              "notifier-only source header absent")
        if args.case == "unknown":
            check(not targets and not samples, "unknown notifier selected producer/named fallback")
        elif identities:
            ids = identities[0]
            allowed = {ids["first"], ids["second"]}
            check(bool(samples), "enabled mode produced no real samples")
            check({s["tid"] for s in samples} == allowed, "sampled target set differs from observed notifier workers")
            check({t["tid"] for t in targets} == allowed, "unexpected/missing target identity")
            first_generations = {s["target"] for s in samples if s["tid"] == ids["first"]}
            second_generations = {s["target"] for s in samples if s["tid"] == ids["second"]}
            check(bool(first_generations) and bool(second_generations) and
                  first_generations.isdisjoint(second_generations), "distinct notifier threads share a target generation")
            for phase in phases:
                # Allow one refresh/capture boundary around each source publication.
                interior = [s for s in samples if phase["begin_ns"] + 200_000_000 <= s["begin_ns"]
                            and s["end_ns"] <= phase["end_ns"] - 100_000_000]
                expected = phase["expected_tid"]
                if expected:
                    check(len(interior) >= 10 and all(s["tid"] == expected for s in interior),
                          f"{phase['name']}: insufficient captures or stale target persisted")
                else:
                    check(not interior, f"{phase['name']}: unknown/self/retired selection still captured")
            check(len(phases) == 7, "missing unknown/self/handoff/retirement phase coverage")
        else:
            check(False, "fixture identity marker absent")
        for sample in samples:
            check(sample.get("status") == 0, "capture failed while fixture targets were alive")
            check(sample["begin_ns"] <= sample["end_ns"] <= sample["unwind_end_ns"], "sample timestamps unordered")
            check(sample.get("notifier") == 1 and sample.get("observed_notifier_tid") == sample["tid"],
                  "sample not tied to its observed notifier identity")
        for summary in summaries:
            for key in ("failed", "buffer_dropped", "target_cap_skipped", "open_failed", "exhausted", "resume_failed"):
                check(summary.get(key) == 0, f"nonzero or absent {key}")
        if args.case == "selected":
            check(bool(summaries), "sampler never reported counters")
    chain_summary = None
    if args.chain:
        trace_headers = records("GT7 source trace:")
        trace_summaries = records("GT7 source trace summary:")
        trace_reports = records("GT7 source trace report:")
        chain_threads = records("GT7 source chain thread:")
        trace_events = records("GT7 source event:")
        check(len(trace_headers) == 1, "chain mode has no unique source trace header")
        if trace_headers:
            expected = {"chain_enabled": 1, "chain_limit": 32, "slots": 12288,
                        "budget": 480000, "base_slots": 4096, "base_budget": 120000,
                        "extra_slots": 8192, "extra_budget": 360000, "tail_incomplete": 1}
            for key, value in expected.items():
                check(trace_headers[0].get(key) == value, f"chain header {key} differs from {value}")
        check(bool(trace_summaries), "chain reporter did not publish separate counters")
        for summary in trace_summaries:
            check(summary.get("chain_enabled") == 1 and summary.get("chain_limit") == 32,
                  "chain summary mode/capacity missing")
            check(3 <= summary.get("chain_count", 0) <= 32 and
                  summary.get("chain_admitted") == summary.get("chain_count"),
                  "upstream fixture notifications did not expand bounded historical coverage")
            for key in ("attempted", "captured", "drained", "dropped", "budget_skipped"):
                check(summary.get(key) == summary.get(f"base_{key}", -1) + summary.get(f"extra_{key}", -1),
                      f"chain aggregate {key} does not equal its separate buckets")
            check(summary.get("base_captured", 0) > 0 and summary.get("extra_captured", 0) > 0,
                  "real Record did not place events in both buckets")
            for key in ("dropped", "budget_skipped", "exhausted", "base_dropped", "extra_dropped",
                        "base_exhausted", "extra_exhausted", "chain_rejected", "chain_budget_blocked",
                        "chain_identity_unavailable"):
                check(summary.get(key) == 0, f"unexpected chain reporter {key}")
        check(bool(trace_reports), "chain reporter wall-time brackets absent")
        for report in trace_reports:
            check(report.get("begin_ns", 0) > 0 and report.get("end_ns", 0) >= report.get("begin_ns", 0) and
                  report.get("wall_ns") == report.get("end_ns", 0) - report.get("begin_ns", 0),
                  "chain report wall-time bracket is invalid")
            check(report.get("base_events", -1) >= 0 and report.get("extra_events", -1) >= 0,
                  "chain report lacks separate event counts")
        check(any(r.get("extra_events", 0) > 0 for r in trace_reports), "extra bucket never drained")
        check(any(e.get("kind") == 7 for e in trace_events), "chain fixture notification events absent")
        if identities:
            ids = identities[0]
            expected_tids = {ids["producer"], ids["first"], ids["second"], ids["decoy"]}
            check({t.get("tid") for t in chain_threads} == expected_tids,
                  "chain dictionary does not include the upstream decoy and actual fixture identities")
        check(len({t.get("slot") for t in chain_threads}) == len(chain_threads) and
              len({t.get("tid") for t in chain_threads}) == len(chain_threads),
              "chain dictionary repeats slots or admitted identities")
        for thread in chain_threads:
            check(0 <= thread.get("slot", -1) < 32 and thread.get("query_ns", 0) > 0 and
                  thread.get("live") == 1 and thread.get("created", 0) > 0,
                  "live fixture thread metadata is absent/invalid")
        for line in lines:
            if line.startswith("GT7 source chain thread:"):
                try:
                    check(isinstance(json.loads(line.split(" name=", 1)[1]), str),
                          "chain thread name is not a JSON string")
                except (IndexError, json.JSONDecodeError):
                    check(False, "chain thread name cannot be decoded")
        chain_summary = {"last_summary": trace_summaries[-1] if trace_summaries else None,
                         "dictionary_count": len(chain_threads), "report_count": len(trace_reports),
                         "reported_base_events": sum(r.get("base_events", 0) for r in trace_reports),
                         "reported_extra_events": sum(r.get("extra_events", 0) for r in trace_reports)}
    outcome = {"case": args.case, "passed": not errors, "log": str(log),
               "sample_count": len(samples), "target_count": len(targets),
               "last_summary": summaries[-1] if summaries else None,
               "chain": chain_summary,
               "limitations": "Real worker captures; no game, fault injection, TID reuse or full final-tail certification.",
               "errors": errors}
    print(json.dumps(outcome, indent=2))
    if errors:
        print(run.stdout[-16000:])
    return bool(errors)


if __name__ == "__main__":
    raise SystemExit(main())
