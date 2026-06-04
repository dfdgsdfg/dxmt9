---
domain: primitive-reorder-diagnostics
subcategory: reverse
order: 03
title: Nonopaque Reverse Subset
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L9519-L9663
---

# Nonopaque Reverse Subset

**Question / hypothesis.** Is the full-reverse win
([[primitive-reorder-diagnostics-reverse.01]]) owned by nonopaque visibility
work? The probe is the complement of the opaque subset
([[primitive-reorder-diagnostics-reverse.02]]): it reverses triangle order only
for blended / depth-write-off / scissored / visibility-sensitive draws and
leaves opaque depth-writing draws in normal order.

**Method.** `run_3dmark05_perf_probe.sh --suffix reverse-nonopaque-indexed-triangles-gputrace-r1
--frame 60 --probe-reverse-nonopaque-indexed-triangles --measure-index-reuse
--top 4 --hot-gpu-share 95 --baseline-joined <measure-index-cache>
--require-xcode-counter-coverage --require-dxmt-join-coverage
--require-top-pso-attribution --min-top-pso-samples-per-draw 0.90
--min-top-dxmt-joined-fraction 1.0`. No-gputrace looked promising
(`gpu_command_buffer_time_ms` `-12.86%`) but the Xcode counter join is
authoritative.

**Result.** GPU `34.391 -> 35.750ms` (`+3.95%`); hot/top VS buffer write
`1472.747 -> 1481.228MiB` (`+0.58%`); hot/top unexplained write `+0.35%`; hot/top
draws `+9.14%`, dxmt vertices/triangles `+3.94%`. Probe 371 draws / 417 skipped,
`2.99MiB` reorder bytes. Row trade-off: `60/3`/`60/1` (opaque, skipped) improve
indirectly while targeted `60/4` worsens `370.276 -> 448.060MiB VS write
(+21.01%)` and `60/0` worsens `+12.43%`.

**Verdict.** Rejected. Broad nonopaque reversal regresses GPU and VS write; the
full-reverse win is not "nonopaque rows only" either. The probe trades small
wins in some rows for larger losses in others and drifts frame shape (fails the
5% gate, [[primitive-reorder-diagnostics-reverse.04]]). Useful only as a
classifier: order moves the hidden bucket, but no broad subset is clean.

**Related.** [[primitive-reorder-diagnostics]] · prev: [[primitive-reorder-diagnostics-reverse.02]]
· next: [[primitive-reorder-diagnostics-reverse.04]] · [[hidden-backend-storage]]
· [[index-reuse-measurement]] (state-class attribution motivated by these row trades).
