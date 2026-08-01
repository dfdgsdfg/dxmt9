---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 16
title: The Setup Prologue Is Half A Per-Draw Metal Debug Group — Worth 2.7 ms And Zero FPS
date: 2026-08-01
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05-nodebuggroup; experiments/output/app-d3d9-3dmark05-encode-sites; experiments/output/app-d3d9-3dmark05-dbggroup-gated-default; experiments/output/app-d3d9-3dmark05-dbggroup-forced-on; experiments/output/app-d3d9-3dmark05-dbggroup-capture-env
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.15.md
---

# The Setup Prologue Is Half A Per-Draw Metal Debug Group — Worth 2.7 ms And Zero FPS

**Question / hypothesis.**
[.15](state-churn-encode-append-decomposition.15.md) left exactly one lead: the
`setup` phase, `4.26 ms/present` of which `2.97` has no counter on it, running
before any binding work. Asked of it the question
[.09](state-churn-encode-append-decomposition.09.md) asked of the SWVP probe —
is anything computed and then thrown away?

**Result — yes, and it is the same shape.** Every draw, unconditionally:

```cpp
std::optional<DebugGroupScope> drawDebugGroup;
if (!suppressRecordedMetalCalls(ctx)) {
  drawDebugGroup.emplace(
      WMT::CommandEncoder{encoder.handle},
      makeLabelStringFmt("Draw[seq=%llu,prim=%u]", seqId, drawDebugPrimCount));
}
```

That is a `vsnprintf`, a `WMT::String::string()` allocation that crosses the
bridge, a `pushDebugGroup`, and a matching `popDebugGroup` at function exit —
**one allocation and three bridge crossings per draw**, `~1,690` draws/present.
The suppression flag is set only by the mini-replay draw recorder, so on a normal
run it never fires.

The labels exist so a `.gputrace` opened in Xcode shows a nested
`RenderPass[...] → Draw[idx,tri]` narrative
(`agents/rules/metal_debugging.rules.md` §2). Nothing reads them at runtime.

**Measured**, by disabling the block and re-running GT2:

| | debug group on | off | delta |
|---|---:|---:|---:|
| `setup` phase | `4.261` | `2.762` | **`-1.499`** |
| `encode_draw` | `17.471` | `14.922` | **`-2.549`** |
| encode stage wall | `24.740` | `22.011` | **`-2.729`** |
| **frame** | **`40.15 ms`** | **`40.16 ms`** | **`+0.01`** |
| **scene fps** | **`24.91`** | **`24.90`** | **`-0.01`** |

It is **half the `setup` phase's unnamed residual** (`1.50` of `2.97`). Where
the rest lands, **read from the per-phase counters rather than inferred**:

| phase | delta |
|---|---:|
| `setup` (format + `push`) | `-1.500` |
| `remainder` (`pop` at function exit) | `-0.747` |
| `stream_prep` | `-0.201` |
| `argbuf_uniform` | `-0.088` |
| others | `-0.014` |
| sum | `-2.550` = `encode_draw` delta |

> **Corrected after review.** This first said the `pop` was `~1.05 ms` in
> `remainder`. That figure was *computed* (`2.549 - 1.499`) while the direct
> measurement sat in the same two runs; the counter says `0.747`. And
> `stream_prep` and `argbuf_uniform` moved by `0.29 ms` combined despite
> containing no debug-group code — reproduced in both off-runs and larger than
> the same-config spread, so it is a real second-order effect (allocator or
> autorelease pressure), not noise. Unexplained, and now at least stated.
>
> The stage-wall delta (`2.729`) also exceeds what the phase counters attribute
> (`2.549`) by `0.18`; the honest cost is **`~2.5-2.7 ms/present`, `n=1`**.

## `2.7 ms/present` of genuinely wasted work, worth zero frame time

`2.729 ms` is `6.8%` of the frame — and removing it moved the frame by `0.01 ms`.

This is the second independent confirmation of
[.13](state-churn-encode-append-decomposition.13.md)'s corrected model, arrived
at by a different route: the encode thread carries `~17 ms/present` of slack, so
CPU removed there is absorbed by waiting and never reaches the frame. The first
confirmation was the `PerfScope` family itself (`4.64 ms` removed, `0.12 ms` of
frame). This is a second, larger one on real production work rather than
instrumentation.

**So the `.09` template does not transfer.** The SWVP probe was `22.6%` of the
frame *and* on the producer thread, which was binding — `c ≈ 1.0`, hence `+29%`.
The identical shape on the encode thread converts at **`c ≈ 0.02-0.03`**.
Finding wasted work is not the same as finding a win; which thread it is on
decides, and on GT2 the encode thread does not bind.

> **Corrected after review: this leaf first quoted `c ≈ 0.004`, which is one
> significant figure derived from a `0.01 ms` frame delta at `n=1`.** That input
> is far inside noise — nominally identical configurations *this same day* spread
> up to `1.5 ms` of frame wall, and the A/A pair spans `3.3%` (`~1.3 ms`). A
> single pair bounds `|c|` only at about `0.5`. Regressing frame wall on encode
> stage wall across the day's nine runs gives `0.019 ± 0.011` excluding the
> `dbggroup-forced-on` outlier, and `.13`'s independent pair (`4.64 ms` removed,
> `0.12 ms` of frame) implies `0.026`. **`c ≈ 0.02-0.03`, upper-bounded around
> `0.05`.** The qualitative conclusion is untouched; the precision was not
> earned, which is the same error as pricing an instrument against an observable
> that cannot see it.

## Resolved: gated on capture

Implemented after this measurement. `perDrawDebugGroupsEnabled()`
(`dxmt9_capture.cpp`) emits the per-draw groups only when a capture is
configured for the process — `DXMT_METAL_CAPTURE_FRAME`, or Apple's
`MTL_CAPTURE_ENABLED` / `METAL_CAPTURE_ENABLED` — with
`DXMT9_PER_DRAW_DEBUG_GROUPS` forcing either direction. Per-render-pass, blit
and present groups are untouched: a few per frame rather than `~1,690`, and
`xctrace` joins on them.

All three branches measured:

| | `setup` | `encode_draw` | groups |
|---|---:|---:|---|
| before gating (always on) | `4.261` | `17.471` | on |
| hard-disabled diagnostic | `2.762` | `14.922` | off |
| **gated, default** | **`2.729`** | **`14.843`** | **off** |
| gated, `DXMT9_PER_DRAW_DEBUG_GROUPS=1` | `4.430` | `18.431` | on (outlier: `1,465` presents vs `~1,568`, frame `+3.7%` — directionally right, do not quote its magnitude) |
| gated, `DXMT_METAL_CAPTURE_FRAME` set | `4.274` | `17.350` | on |

The default matches the hard-disabled diagnostic, and both enable paths restore
the cost — so captures still narrate and normal runs stop paying.

It was a real trade, not a free one:

| for | against |
|---|---|
| `2.7 ms/present` of work with no runtime consumer | zero FPS on GT2 |
| Per-draw heap allocation on a hot path, which `codebase_conventions.rules.md` explicitly forbids | the labels are the documented `.gputrace` navigation aid (`metal_debugging.rules.md` §2) |
| Would matter on any workload where encode *does* bind — untested | removing them silently degrades every future capture |

Gating on capture keeps both sides — the same move the SWVP hoist made, gating
work on the condition that makes it useful. **One route needed an explicit
opt-in**: `run_with_wine_metal_capture_layer.sh` inserts the capture layer
through a patched `Info.plist` and deliberately sets none of the three capture
envs (`MTL_CAPTURE_ENABLED` black-screens 3DMark05), so an Xcode-attached
capture through it would have lost its per-draw narrative silently. That
wrapper now exports `DXMT9_PER_DRAW_DEBUG_GROUPS=1`. **This buys no GT2 frame time and is
not claimed to**: it removes a hot-path allocation the conventions forbid, and it
is the kind of change whose value shows up only on a workload where encode
binds, which remains untested.

**Scope.** One run per configuration, GT2 only, one thermal window; frame
figures agree to `0.01 ms` which is well inside the same-build noise this day
established (`3.3%`), so the null FPS result is "no detectable change", bounded
by that noise rather than proven zero. All absolute figures are
instrument-inflated readings ([.13](state-churn-encode-append-decomposition.13.md)).
The remaining `~1.5 ms` of `setup`'s residual is still unattributed: the
signpost pair, two `emitEncodeProgressDrawStage` calls, and `nextDrawOrdinal`'s
atomic are the candidates, none yet measured.

**Related.**
[append-decomposition.15](state-churn-encode-append-decomposition.15.md) ·
[append-decomposition.13](state-churn-encode-append-decomposition.13.md) ·
[append-decomposition.09](state-churn-encode-append-decomposition.09.md) ·
[frame-lifecycle](../frame-lifecycle.md) ·
[state-churn-encode](index.md)
