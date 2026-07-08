---
domain: backend-shape-classifiers
workload: 3DMark05 GT1
subcategory: scissor
order: 02
title: Scissor State Classifier
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L8210-L8299
---

# Scissor State Classifier

**Question / hypothesis.** Full Xcode-counter capture: was the earlier
force-fragment movement in row `60/2` caused by scissor state itself, and does
scissor own the hidden VS-write bucket?

**Method.** `DXMT_DISABLE_SCISSOR=1` (wrapper `--disable-scissor`), `--frame 60
--encoder-breakdown-seq 60 --dump-shaders`, finalized vs
`current-normal-gputrace-r1` with full coverage/PSO/shader-dump gates
(`--min-top-pso-samples-per-draw 0.90 --min-top-dxmt-joined-fraction 1.0`).
Shader dump matched `9/9` VS and `9/9` PS rows.

**Result.**

| Metric | Baseline | Disable scissor | Delta |
|---|---:|---:|---:|
| Total GPU time | `35.456ms` | `36.921ms` | `+4.13%` |
| Top-3 GPU time | `34.837ms` | `36.295ms` | `+4.19%` |
| Top-3 VS buffer write | `1627.240MiB` | `1627.315MiB` | `+0.00%` |
| Top-3 unexplained write | `1627.596MiB` | `1627.581MiB` | `-0.00%` |
| Top-3 VS B / VSOut | `7.868x` | `7.869x` | `+0.00%` |

Hot rows `60/2 +5.70%`, `60/1 +4.44%`, `60/0 -1.48%` GPU; VS invocations and VSOut
key (`0xfff`) fixed across all three.

**Verdict.** Rejected. Disabling scissor does not reduce the VS-write bucket or
the hidden backend estimate — it only regresses GPU time within
frame-to-frame/backend variation. The earlier `60/2` force-fragment movement is
not explained by scissor alone. Scissor is not the owner of the hidden
vertex/tiler/parameter storage.

**Related.** [backend-shape-classifiers](../backend-shape-classifiers.md) · expands [backend-shape-classifiers-scissor.01](backend-shape-classifiers-scissor.01.md) · confirms [hidden-backend-storage](../hidden-backend-storage.md) · related [backend-shape-classifiers-cull.02](backend-shape-classifiers-cull.02.md) (same capture campaign).
