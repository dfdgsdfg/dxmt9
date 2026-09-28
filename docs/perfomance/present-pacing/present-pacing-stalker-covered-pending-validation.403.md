---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "H403: Covered-Pending observer-OFF reversed repeats"
type: evidence
status: fast-path-exercised-no-fps-gain-demonstrated
updated: 2026-09-27
source: traces/stkcop-pending-h403/{run.py,analyze.py,summary.json,native-testlog.txt}; experiments/output/app-d3d9-stkcop-bench-pending-h403-{off-warm,on-warm,off-r1,on-r1,on-r2,off-r2,on-counts}/result.json
related: present-pacing-stalker-query-split-census.402.md; present-pacing-stalker-renderer-comparison.399.md
---

# H403: Pending avoids the mutex, but no throughput gain is demonstrated

HEAD `dbc216d5`, same five staged binary hashes as H399/H402. Sikarugir
CX24.0.7, Day/high, 1280x720 window, `-max-threads 2`, `snd_targets 16`.
Hold `DXMT9_METAL_OCCLUSION=0` fixed and change only
`DXMT9_QUERY_COVERED_PENDING=0/1`. This tests the default primitive-count
occlusion lane, not actual GPU visibility.

One warm-up per policy, then OFF/ON followed by ON/OFF. Performance counters,
frame sampling and query observers are disabled in all timing runs. The final
ON counts-only diagnostic is excluded from FPS comparison. All seven runs
exit 0, pass the runner and emit benchmark-owned results. No concurrent build
or second benchmark was launched. This does not prove absence of all OS noise.

`analyze.py` verifies identical requested/final/control configuration hashes
and staged images across all seven runs, absence of perf-counter logs, and
engine average consistency with reciprocal per-frame durations. Requested
configuration SHA256 is
`1f17283bd2ac940ed4c813e5a186f332241d1335b2a1edbd90b7e8a93747205e`.

## Observer-OFF results

| Pair | OFF FPS | ON FPS | ON change |
|---|---:|---:|---:|
| OFF then ON | 30.170450 | 29.836828 | -1.106% |
| ON then OFF | 29.646523 | 29.663712 | +0.058% |
| Mean | 29.908487 | 29.750270 | -0.529% |

Warm-up values 31.804108 / 30.449089 are retained but excluded. Two samples
per policy do not establish statistical equivalence or a small regression.
Pair direction is inconsistent; no FPS benefit is demonstrated. Do not compare
these means to H399 as a code speedup: binaries are unchanged and run conditions
can drift.

## Separate eligibility census

The ON diagnostic uses `DXMT9_QUERY_BACKEND_OBSERVER=counts`, Info logging,
and still no perf/frame counters. Final backend totals:

| Counter | Calls |
|---|---:|
| All / FLUSH | 1,862,487 |
| Covered-Pending fast path | 1,853,808 |
| Authoritative slow path | 8,679 |
| Pending | 1,854,437 |
| Complete | 8,050 |
| Invalid / Failed | 0 / 0 |

Fast-path plus slow-path counts conserve all calls. The candidate handles
99.534% of all calls and 99.966% of Pending outcomes; it is not inactive.
Possible success remains on the locked path. Counts have observer overhead
and provide neither saved CPU time nor contention duration. Poll frequency
alone therefore did not predict end-to-end throughput improvement.

## Safety scope and decision

The existing native `dxmt9-query-covered-pending-spec` and
`dxmt9-query-covered-pending-sync-disabled-spec` pass 2/2 after all wild runs
(`meson test --no-rebuild`; existing test binaries, no rebuild). They cover
the production pure predicate's bounded identity/progress/terminal/marker
truth table and disabling the candidate when asynchronous fences are off.
GPU exact readback and TLA were not rerun in this experiment.

The OFF warm-up and ON first-repeat screenshots contain rendered game scenes
without gross black-screen corruption. Different cameras and single snapshots
are not a same-frame pixel oracle or complete visual/Reset qualification.
Observer-OFF runs do not provide a sampled GPU-error counter.

Keep the runtime default unchanged. The result weakens repeated backend
queue-mutex polling as the dominant explanation of H399's 1.825x renderer
gap under the default occlusion policy; it does not prove zero CPU saving or
generalize to the real-visibility lane. Next prioritize matched-scene visibility
and draw workload, followed by app-supply versus GPU critical-path attribution,
instead of another unpriced polling micro-optimization. H401's approximate
occlusion semantics and H402's scope-publication increase remain open.

No runtime implementation, default, README, commit or push changes.
