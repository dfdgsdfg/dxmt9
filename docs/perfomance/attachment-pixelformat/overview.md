---
domain: attachment-pixelformat
workload: 3DMark05 GT1
title: "Attachment / Pixel-Format — RT PixelFormatView suppression and lossless-compression hints - Current Overview"
type: domain-overview
status: current
updated: 2026-07-08
source: docs/perfomance/attachment-pixelformat/log.md; docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/attachment-pixelformat/index.md; docs/perfomance/attachment-pixelformat/log.md
---

# Attachment / Pixel-Format — RT PixelFormatView suppression and lossless-compression hints - Current Overview

> Current, compact view for this performance domain. Historical detail from the former
> top-level `attachment-pixelformat.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

## Scope

This domain owns every attempt to chase Xcode's **lossless-compression**
insight: a render target carrying Metal `PixelFormatView` usage (and the
swizzled shader-read view dxmt9 attaches to honor D3D9's expanded read contract)
is excluded from Metal compression even when the captured frame uses it as a
render target only. The experiments suppress that view for R32F RTs and for
X8R8G8B8/X8B8G8R8 RTs, add attachment-metadata and X8 sampler-binding
instrumentation to map the hint to concrete hot-encoder RT shapes, and add a
shader-side X8 alpha-fill companion so an X8 view could be dropped safely. The
question each asks: *does pixel-format/attachment shape own the ~1.6 GiB VS
buffer-write bucket?* The answer is consistently **no** — these probes move the
texture-write bucket, not the VS-write owner.

## Latest Conclusions

> **Every row below cites a leaf now marked `outdated: retired-journal`.** None of
> these verdicts can be re-checked today; they are kept because they record which
> attachment/pixel-format lanes were already tried and rejected.

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H1 | R32F RT `PixelFormatView`/shader-read view owns GT1 GPU cost | rejected (texture-write `-49.66%`, VS write unchanged) | [attachment-pixelformat-r32f.01](attachment-pixelformat-r32f.01.md) |
| H2 | Per-encoder attachment metadata can map Xcode's `fmt2` compression hint to hot RT shapes | tooling (maps to X8R8G8B8 RT0 in enc0/2; `usage=0x2` is not an unsampled proof) | [attachment-pixelformat-metadata.01](attachment-pixelformat-metadata.01.md) |
| H3 | Allocation-wide X8 RT view suppression removes the `fmt2` hint and moves cost | rejected (too coarse; run incomplete; X8 rows mostly textured) | [attachment-pixelformat-x8.01](attachment-pixelformat-x8.01.md) |
| H4 | The hot GT1 encoder actually samples X8 RT aliases (so suppression matters there) | tooling/refuted (hot enc `60/2` samples 0 X8 RT; sampling only in post passes) | [attachment-pixelformat-x8.02](attachment-pixelformat-x8.02.md) |
| H5 | Shader X8 alpha-fill + view suppression moves the texture/store or VS-write bucket | rejected (hot passes 0 alpha-fill; top-3 VS write unchanged `~1627.25MiB`) | [attachment-pixelformat-x8.03](attachment-pixelformat-x8.03.md) |

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

> 5 of the 5 leaves listed below are marked `outdated:` and open with a banner naming the ground. They are history, not re-checkable evidence.

- [attachment-pixelformat-x8.03 - X8 Shader Alpha-Fill Companion Probe](attachment-pixelformat-x8.03.md)
- [attachment-pixelformat-x8.02 - X8 Sampler Binding Attribution](attachment-pixelformat-x8.02.md)
- [attachment-pixelformat-x8.01 - Broad X8 Suppression Attempt](attachment-pixelformat-x8.01.md)
- [attachment-pixelformat-r32f.01 - R32F RT PixelFormatView Suppression Probe](attachment-pixelformat-r32f.01.md)
- [attachment-pixelformat-metadata.01 - Attachment Metadata Probe](attachment-pixelformat-metadata.01.md)
