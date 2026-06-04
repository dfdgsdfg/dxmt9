---
domain: mini-replay-bisection
workload: 3DMark05 GT1
subcategory: bisect
order: 01
title: Encoder2 Bisection Analysis
date: undated
type: experiment-run
status: accepted
source: specs/perfomance.plan.md#L14879-L15014
---

# Encoder2 Bisection Analysis

**Question / hypothesis.** Within the reproduced 113-draw encoder2 replay
([[mini-replay-bisection-replay.03]]), is the ~1 GiB VS-write pressure owned by a
single late draw/state transition, or is it additive across independent draw
windows? Binary-search the draw range to localize the hot region.

**Method.** Filtered the verified 113-draw manifest's `draws` array into prefix /
window candidates (preserving shader paths, geometry payloads, draw order, real
`frame60-2-depth.bin` depth input). No-capture smoke passed for every candidate;
each was captured with Xcode encoder counters under
`analysis/bisection/`.

**Result.**

| Case | Draws | GPU ms | VS buffer write | VS inv | VS B / VS inv |
|---|---:|---:|---:|---:|---:|
| full replay | 113 | 18.115 | 1090.901MiB | 668,929 | 1710.0B |
| `prefix-000-013` | 14 | 0.245 | **0.000MiB** | 87,425 | 0.0B |
| `window-014-027` | 14 | 5.753 | 347.914MiB | 90,614 | **4026.0B** |
| `window-028-055` | 28 | 5.820 | 315.029MiB | 198,744 | 1662.1B |
| `window-056-083` | 28 | 4.123 | 221.271MiB | 150,907 | 1537.5B |
| `prefix-000-055` | 56 | 11.401 | 663.657MiB | 376,783 | 1846.9B |
| `prefix-000-083` | 84 | 15.147 | 884.870MiB | 527,690 | 1758.3B |

**Verdict.** ACCEPTED — additive, independent sources. `0..13` is cold (0 B);
`14..27` is the first hot region and has the worst density (4026.0B/VS inv). The
splits are additive: `0..55`+`56..83` = 884.928 vs 884.870 MiB measured;
`0..27`+`28..55` = 662.961 vs 663.657 MiB. This rules out a single
prefix-transition-state explanation — the reproduced class is per-window
vertex-stage backend write amplification. (Also corrected stale manifest metadata:
hot pairs genuinely read high texcoords, so a `position,fogFactor,texcoord0` trim
would corrupt the replay — see [[vsout-layout]].) Drill into `14..27` next.

**Related.** [[mini-replay-bisection]] · [[mini-replay-bisection-replay.03]] ·
[[mini-replay-bisection-pair.01]] · [[hidden-backend-storage]] ·
[[index-cache-locality]] · [[vsout-layout]] · [[overview-3dmark05-gt1]]
