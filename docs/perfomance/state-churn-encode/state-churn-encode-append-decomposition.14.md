---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 14
title: Partitioning encodeDraw Works — But The Residual It Seemed To Show Was Mostly Mine
date: 2026-08-01
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05-encode-phases-v2; experiments/output/app-d3d9-3dmark05-encode-phases
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.13.md
---

# Partitioning encodeDraw Works — But The Residual It Seemed To Show Was Mostly Mine

**Question / hypothesis.**
[.13](state-churn-encode-append-decomposition.13.md) found `34%` of
`encode_draw` — `5.7 ms/present`, `14%` of the GT2 frame — with no counter on
it, and showed the gap is *distributed*: the largest uninstrumented line regions
are debug paths that early-out. So "instrument the biggest gap" was ruled out.
A partition that sums to the parent by construction is the only shape that can
localize it. Where is it?

**Method.** `EncodeDrawPhaseTimer` stamps the clock at seven fixed points in
`encodeDraw` and attributes each interval to exactly one phase; the destructor
closes the final phase, so every nanosecond between entry and return lands
somewhere and early exits are covered. Always on, not flag-gated: `.13` measured
the whole `PerfScope` family at `0.3%` of the frame, because the unix side calls
macOS `steady_clock` directly rather than going through Wine's `QPC`.

**Result — the partition closes.**

Frame `40.17 ms`, `encode_draw` `17.12 ms/present` (`43%` of the frame).

| phase | ms/present | share |
|---|---:|---:|
| `remainder` (last mark → return) | `5.066` | `29.6%` |
| `setup` (entry → tile-FFP PSO) | `4.224` | `24.7%` |
| `argbuf_uniform` | `3.617` | `21.1%` |
| `stream_prep` | `1.970` | `11.5%` |
| `base_state` | `1.052` | `6.1%` |
| `vertex_bind` | `0.909` | `5.3%` |
| `ffp_vertex` | `0.101` | `0.6%` |
| `tile_ffp_fallthrough` | `0.001` | `0.0%` |
| **sum** | **`16.94`** | **`98.9%`** |

`98.9%` of the parent, the missing `1.1%` being the marks' own clock reads and
where the parent `PerfScope` opens relative to the first mark. The partition
works.

## One label was wrong, and the counter is renamed rather than explained

The phase after the `if (indexedDraw)` block reads `0.001 ms` — impossible, since
`index_setup` alone is `2.26 ms` and sits inside it. The cause is that
`encodeDraw`'s common path returns from **inside a nested block** (`return true`
after `emitTileFfpPostPass()`), before that mark is ever reached. The
destructor therefore books the indexed-setup and draw-issue work, and the mark
after the return only catches a rare fallthrough.

So the partition was right and the naming was a trap. The counters are renamed:
the destructor's is `remainder` — accurate on every path, rather than named for
a region it only measures on the common one — and the rare mark is
`tile_ffp_fallthrough`. A counter whose name says "indexed" while measuring a
near-dead path is exactly the kind of thing that costs someone a day later.

## Where the unattributed time actually is

Subtracting the named child timers that fall inside each phase:

| phase | phase ms | named children | **unnamed** |
|---|---:|---:|---:|
| `stream_prep` | `1.970` | `0.000` | **`1.970`** |
| `setup` | `4.224` | `2.591` | **`1.633`** |
| `vertex_bind` | `0.909` | `0.000` | **`0.909`** |
| `argbuf_uniform` | `3.617` | `3.182` | `0.434` |
| `base_state` | `1.052` | `0.729` | `0.322` |
| `remainder` | `5.066` | `4.766` | `0.300` |

> **Corrected 2026-08-01 after reading the code — `stream_prep` is not
> unnamed.** The region contains a `stream_bind` call site (the viewport /
> scissor / cull bind at `d3d9_draw_encoder.mm:11833`) and the *whole* of
> `raster_state` (`0.299 ms`, its only call site). The "named children" column
> above reads `0.000` for it only because `stream_bind` was excluded from that
> column, and I then read the resulting zero as evidence of missing coverage.
> The caveat below was already written and I asserted the claim anyway. The same
> error applies to `vertex_bind`, which is bounded by two `stream_bind` sites.
>
> What survives: the `setup` prologue's `1.63 ms` beyond `fvf_decode` and
> `pipeline_lookup` is real, since those are its only children. The phase
> numbers themselves are unaffected.

**Read that table as indicative, not exact.** Several child timers have multiple
call sites spread across phases — `stream_bind` has five, `fvf_decode` three —
so they cannot be assigned to a single phase, and `stream_bind`'s `4.20 ms` is
excluded from the "named children" column entirely. The *phase* numbers are
exact; the per-phase split between named and unnamed is not.

## Reading `stream_prep` for discarded work — there is none

The `.09` question, asked of the `1.97 ms`: is anything computed and thrown
away? The region resolves the vertex buffer and builds `vertexBytes`, a span
over the CPU-side shadow or mapped contents. Tracing every use of that span:

| consumer | status |
|---|---|
| `traceEncode` blocks (`:11969`, `:12009`, `:13167`) | debug, early-out |
| fixed-function trace (`:12094`-`:12191`) | gated on `debug::fixedFunctionTraceTextureHandle()` |
| `traceEffectIndexedGeometry` (`:13790`, `:14350`) | returns immediately on `effectDrawTraceSeqMatches()` |
| `expandIndexedStreamToFlatVertexBytes` (`:13207`) | real, but only on the index-expansion path |

So on a default GT2 draw `vertexBytes` is indeed built and not consumed — but
**this is not the SWVP shape**. That probe read and adjusted every index in the
buffer, `~6,360` element iterations per draw. This is a handful of span
assignments over memory that is already mapped; the pool lookup that precedes it
is needed for `vertexBuffer` regardless. Cheap to compute, cheap to discard.

The actual content of `stream_prep` is the per-draw viewport / scissor / cull
rebind — and a comment at `:11840` records that the obvious fix was already
tried: a per-draw viewport/scissor shadow cache (`5eef5d4`, 2026-06-05) was
reverted because GT1 bind diversity made the hit rate ~zero while the equality
comparisons cost `+12.7%` `encode_chunk_cpu_ms`.

**Verdict.** The partition works and is the right instrument. The attribution
conclusion it first supported does not: `stream_prep` and `vertex_bind` are
covered by `stream_bind` call sites, and reading `stream_prep` found no
discarded work worth removing. **The one solid residual is the `setup`
prologue's `1.63 ms`** — `encode_draw`'s first `4.22 ms` runs before any binding
work, and only `2.59` of it has a name. That is where to look next.

**What this does not say.** Nothing here is a removal candidate. `.09`'s SWVP
probe was `22.6%` of a frame doing something genuinely useless; `stream_prep`
was read with that same question and the answer was no. The encode side has a
map of where the time is and, so far, no equivalent finding.

**Scope.** One run per configuration, GT2 only, one thermal window. Phase
boundaries were chosen at function-body statement level; a boundary inside a
branch would attribute unevenly across draws that take different paths, and the
`tile_ffp_fallthrough` case above is exactly that failure caught by its own
number being absurd. The named/unnamed split is unreliable wherever a
multi-site child lands, which is most phases — only `setup` is clean.

**Related.**
[append-decomposition.13](state-churn-encode-append-decomposition.13.md) ·
[append-decomposition.12](state-churn-encode-append-decomposition.12.md) ·
[append-decomposition.09](state-churn-encode-append-decomposition.09.md) ·
[state-churn-encode](index.md)
