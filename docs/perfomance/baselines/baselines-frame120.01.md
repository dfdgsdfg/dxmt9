---
domain: baselines
workload: 3DMark05 GT1
subcategory: frame120
order: 01
title: Frame 120 Xcode Counter Snapshot
date: 2026-05-31
type: measurement
status: accepted
source: specs/perfomance.plan.md#L2042-L2091
---

# Frame 120 Xcode Counter Snapshot

**Question / hypothesis.** What does the original GPU-limited 3DMark05 GT1 frame
actually cost, and which counters dominate? This is the capture that first
defined the bottleneck shape the whole investigation chases.

**Method.** `tmp/frame120 Counters.csv` is the original Xcode encoder-counter
export, copied unmodified to
`traces/app-d3d9-3dmark05-20260531-205116-gt1/analysis/frame120-counters-xcode.csv`
with a sortable reduced version
`frame120-counters-summary.csv`. Xcode Performance view: 4 command buffers,
10 render encoders, 387 draw calls, captured frame `frame120`.

**Result.**
- Total GPU time `33.611ms` for the captured frame (~`29.8 FPS` if representative + GPU-limited).
- Top 3 render encoders: `33.075ms` / `98.40%` of the frame.
- The same color/depth pair `rt=0x30000460000000c,depth=0x300000100000001`
  appears in two passes: `24.643ms` / `73.32%`.
- Rank 1 `cb_seq_476 RenderPass[rt=0x30000460000000c,depth=0x300000100000001]`:
  `18.929ms` / `56.32%`, 179 draws; limiter LLC `35.76%`, MMU `34.03%`,
  Buffer Write `20.85%`, Buffer Read `15.79%`; ALU only `5.75%`, Texture Read `2.15%`;
  Write `1001.8MiB`, Buffer Write `981.2MiB`.
- Rank 2 `cb_seq_475 RenderPass[rt=0x300003d0000000b,depth=0x300000100000004]`:
  `8.431ms` / `25.08%`; LLC `31.29%`, MMU `24.11%`, Buffer Write `22.08%`;
  Write `444.6MiB`, Buffer Write `421.4MiB`.
- Rank 3 `cb_seq_475 RenderPass[rt=0x30000460000000c,depth=0x300000100000001]`:
  `5.714ms` / `17.00%`; LLC `34.28%`, Buffer Write `18.21%`, MMU `13.36%`;
  Write `231.3MiB`, Buffer Write `225.4MiB`.
- Present + post passes: `0.537ms` / `1.60%`, not material.

**Verdict.** Accepted as the original bottleneck-shape capture. The frame is
**not** ALU- or texture-bound; the dominant counters are LLC, MMU, and
buffer/device write traffic, concentrated in three render encoders, two of which
re-enter the same RT/depth pair. This is the first evidence pointing at hidden
backend write pressure rather than shader ALU or sampling — the seed of the
[hidden-backend-storage](../hidden-backend-storage.md) thesis.

> Note: the task brief paired this leaf with `35.456ms / 98.25%`; those are the
> later frame60 normal-source refresh numbers (specs L2143). The faithful
> frame120 snapshot values are `33.611ms` / top-3 `98.40%`, matching the
> `24.643ms (73.3%)` re-entry detail at source L2068.

**Related.** [baselines](../baselines.md) · [overview-3dmark05-gt1](../overview-3dmark05-gt1.md) · [hidden-backend-storage](../hidden-backend-storage.md) ·
[render-pass-store](../render-pass-store.md) (same RT/depth pair re-entry, `73.32%`) ·
[baselines-runlevel.01](baselines-runlevel.01.md) (run-level context).
