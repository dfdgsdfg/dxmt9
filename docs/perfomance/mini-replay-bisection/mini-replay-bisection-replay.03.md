---
domain: mini-replay-bisection
workload: 3DMark05 GT1
subcategory: replay
order: 03
title: Wider Encoder2 Payload Capture
date: undated
type: experiment-run
status: accepted
source: specs/perfomance.plan.md#L14783-L14878
---

# Wider Encoder2 Payload Capture

**Question / hypothesis.** If depth/scissor are not the missing owner
([[mini-replay-bisection-depth.01]]), does replaying the **whole** `60/2`
encoder2 geometry/material sequence reproduce the original hot-row vertex-stage
write dominance? This tests whether the wider draw sequence and the backend state
it induces is the missing condition.

**Method.** Dumped the full encoder2 slice: `seq=60`, `encoder=2`, encoder-local
draws `0..112` (113 draws, ordinals `42590..42702`). Built
`frame60-mini-replay-manifest-encoder2-113.json` (a manifest-builder sort bug that
placed `encoder_draw_index=0` last was fixed; `tests/scripts/test_build_3dmark05_mini_replay_manifest.py`
locks the `0,1` ordering). Ran with `--primitive-order original --draw-order
original` and the real `frame60-2-depth.bin` `--depth-input`, then Xcode counters.

**Result.** No-capture smoke: `mini replay draws=113 repeat=1`. Xcode single
render-encoder row vs current-head hot rows:

| Case | GPU ms | VS buffer write | VS invocations | VS B / VS inv | Primitives | Vertex stage |
|---|---:|---:|---:|---:|---:|---:|
| current-head `60/2` | 20.327 | 981.171MiB | 642,001 | 1602.5B | 389,376 | 96.06% |
| **113-draw replay** | **18.115** | **1090.901MiB** | 668,929 | **1710.0B** | 390,345 | 98.93% |
| sorted-row control `60/2` | 7.925 | 281.955MiB | 667,944 | 442.6B | 366,197 | 90.61% |

**Verdict.** ACCEPTED. The 113-draw replay reproduces the same vertex-stage
memory-pressure class as the hot row — GPU close to original (18.115 vs 20.327ms)
and VS write even higher (1090.901 vs 981.171 MiB). The missing condition was the
wider encoder2 sequence, not depth/scissor. The sorted-row control (similar VS inv
but only 442.6B/VS inv) proves raw vertex count is insufficient. Next: bisect this
113-draw replay ([[mini-replay-bisection-bisect.01]]).

**Related.** [[mini-replay-bisection]] · [[mini-replay-bisection-depth.01]] ·
[[mini-replay-bisection-bisect.01]] · [[hidden-backend-storage]] ·
[[tvb-mechanism-proof]] · [[overview]]
