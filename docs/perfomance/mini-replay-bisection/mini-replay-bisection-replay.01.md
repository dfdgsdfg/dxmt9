---
domain: mini-replay-bisection
workload: 3DMark05 GT1
subcategory: replay
order: 01
title: Full 16-Draw Multi-Shader Mini-Replay
date: undated
type: experiment-run
status: inconclusive
source: specs/perfomance.plan.md#L14321-L14459
---

# Full 16-Draw Multi-Shader Mini-Replay

**Question / hypothesis.** Do the captured shader + stream0/stream1/index/cbuf
payloads, replayed standalone, reproduce the original `60/2` hot encoder's
~1 GiB `VS Buffer Device Memory Bytes Written` bucket — proving the payloads alone
are sufficient to drive the hidden vertex-stage write pressure?

**Method.** Multi-PSO runner ([mini-replay-bisection-harness.02](mini-replay-bisection-harness.02.md)) on the
16-draw `60/2` screen-blend manifest (draw `71..86`, 6 VS/PS pairs),
`--primitive-order original --draw-order original`, captured with
`--capture-path … mini-replay-full16.gputrace`, then Xcode encoder-counter export.

**Result.** 1 render encoder, 16 draws, 86,466 vertices, GPU `3.710ms`. Counter
comparison vs original `60/2`:

| Metric | Original `60/2` | Mini full16 | Mini / orig |
|---|---:|---:|---:|
| GPU time | 20.327ms | 3.710ms | 0.183x |
| VS buffer write | 981.171MiB | **31.974MiB** | 0.033x |
| VS invocations | 642,001 | 54,104 | 0.084x |
| VS buffer / VS inv | 1602.5B | 619.7B | 0.387x |
| FS invocations | 3,296,064 | 22,057,376 | 6.692x |
| Fragments / primitive | 36.0 | 738.0 | 20.5x |
| Vertex stage time | 96.06% | 26.11% | 0.272x |

**Verdict.** INCONCLUSIVE. The replay reproduced the same *class* of VS-write
traffic (almost entirely `VS Buffer Device Memory Bytes Written`) but **not its
scale or shape**: the original is vertex-stage dominated (96.06%, 1602.5B/VS inv)
while the standalone slice is fragment/overdraw dominated (22.1M FS inv, 738
frags/prim, 26.11% vertex). Payloads alone are insufficient — the next replay must
preserve more original pass context, not add more of the same draws.

**Related.** [mini-replay-bisection](index.md) · [mini-replay-bisection-harness.02](mini-replay-bisection-harness.02.md) ·
[mini-replay-bisection-replay.02](mini-replay-bisection-replay.02.md) (scissor pollution found next) ·
[hidden-backend-storage](../hidden-backend-storage/index.md) · [overview-3dmark05-gt1](../overview-3dmark05-gt1.md)
