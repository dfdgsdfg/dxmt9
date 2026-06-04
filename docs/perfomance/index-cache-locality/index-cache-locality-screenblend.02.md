---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: screenblend
order: 02
title: Layout-Stride Screen-Blend Cache No-Gputrace Scout
date: 2026-06-04
type: scout
status: inconclusive
source: specs/perfomance.plan.md#L1293-L1376
---

# Layout-Stride Screen-Blend Cache No-Gputrace Scout

**Question / hypothesis.** The remaining hot row `50/2` is entirely depth-read and
contains the screen-blend/scissor/textured subset. Can the strict screen-blend
index-cache predicate generate real reordered-cache hits inside `50/2` before
spending Xcode time?

**Method.** `run_3dmark05_perf_probe.sh --suffix layoutstride-screenblend-index-cache-frame50-nogputrace-r1
--frame 50 --encoder-breakdown-seq 50 --no-gputrace --optimize-screen-blend-index-cache
--optimize-screen-blend-index-cache-min-gain-pct 10 --measure-index-cache-opt-candidate
--timeout 240` (`DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_CACHE=1`). Timeout-finalized pass,
`present_encoded=1440`.

**Result.** Reordered-cache hits isolate to `50/2` (`50/0`, `50/1` show candidate
telemetry only — the screen-blend path does not target opaque rows). Row `50/2`:
`162` candidate draws, `103` reordered lookups, `66` hits, `37` rejected; actual hit
LRU32 `328,856→241,780` (`-87,076`); full candidate ceiling LRU32 `675,973→500,805`
(`-175,168`). All hits are alpha-blended `InvDestColor + One + Add`, depth test on,
depth write off, no alpha-test/clip/stencil; largest groups `33` draws / `25` hits each.

**Verdict.** Inconclusive / promising diagnostic candidate. The accepted screen-blend
subset can reduce effective LRU32 inside the dominant `50/2` row, but the path is
explicitly profiling-only — screen-blend output is destination-dependent and prior
same-input FS probes showed small image differences. Any Xcode capture must be read
as a row-`50/2` diagnostic unless paired with a semantic image proof.

**Related.** [[index-cache-locality]] · prev: [[index-cache-locality-screenblend.01]]
· next: [[index-cache-locality-screenblend.03]] · [[index-cache-locality-opaque.03]]
(50/2 left untouched there) · [[hidden-backend-storage]].
