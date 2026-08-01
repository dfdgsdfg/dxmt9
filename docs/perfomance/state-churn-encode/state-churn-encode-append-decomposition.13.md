---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 13
title: A Third Of The Encode Draw Path Is Unattributed — And This Instrument, Unlike The PE One, Is Nearly Free
date: 2026-08-01
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05-perfscope-on; experiments/output/app-d3d9-3dmark05-perfscope-noop
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.12.md
---

# A Third Of The Encode Draw Path Is Unattributed — And This Instrument, Unlike The PE One, Is Nearly Free

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

## First: the instrument is trustworthy — verified, not assumed

`PerfScope` reads `steady_clock::now()` **unconditionally**; it is not gated by
`DXMT_PERF_COUNTERS`. With ~18 scopes on a `9.7 us` draw, and with the unix
provider also built x86_64 under Rosetta, the obvious worry was
[.10](state-churn-encode-append-decomposition.10.md) repeating on this side —
a decomposition that is mostly its own clock reads.

Measured, not argued. A diagnostic build with `PerfScope` forced to a no-op,
paired back-to-back against the normal build in the same thermal window:

| | frame | scene fps |
|---|---:|---:|
| `PerfScope` **off** | `40.10 ms` | `24.94` |
| `PerfScope` **on** | `40.22 ms` | `24.86` |

**`0.12 ms/present`, `0.3%`.** The hypothesis was wrong and the encode
decomposition can be read at face value.

An earlier unpaired look at the same no-op run suggested `+4.5%` against the
A/B's `23.87 fps` — that was entirely session drift across thermal windows, and
is exactly the error the paired run exists to prevent.

**Why this side is cheap and the PE side is not.** Both are x86_64 under
Rosetta, so Rosetta is not the variable. The PE-side clock goes through Wine's
`QPC` emulation — the header of `d3d9_pe_stats_decimation.hpp` prices full
instrumentation at `~1.5 us` per timed scope, and `.10` measured a single read
at `~180 ns`. The unix side calls macOS's `steady_clock` directly. **The
expensive thing was never Rosetta; it was Wine's QPC.** Adding scopes on the
encode side is therefore nearly free, and does not need the flag-gating the
`DXMT9_PERF_*_SPLIT` family uses.

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

**Verdict.** `encode_draw` is `16.45 ms/present`, `41%` of the GT2 frame; two
thirds is attributed, and **`5.7 ms/present` — `14%` of the whole frame — is
inside our hottest function with no counter on it**. `stream_bind` at `4.1 ms`
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
