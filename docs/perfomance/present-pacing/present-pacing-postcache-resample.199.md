---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: producer-attribution
order: 199
title: Post-Cache Producer Re-Sample - The Wall Is Now The Guest Blob
date: 2026-07-08
type: xctrace-cpu
status: accepted-attribution
source: traces/app-d3d9-3dmark05-cpu-attrib-postcache-r1-20260708/analysis/time-profile.xml; experiments/output/app-d3d9-3dmark05-cpu-attrib-postcache-r1-20260708/result.json; docs/perfomance/present-pacing/present-pacing-producer-sampling-attribution.196.md
related: docs/perfomance/present-pacing/index.md; docs/perfomance/present-pacing/present-pacing-readonly-cache-stacking.198.md
---

# Present-Pacing H212 - Post-cache producer re-sample

## Question

With the readonly cache promoted (H211), what does the H196 sampling method
say the producer runs now, and did the wow64 bucket shrink in proportion to
the `-96%` bridge-crossing reduction?

## Run

H196/r4 method on current HEAD: promoted trio live (`2,220` presents, cache
`54.9` locks/present, offload replay `9.11ms`, `169.0` reordered hits),
parallel 25s xctrace window, producer auto-selected (`0x1a056ba`,
`25.1s/25s` running, ~`49ms/present` producer busy at `~510` window
presents).

## Decomposition vs H196

| Owner | H196 pre-cache | H212 post-cache | ms/present now |
|---|---:|---:|---:|
| Rosetta-translated guest blob (game x86 + 32-bit PE DLLs) | `70.6%` | `73.5%` | `~36` |
| wow64 / win64-PE layer | `21.7%` | `19.8%` | `~9.8` |
| `wine.real` | `4.2%` | `4.4%` | `~2.1` |
| `winemetal.so` unix | `2.2%` | `1.4%` | `~0.7` |

## Findings

1. **The wow64 layer did not collapse with the bridge storm.** Crossings fell
   `1,478.7 -> 54.9/present` (`-96%`), but the bucket only moved
   `12.6 -> 9.8ms/present`. The winemetal-adjacent slice fell proportionally
   (`guest64-pe <- winemetal.so` pair `5.9% -> 4.1%`, `~3.4 -> ~2.1ms`), and
   the bucket's top frames are unchanged — so most of the remaining wow64/PE64
   time is the game's own win32/syscall traffic through the emulation layer,
   not dxmt9 bridge calls. Not reachable by dxmt9.
2. **dxmt9-named producer cost is now ~3ms/present** (`0.7` unix +
   `~2.1` wow64-over-winemetal), out of a `~49ms` producer wall.
3. **The wall is the guest blob at ~36ms/present** — game x86 code plus our
   32-bit PE `d3d9.dll` recording path, host-side unsplittable. Its split is
   the only remaining question that decides whether dxmt9 has another CPU
   lever in GT1.

## Next owner

Guest-side decimated PE stats (time every Nth event to avoid H192's `34%`
stats perturbation) to size the PE `d3d9.dll` share inside the `36ms` blob:

- PE share `~8-10ms` → Plan B (inline const deltas in draw packets,
  eliminating the `645` const records + `4,206` setter shells per present)
  regains a `+15-20%` ceiling and should be specced.
- PE share `~2-3ms` → dxmt9's GT1 CPU frontier is exhausted; pivot to
  generalizing the promoted trio on other workloads (SFIV), GPU-bound-scene
  locality work, or the startup flake (now with the
  `mutex lock failed` signature from H196/r3).
