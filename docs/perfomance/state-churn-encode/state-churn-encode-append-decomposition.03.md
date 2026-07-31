---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 03
title: After The Round Trip Is Gone, Append Splits Three Ways And The Chunk Flush Leads
date: 2026-07-31
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05-gt2-decim64-postremoval; experiments/output/app-d3d9-3dmark05-gt2-decim16-postremoval
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.02.md; docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.01.md; docs/perfomance/present-pacing/present-pacing-post-defselect-cpu-attribution.04.md
---

# After The Round Trip Is Gone, Append Splits Three Ways And The Chunk Flush Leads

**Question / hypothesis.**
[append-decomposition.01](state-churn-encode-append-decomposition.01.md) split
`appendRecordDirect` and named the serialize-parse-reserialize round trip as the
only remaining move.
[.02](state-churn-encode-append-decomposition.02.md) measured the structural
change that removed it. But the split that *named* the target was taken before
that removal, so the attribution pointing at what to do next is now stale — the
same staleness just corrected in the root baseline table. Where is PE cost now?

**Method.** `DXMT9_PE_STATS_DECIMATION` at `N=64` and `N=16` with
`DXMT_LOG_LEVEL=info`, GT2, `perf` profile, no gputrace, `--keep-frontmost`, at
`8364aff2`. Per-scope cost is `sampled_ms * N / presents` **minus** the shared
`null_scope` calibration, per
`agents/rules/environment_variables_bridge.rules.md`. The instrument measured
itself at `187` / `180 ns` per sample in the two runs, consistent with the
`186 ns` attribution.04 established.

**Result — the four top-level scopes, calibrated.** The two decimation rates
agree within `2.2%`.

| Scope | events/present | corrected ns | **ms/present** (`N=64`) | (`N=16`) |
|---|---:|---:|---:|---:|
| `appendRecordDirect` | `2,707` | `2,579` | **`6.98`** | `7.07` |
| `buildSparseStateV2` | `1,695` | `360` | `0.61` | `0.55` |
| `touchConstShadow` | `21,588` | `18` | `0.39` | `0.45` |
| `flushConstShadow` | `10,211` | `9` | `0.09` | `0.18` |
| **total** | | | **`8.07`** | `8.25` |

Against a `53.4 ms` GT2 frame, PE recording is **`15.1%`**, down from
attribution.04's `16.1%`, and `appendRecordDirect` is still `86%` of it.

**Result — inside append.** The `encode` and `flush` phase timers survived the
migration and were re-pointed at the sparse emitters, so this is the same axis
as .01. Phases are recorded only on sampled appends, so
`append_flush_sampled / append_encode_sampled` = `738 / 48,218` is the fraction
of appends that flush: `1.53%`, or `41.4` flushes per present — which is exactly
`2,707` appends over the default `DXMT9_PE_CHUNK_MAX_RECORDS = 64`.

| Component | per call | per present | share of append |
|---|---:|---:|---:|
| chunk `flush` | `65,505 ns` × `41.4`/present | **`2.71 ms`** | **`38.8%`** |
| section `encode` | `890 ns` × `2,707`/present | `2.41 ms` | `34.5%` |
| envelope / remainder | | `1.86 ms` | `26.7%` |

**Verdict.** The round trip's removal shows up where predicted and nowhere else:

| | pre (attribution.04 / .01) | post | change |
|---|---:|---:|---:|
| `appendRecordDirect` per call | `2,851 ns` | `2,579 ns` | `-9.5%` |
| `encode` phase per call | `1,174 ns` | `890 ns` | `-24%` |
| `flush` phase per call | `64,817 ns` | `65,505 ns` | `+1%` (unchanged) |
| PE recording total | `8.59 ms` | `8.07 ms` | `-6.1%` |

`encode` fell by roughly the round trip's share and `flush` did not move at all,
which is what a correct structural change looks like from the outside.

**The frontier moved, and it moved somewhere new.** Before, `encode` was the
single dominant component and had an obvious structural fix. Now append splits
close to three ways, and the largest single piece is the **chunk flush** —
`2.71 ms/present`, `5.1%` of the GT2 frame, `41.4` bridge commits per present at
`65.5 us` each.

That is immediately testable with no code: `DXMT9_PE_CHUNK_MAX_RECORDS` sets the
cap that produces `41.4`. **The hypothesis is not free, which is why it needs
measuring rather than assuming** — a flush is a bridge commit whose synchronous
half (wire validation, import, handle marking) scales with the chunk it
commits, so a larger chunk may move that cost rather than remove it. A coarser
chunk also coarsens the producer-to-worker handoff, though the offload worker
idling `~39 ms/present` leaves room for that.

> **Tested and rejected the same day**, in
> [append-decomposition.04](state-churn-encode-append-decomposition.04.md):
> raising the cap to `256` cuts flushes `2.9x` (`47.4 -> 16.1`/present) and
> costs `-4.0%` scene fps, spreads disjoint. The caution above was the right
> one — the bill moves rather than leaves. The `2.71 ms` is real but is not
> addressable by the cap; a flush-side win would have to come from decomposing
> what the synchronous half of a commit does, which has never been measured.

**Scope.** One GT2 run per decimation rate; the agreement between them is
evidence that the extrapolation is stable, not that it is absolutely accurate.
The `pre` column is quoted from attribution.04 and .01, whose runs were taken at
a nearby but not identical commit, so the deltas are sound in direction and
approximate in magnitude. Nothing here was measured on GT1, GT3, or SFIV.

**Related.**
[append-decomposition.02](state-churn-encode-append-decomposition.02.md) ·
[append-decomposition.01](state-churn-encode-append-decomposition.01.md) ·
[attribution.04](../present-pacing/present-pacing-post-defselect-cpu-attribution.04.md) ·
[state-churn-encode](index.md)
