---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 14
title: Partitioning encodeDraw Localizes The Missing Third — Stream Prep And The Prologue
date: 2026-08-01
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05-encode-phases-v2; experiments/output/app-d3d9-3dmark05-encode-phases
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.13.md
---

# Partitioning encodeDraw Localizes The Missing Third — Stream Prep And The Prologue

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

**Two phases are `100%` unnamed** — `stream_prep` (`1.97 ms`: transient buffer
construction, stream/index staging) and `vertex_bind` (`0.91 ms`) — and the
`setup` prologue carries another `1.63 ms` beyond its `fvf_decode` and
`pipeline_lookup` children. Those three account for `~4.5` of the `~5.7 ms`.

**Read that table as indicative, not exact.** Several child timers have multiple
call sites spread across phases — `stream_bind` has five, `fvf_decode` three —
so they cannot be assigned to a single phase, and `stream_bind`'s `4.20 ms` is
excluded from the "named children" column entirely. The *phase* numbers are
exact; the per-phase split between named and unnamed is not.

**Verdict.** The missing third is now localized to three regions, and the two
largest are unambiguous: **`stream_prep` and `vertex_bind` have no child timer
at all**, which is why nothing showed them. `setup` at `4.22 ms` is also larger
than expected for a prologue — it is `24.7%` of `encode_draw` before any binding
work begins.

**What this does not say.** Nothing here says the work is removable. `.09`'s
SWVP probe was `22.6%` of a frame doing something genuinely useless; there is no
equivalent finding yet on the encode side, only a map of where the time is.
The next step is to read `stream_prep` and the `setup` prologue with the same
question `.09` asked — is any of this computed and then discarded — before
proposing anything.

**Scope.** One run per configuration, GT2 only, one thermal window. Phase
boundaries were chosen at function-body statement level; a boundary inside a
branch would attribute unevenly across draws that take different paths, and the
`tile_ffp_fallthrough` case above is exactly that failure caught by its own
number being absurd. The three phases with no named children are the ones where
that risk is lowest, since nothing else in them is being double-booked.

**Related.**
[append-decomposition.13](state-churn-encode-append-decomposition.13.md) ·
[append-decomposition.12](state-churn-encode-append-decomposition.12.md) ·
[append-decomposition.09](state-churn-encode-append-decomposition.09.md) ·
[state-churn-encode](index.md)
