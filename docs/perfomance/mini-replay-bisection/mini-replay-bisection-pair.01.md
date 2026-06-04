---
domain: mini-replay-bisection
workload: 3DMark05 GT1
subcategory: pair
order: 01
title: Pair-Local Mini-Replay Captures
date: undated
type: experiment-run
status: accepted
source: specs/perfomance.plan.md#L15015-L15110
---

# Pair-Local Mini-Replay Captures

**Question / hypothesis.** Inside the hot `window-014-027`
([[mini-replay-bisection-bisect.01]]), is the VS-write amplification owned by
specific shader-pair / large-indexed-draw classes, and is it per-draw additive
rather than a cross-draw transition artifact? Is it alpha/scissor-shaped or
geometry/locality-shaped?

**Method.** The manifest builder gained `--encoder-draw-indices` to capture
non-contiguous encoder-local draws without widening the geometry gate (unit:
`tests/scripts/test_build_3dmark05_mini_replay_manifest.py`). Captured pair
`0xfea7cb/0xa0910f` draws `[14,15,18,19,21]`, pair `0xdee2a2/0x2f2090` draws
`[26,27]`, the three largest indexed draws `[15,19,27]`, and singletons `15`/`27`,
each with Xcode counters.

**Result.**

| Case | Draws | Alpha/scissor | GPU ms | VS buffer write | VS inv | VS B / VS inv | Named tiled total |
|---|---:|---|---:|---:|---:|---:|---:|
| `window-014-027` | 14 | 4/4 | 5.753 | 347.914MiB | 90,614 | 4026.0B | 2.938MiB |
| `fea7/a091` | 5 | 0/0 | 3.709 | 254.140MiB | 61,097 | 4361.7B | 2.375MiB |
| `dee2/2f20` | 2 | 2/2 | 1.525 | 93.056MiB | 23,673 | 4121.8B | 0.500MiB |
| `large-15-19-27` | 3 | 1/1 | 3.997 | 273.554MiB | 66,644 | 4304.1B | 2.375MiB |
| `single-15` | 1 | 0/0 | 1.574 | 95.517MiB | 22,972 | 4360.0B | 0.688MiB |
| `single-27` | 1 | 1/1 | 1.439 | 81.328MiB | 20,700 | 4119.7B | 0.375MiB |

**Verdict.** ACCEPTED — per-draw geometry/shader-pair amplification. The two
pairs explain **347.196 MiB = 99.8%** of the window VS write with only 7/14 draws.
`fea7/a091` has no alpha/scissor yet is worst per invocation, so this is **not**
an alpha/scissor signature. Single-draw additivity holds within 0.44%
(`2·single-15 + single-27` predicts 66,644 VS inv exactly, 272.362 vs 273.554 MiB).
Named tiled counters stay tiny (≤2.375 MiB) vs the VS write bucket — the measured
cost is hidden Apple vertex/tiler/backend storage below visible VSOut and below
named tiled counters. An actual-read VSOut trim A/B moved VS write only -0.01%,
rejecting pair-liveness as root cause. This per-draw class is what
[[tvb-mechanism-proof]] then proved is `VS invocations × per-vertex VSOut bytes`.

**Related.** [[mini-replay-bisection]] · [[mini-replay-bisection-bisect.01]] ·
[[hidden-backend-storage]] · [[tvb-mechanism-proof]] · [[vsout-layout]] ·
[[index-cache-locality]] · [[primitive-reorder-diagnostics]] · [[overview]]
