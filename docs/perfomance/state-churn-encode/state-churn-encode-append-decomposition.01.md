---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 01
title: appendRecordDirect Is A Serialize-Parse-Reserialize Round Trip; Removing 19.6 KB Of Dead Zeroing Buys 0.9%
date: 2026-07-29
type: experiment-run
status: accepted-local-cleanup-below-fps-resolution
source: experiments/output/app-d3d9-3dmark05-gt2-append-phases-r1; experiments/output/app-d3d9-3dmark05-gt2-append-types-r1; experiments/output/app-d3d9-3dmark05-gt2-zeroinit-base; experiments/output/app-d3d9-3dmark05-gt2-zeroinit-cand; experiments/output/app-d3d9-3dmark05-gt2-zeroinit-phases
related: docs/perfomance/present-pacing/present-pacing-post-defselect-cpu-attribution.04.md
---

# appendRecordDirect Is A Serialize-Parse-Reserialize Round Trip; Removing 19.6 KB Of Dead Zeroing Buys 0.9%

**Question / hypothesis.**
[attribution.04](../present-pacing/present-pacing-post-defselect-cpu-attribution.04.md)
left `appendRecordDirect` as the only PE scope worth opening: `14.5%` of GT2's
frame at `2,851 ns` per call over `2,705` calls per present, and `90%` of all PE
recording cost. Where does it go?

**What the record set looks like.** A zero-timing per-type census (counting
only, so it does not add to the bias that dominates short scopes):

| record type | calls / present | share | bytes / call |
|---|---:|---:|---:|
| `drawidx` | `1,669` | `61.3%` | `4,920` |
| `vsconst` | `916` | `33.7%` | `938` |
| `psconst` | `92` | `3.4%` | `50` |
| `draw` | `25` | `0.9%` | `4,888` |
| `applystate` | `10` | `0.4%` | `4,888` |
| `clear` | `9` | `0.3%` | `32` |

**Phase split**, calibrated against the clock pair:

| phase | corrected ns | ms / present | share of append |
|---|---:|---:|---:|
| `resize` scratch | `11` | `0.03` | `0.3%` |
| `build` legacy record | `158` | `0.43` | `4.0%` |
| **`encode`** legacy -> V2 | **`1,464`** | **`3.98`** | **`36.7%`** |
| `flush` chunk | `64,817` | `2.37` | `21.9%` |

The structure this exposes: a record is written into a scratch buffer in the
**legacy** wire format, and `appendLegacyCommandRecordAsV2` then re-parses that
buffer and re-encodes it into V2. The legacy format is a pure intermediate, and
the parse-and-re-encode costs `9x` what building it did.

**The finding inside `encode`.** `appendLegacySparseRecord` opens with five
value-initialized aggregates and a `switch` that uses exactly one:

```cpp
D9CCommandRecordDrawPrimitive primitive{};              // 4,888 B
D9CCommandRecordDrawIndexedPrimitive indexedPrimitive{}; // 4,920 B
D9CCommandRecordDrawPrimitiveUP primitiveUp{};           // 4,904 B
D9CCommandRecordDrawIndexedPrimitiveUP indexedPrimitiveUp{}; // 4,924 B
D9CCommandRecordApplyState apply{};                      // 4,888 B
```

`24,524` bytes zeroed per call, of which `19,636` are never read. They cannot
move into the `switch` branches — `packet`/`indexed` point into them and are
used afterwards — but the `{}` is provably redundant: `loadLegacy` returns
false when the source is smaller than the destination and otherwise `memcpy`s
exactly `sizeof(T)` over it, so every path that reads one has fully overwritten
it and every path that does not has already returned.

**Result of removing it.**

| | before | after | delta |
|---|---:|---:|---:|
| `encode` corrected | `1,464 ns` | `1,174 ns` | **`-19.8%`** |
| GT2 scene fps | `18.78` | `18.69` | `-0.5%` (noise) |

**Verdict.** The mechanism works and is worth keeping — it is a real
`290 ns x 1,704 = 0.49 ms/present` saving, reproduced by the same instrument,
at zero risk. But it is **`0.93%` of a `53.2 ms` frame, below this workload's
FPS resolution**, and must not be recorded as an FPS win.

**The prediction that was wrong, and why.** Removing `80%` of the zeroed bytes
cut `encode` by `20%`, not by most of it. `24 KB` of sequential stack zeroing in
`290 ns` is about `84 GB/s` — plausible for L1/L2-resident writes, and four
times faster than the `~20 GB/s` assumed when predicting `2.5 us`. Bulk
initialization is cheaper than it looks when it never leaves cache; the
[H226 shader case](../shader-codegen/shader-codegen-defselect.01.md) was
expensive because its `4 KB` per *invocation* spilled to device memory, which is
a different regime from `24 KB` per *call* on a hot stack.

**What is left in `encode`.** `1,174 ns`, still `30%` of append: `loadLegacy`'s
`4.9 KB` memcpy plus the V2 section re-encode. Removing the round trip itself
means retiring the legacy intermediate format, which is a structural change, not
a local one.

**Instrument caveat.** The phase timers cost one clock pair each, so a run with
them enabled inflates the parent scope — the same GT2 append read `4,180 ns`
with phases on against `2,851 ns` with them off. Phases are comparable to each
other, not to the parent. This is recorded in the source at the accumulators.

**Related.**
[attribution.04](../present-pacing/present-pacing-post-defselect-cpu-attribution.04.md) ·
[state-churn-encode](index.md) · [present-pacing](../present-pacing/index.md)
