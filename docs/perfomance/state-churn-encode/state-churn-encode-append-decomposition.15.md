---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 15
title: Per-Call-Site Counters Make The Split Computable — And Invert Two Of My Conclusions
date: 2026-08-01
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05-encode-sites
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.14.md
---

# Per-Call-Site Counters Make The Split Computable — And Invert Two Of My Conclusions

**Question / hypothesis.**
[.14](state-churn-encode-append-decomposition.14.md) ended by admitting the
per-phase named/unnamed split was uncomputable: `stream_bind` has five call
sites and `fvf_decode` three, spread across phases, so their aggregates belong
to no single phase. Two hand-mappings of those sites were published and both
were wrong. Give each site its own counter and let the arithmetic decide.

**Method.** `PerfScope` takes an optional second record target. It costs one
atomic add and **no extra clock read** — the elapsed value is already computed —
and is null unless `DXMT9_PERF_ENCODE_DRAW_PHASE_SPLIT` is set. Eleven sites
split: `stream_bind` ×5, `fvf_decode` ×3, `uniform_build` ×3. `issue`'s three
sites all fall in one phase, so its aggregate already suffices.

Counters are named for what each site *does* (`stream_bind_index`,
`fvf_decode_bytes`), not for the phase it currently sits in. A name that encodes
a location is a name that lies the moment code moves — which is exactly how the
`indexed`/`remainder` counter went wrong in `.14`.

**Self-check first: every site sum equals its aggregate to `100.0%`** — `4.235`,
`1.563`, `0.892`. Note what this does and does not prove: the same elapsed value
is recorded to both counters, so equality is guaranteed *if every site passes a
site pointer*. It therefore proves complete call-site coverage — including the
`~0` `fvf_decode_expanded` — and cannot catch a site wired to the wrong name.

## The two inversions

| aggregate | ms | what it actually is |
|---|---:|---|
| `stream_bind` `4.235` | `2.555` | **index staging** (`stream_bind_index`, `60%`) |
| | `0.787` | texture |
| | `0.543` | vertex stream |
| | `0.346` | viewport / scissor / cull |
| | `0.004` | FFP stream |
| `fvf_decode` `1.563` | `1.340` | **stream0 bytes** (`86%`) |
| | `0.224` | declaration decode |

**`stream_bind` is not a state-binding cost.** `60%` of it is index staging —
`index_setup` (`2.359`) nests inside the site at `13502`. `.13` called
`stream_bind` "the largest *named* item" at `4.1 ms` and left the impression of
per-draw state rebinding; most of it is index work.

**`fvf_decode` is not a `setup` cost.** `86%` is the site at `11909`, which runs
on every draw inside `stream_prep`. `.14` credited the whole `1.508` to `setup`.

## The split, now computable

Nesting handled — `texture_sampler_bind` (`0.724`) is inside
`stream_bind_texture`, and `index_setup` (`2.359`) inside `stream_bind_index`;
adding either would double-count. `tile_ffp_fallthrough` is merged into
`remainder`, because the common path returns before its mark (`.14`).

| phase | ms | named | **unnamed** | |
|---|---:|---:|---:|---:|
| `setup` | `4.261` | `1.288` | **`2.973`** | `70%` |
| `remainder` | `5.258` | `3.481` | `1.776` | `34%` |
| `argbuf_uniform` | `3.692` | `3.160` | `0.532` | `14%` |
| `vertex_bind` | `0.951` | `0.630` | `0.320` | `34%` |
| `base_state` | `1.053` | `0.787` | `0.265` | `25%` |
| `ffp_vertex` | `0.104` | `0.005` | `0.099` | `95%` |
| **`stream_prep`** | `1.974` | `1.969` | **`0.005`** | **`0%`** |
| total | `17.291` | `11.321` | `5.970` | `35%` |

The phase total (`17.291`) is `0.180` under `encode_draw` (`17.471`) — `~107 ns`
per draw of entry/exit boundary, quantified in `.14`. "Sums to the parent by
construction" is true to `~1%`, not exactly.

**`stream_prep` is `99.8%` accounted for.** `.14` first published it as `100%`
unnamed and the largest gap. It is the *smallest*. Both of that leaf's
hand-mappings were wrong in the same direction, and this is the third attempt —
the first two were arithmetic on names, this one is arithmetic on measurements.

**`setup` is the real gap: `2.97 ms/present` unnamed, `70%` of the phase.** `.14`
put it at `1.63` by crediting `setup` with all of `fvf_decode`; the correct
figure is `1.8x` larger. `encode_draw`'s first `4.26 ms` runs before any binding
work and only `1.29` of it has a name.

**Scope.** One run, GT2 only. All figures are instrument-inflated readings —
the `PerfScope` family costs `4.64 ms/present`
([.13](state-churn-encode-append-decomposition.13.md), corrected), so treat the
proportions as sound and the absolutes as upper bounds. The phase boundaries are
statement-level, but `stream_bind`'s texture site and `texture_sampler_bind`
share an end brace, which is what produced a *negative* `base_state` residual on
the first pass — a nested pair summed as siblings. Negative residuals are the
useful failure mode here: they say the mapping is wrong rather than quietly
biasing a number, and both times that is how the error surfaced.

**What this does not say.** Still no removal candidate. `setup` is now a
`2.97 ms` block with no counter on it rather than a `1.63 ms` one, which makes
it worth reading. [.16](state-churn-encode-append-decomposition.16.md) read it:
half of that residual IS `.09`-style discarded work (a per-draw Metal debug
group), worth `2.7 ms/present` and `0.01 ms` of frame time — the encode thread
does not bind, so the shape transfers and the payoff does not.

**Related.**
[append-decomposition.14](state-churn-encode-append-decomposition.14.md) ·
[append-decomposition.13](state-churn-encode-append-decomposition.13.md) ·
[append-decomposition.12](state-churn-encode-append-decomposition.12.md) ·
[state-churn-encode](index.md)
