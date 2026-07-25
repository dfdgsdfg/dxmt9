---
domain: hidden-backend-storage
workload: 3DMark05 GT2
subcategory: shape
order: 42
title: "Cross-Chunk DCE Removes the R32F Pass but Cannot Wait for GT2 Proof"
date: 2026-07-25
type: mechanism-and-performance-gate
status: rejected-default-accepted-opt-in
source: experiments/output/app-d3d9-3dmark05-gt2-crosschunk-dce-baseline-r1-20260725; experiments/output/app-d3d9-3dmark05-gt2-crosschunk-dce-mechanism-r1-20260725; experiments/output/app-d3d9-3dmark05-gt2-crosschunk-dce-prefix-r2-20260725; experiments/output/app-d3d9-3dmark05-gt2-crosschunk-dce-passcoalesce-prefix-r3-20260725; experiments/output/app-d3d9-3dmark05-gt2-crosschunk-dce-opportunistic-r4-20260725; experiments/output/app-d3d9-3dmark05-gt2-crosschunk-dce-opportunistic-bootstrap-r5-20260725
related: docs/perfomance/overview-3dmark05-gt2.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.41.md; specs/d3d9-renderer/gap.md
---

# Cross-Chunk DCE Removes the R32F Pass but Cannot Wait for GT2 Proof

## Question

[shape.41](hidden-backend-storage-shape.41.md) proved that GT2's final dominant
R32F pass is overwritten by the next frame before either its color or depth
output is read. This experiment asks whether a bounded cross-chunk proof can
remove that pass without stalling the encode lane or weakening queue,
completion, and Present semantics.

## Implemented Proof Boundary

The prototype is the opt-in `dce` framegraph feature:

- It treats a full-subresource Clear as a write and a partial Clear as
  read/write.
- Present is a canonical read, and a pass is dead only when every output is
  proven overwritten before any read or observation.
- It computes a proof-independent prefix from the passcoalesce-optimized
  command order and may encode only that exact prefix early.
- It samples the ready FIFO once after the prefix. An already-ready successor
  supplies the overwrite proof; the queue never waits for a future successor.
- The final fresh proof validates the exact encoded prefix. If it would remove
  an already-encoded pass, that pass is conservatively revived.
- Completion identity and resource lifetime remain attached to the complete
  original source sequence, not to the shortened replay plan.

When no prior successor proof exists, all current resources are used only as a
bootstrap scheduling hint to find the earliest possible proof-dependent
boundary. The hint never authorizes omission.

## GT2 Experiment Sequence

All runs use the same current x86_64 provider, Sikarugir-CX runtime, GT2
selection, frame sampling, no gputrace, and frontmost supervision.

| Run | Policy | Frames | Instantaneous FPS mean | Sampled throughput | Wall p50 / p95 | GPU-CB p50 / p95 |
|---|---|---:|---:|---:|---:|---:|
| baseline r1 | passcoalesce only | `548` | `9.157` | `8.063` | `106.731 / 165.558ms` | `2.886 / 3.440ms` |
| mechanism r1 | hold the whole source for successor proof | `403` | `6.670` | `6.132` | `143.029 / 263.599ms` | `1.046 / 1.193ms` |
| prefix r2 | source-contiguous prefix plus wait | `537` | `9.251` | `8.277` | `107.004 / 225.681ms` | `4.289 / 5.468ms` |
| prefix r3 | passcoalesce-order prefix plus wait | `413` | `6.902` | `6.332` | `142.400 / 248.909ms` | `1.064 / 1.182ms` |
| opportunistic r4 | no wait, no bootstrap hint | `524` | `8.708` | `7.691` | `114.210 / 167.948ms` | `2.981 / 3.560ms` |
| bootstrap r5 | no wait, safe bootstrap prefix | `536` | `8.938` | `7.931` | `110.728 / 165.182ms` | `2.968 / 3.561ms` |

The r2 source-contiguous prefix disabled passcoalesce in the final partial
EncodeSession, so it is not a valid feature-composition performance
comparison. It remains useful as an intermediate mechanism observation only.
The r3 planner fixes that composition by deriving the prefix from the
passcoalesce command permutation and retaining passcoalesce for the suffix.

## Mechanism Proof

The two waiting-prefix runs establish that the final pass is removable when a
successor proof is available:

| Counter | prefix r2 | passcoalesce-prefix r3 |
|---|---:|---:|
| successor proofs selected | `536` | `412` |
| passes dropped | `511` | `389` |
| commands omitted | `74,011` | `56,283` |
| draw calls / Present | `1,278` | `1,293` |
| render passes / Present | `15.71` | `14.77` |

The passcoalesce-only baseline encodes about `1,682` draws and `15.77` render
passes per Present. In r3, GPU-CB p50 falls `2.886 -> 1.064ms`, proving that
the optimizer removes substantial submitted GPU work. But instantaneous FPS
falls `9.157 -> 6.902` (`-24.6%`) and wall p50 grows to `142.400ms`: holding
the cheap prefix until the producer publishes the successor serializes the
producer and encoder and is much more expensive than the removed GPU work.

## Accepted No-Wait Result

The final r5 design exposes the largest safe prefix, samples the ready FIFO
once, and immediately finalizes the current source if no successor is ready:

| Counter | r5 |
|---|---:|
| prefixes / prefix commands | `498 / 111,619` |
| successor selected / fail-open | `1 / 535` |
| ready-proof selection rate | `0.19%` |
| proof resources | `5` |
| passes dropped / commands omitted | `1 / 30` |
| invalid-prefix, missing-pipeline, proof-validation, GPU errors | `0` |

The original no-bootstrap r4 run selected no proofs because no established
proof existed from which to plan a prefix. The bootstrap scheduling hint fixes
that liveness loop, but GT2 still publishes the successor too late for the
no-wait window in `535/536` frames. The accepted design is therefore
performance-safe in policy but effectively a no-op for this workload.

## Correctness Gate

All six runs complete, report zero GPU errors, and show coherent forest,
tree, R32F lighting, and bloom output in the inspected captures. No invalid
prefix, missing pipeline, or proof-validation diagnostic fires. This is
qualitative evidence only; exact device-backed pixel parity remains open and
is not claimed by this experiment.

## Verdict

Cross-chunk DCE is implemented and its removal mechanism is proven, but `dce`
remains opt-in and is not added to the default feature set. GT2 has real dead
GPU work, yet an implementation that waits for its proof loses `24.6%`
instantaneous FPS, while the safe no-wait implementation obtains only one
proof and omits `30` commands in `536` frames. A new Xcode capture is not
justified until another workload or scheduling design produces meaningful
ready-proof volume.

A future design could carry a candidate across Present, but that expands queue
ownership, drawable, and completion semantics beyond this bounded prototype.
It requires a separate architecture and verification gate rather than a
weaker form of the current no-wait policy.
