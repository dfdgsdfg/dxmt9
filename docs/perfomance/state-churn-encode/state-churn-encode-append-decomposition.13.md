---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 13
title: A Third Of The Encode Draw Path Is Unattributed — And I Priced The Instrument With The Wrong Observable
date: 2026-08-01
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05-perfscope-on; experiments/output/app-d3d9-3dmark05-perfscope-noop
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.12.md
---

# A Third Of The Encode Draw Path Is Unattributed — And I Priced The Instrument With The Wrong Observable

**Question / hypothesis.**
[.12](state-churn-encode-append-decomposition.12.md) found the bottleneck had
moved: the producer is `22%` of the GT2 frame and the encode thread `50%`. The
`.08`-style entry decomposition has only ever been applied to the D3D9 producer
side. What is inside `encode_draw`, and is the existing counter tree enough?

**Method.** The encode path already has `PerfScope` child timers, but nesting is
by lexical scope, not name — `binding_packet_plan` sits inside `binding_packet`,
and several timers appear at multiple call sites. Reading the tree off the
counter names double-counts (a naive sum reaches `102%` of the parent). So the
tree is derived mechanically: track `PerfScope` object lifetimes by brace depth
through `encodeDraw` and keep only the timers whose parent chain is exactly
`[EncodeDraw]`. Counters come from a single GT2 run with no PE-side decimation,
so nothing on the producer side perturbs the encode numbers.

## First: the instrument — and I priced it with the wrong observable

`PerfScope` reads `steady_clock::now()` **unconditionally**; it is not gated by
`DXMT_PERF_COUNTERS`. With ~18 scopes on a `9.7 us` draw, and the unix provider
also built x86_64 under Rosetta, the worry was
[.10](state-churn-encode-append-decomposition.10.md) repeating on this side.

This section originally answered that with a paired same-window A/B of a no-op
`PerfScope` build against the normal one — **frame wall `40.10` vs `40.22 ms`,
`0.3%`** — and concluded the instrument was nearly free.

> **Corrected 2026-08-01 after review. That measurement could not have detected
> the cost it was looking for.** Frame wall is insensitive to encode-thread CPU,
> because — as this same document's sibling
> [.12](state-churn-encode-append-decomposition.12.md) had already established —
> the encode thread carries `~17 ms/present` of slack. Adding or removing CPU
> there is absorbed by waiting and never reaches the frame.
>
> The same two runs carry an **event-timestamp** stage counter that survives a
> no-op `PerfScope` build, and it shows the real cost:
>
> | | encode stage wall | wait before dequeue | frame |
> |---|---:|---:|---:|
> | `PerfScope` **off** | `19.108` | `19.204` | `40.10` |
> | `PerfScope` **on** | `23.745` | `14.913` | `40.22` |
> | + `EncodeDrawPhaseTimer` | `24.390` | `14.298` | `40.17` |
>
> **The `PerfScope` family costs `4.64 ms/present`, `11.5%` of the frame** — 38x
> the `0.12 ms` claimed — and the phase timer of
> [.14](state-churn-encode-append-decomposition.14.md) adds `0.645 ms` (`1.6%`),
> which is why it is now gated behind `DXMT9_PERF_ENCODE_DRAW_PHASE_SPLIT`
> rather than always-on. Encode CPU added is matched almost exactly by waiting
> removed, at constant frame — a redistribution signature that thermal drift
> cannot produce.
>
> The Wine-QPC contrast survives but not the conclusion drawn from it: a clock
> read is **`~43 ns`** here against the PE side's `~180 ns`, so this instrument
> is four times cheaper, **not free**. Every absolute number below is inflated
> by its own measurement.

**What this means for the numbers below.** `encode_draw`'s `16.45 ms` reading
contains the family's own cost; true work is **`~11.8 ms/present`**. The
*proportions* between children are unaffected to first order, since every scope
pays the same overhead, but no absolute figure in the table should be quoted
without this correction.

## The decomposition

Frame `40.22 ms`; `encode_chunk` `20.74`; `encode_draw` `16.45` (**`41%` of the
frame on its own**).

| direct child of `encodeDraw` | ms/present | share of `encode_draw` |
|---|---:|---:|
| `stream_bind` | `4.097` | `24.9%` |
| `binding_packet` | `2.373` | `14.4%` |
| `fvf_decode` | `1.508` | `9.2%` |
| `pipeline_lookup` | `1.080` | `6.6%` |
| `uniform_build` | `0.872` | `5.3%` |
| `issue` | `0.871` | `5.3%` |
| **attributed** | **`10.801`** | **`65.7%`** |
| **UNATTRIBUTED** | **`5.651`** | **`34.3%`** |

`argbuf_setup` is a genuine direct child that reads `0.000`: its path
(`argbufTableMode && reopenArgbufHybrid`) is not taken because
`DXMT9_ARGBUF_DIRECT_CBUF` is default-on. That is a real zero, not a gap.

A further **`4.29 ms/present`** sits in `encode_chunk` outside `encode_draw`
entirely — per-chunk rather than per-draw work, and also unattributed.

## The residual is distributed, not one hidden block

`1,422` of `encodeDraw`'s `4,096` lines lie outside any direct-child scope —
`35%`, suspiciously close to the `34%` of time. That coincidence does not
survive reading the code. The largest uncovered regions are **debug paths that
early-out**:

| lines | what |
|---:|---|
| `12050-12416` (367) | fixed-function trace, gated on `debug::fixedFunctionTraceTextureHandle() != 0` |
| `12967-13098` (132) | `if (traceEncode)` encode tracing |
| `10974-11153` (180) | a *lambda definition* (`uploadTransientBuffer`) — costs nothing where it is declared |

So line coverage is not a proxy for time here, and the `5.7 ms` is spread across
many small uninstrumented stretches rather than concentrated in one. **This is a
negative result about method**: the next step cannot be "instrument the biggest
gap", it has to be a coarse sequential partition of `encodeDraw` that sums to
the parent by construction, refined afterwards.

**Verdict.** `encode_draw` *reads* `16.45 ms/present`; roughly `4.6` of that is
the instrument, so true work is **`~11.8 ms`**. Two thirds of the reading is
attributed, leaving `~5.7 ms` unattributed **as measured** — but part of that
residual is the instrument too, so the honest statement is that a third of the
reading has no counter on it and the absolute size of the real gap is not known
from this run. `stream_bind` at `4.1 ms`
is the largest *named* item. Both are larger than anything remaining on the
producer side, which the same-day `.12` measured at `9.14 ms` in total.

**Scope.** One run per configuration, GT2 only, one thermal window. The
`PerfScope`-lifetime tree is derived from lexical nesting and would mis-parse a
scope opened inside a macro; spot-checked against the source at
`binding_packet` (which does contain `plan`, `cache` and `texture_record`) and
at `stream_bind` (five call sites, all direct children). The child timers are a
partition only of the code they wrap — they do not include the branches between
them, which is precisely what the residual is.

**Related.**
[append-decomposition.12](state-churn-encode-append-decomposition.12.md) ·
[append-decomposition.10](state-churn-encode-append-decomposition.10.md) ·
[append-decomposition.08](state-churn-encode-append-decomposition.08.md) ·
[frame-lifecycle](../frame-lifecycle.md) ·
[state-churn-encode](index.md)
