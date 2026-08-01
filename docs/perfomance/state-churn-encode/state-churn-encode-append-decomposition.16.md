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

It is **half the `setup` phase's unnamed residual** (`1.50` of `2.97`), and the
split is explained: the `push` and the string formatting land in `setup`, while
the `~1.05 ms` `pop` lands in `remainder`, because the `optional` is destroyed at
function exit.

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
The identical shape on the encode thread converts at **`c ≈ 0.004`**. Finding
wasted work is not the same as finding a win; which thread it is on decides,
and on GT2 the encode thread does not bind.

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
| gated, `DXMT9_PER_DRAW_DEBUG_GROUPS=1` | `4.430` | `18.431` | on |
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
work on the condition that makes it useful. **This buys no GT2 frame time and is
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
