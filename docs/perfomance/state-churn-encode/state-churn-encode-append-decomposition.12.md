---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 12
title: After The Hoist The Bottleneck Moved — The Encode Thread Is Now 2.2x The Producer
date: 2026-08-01
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05-gt2-entry-posthoist-n64; experiments/output/app-d3d9-3dmark05-gt2-entry-posthoist-n16
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.11.md; docs/perfomance/frame-lifecycle.md
---

# After The Hoist The Bottleneck Moved — The Encode Thread Is Now 2.2x The Producer

**Question / hypothesis.** [.11](state-churn-encode-append-decomposition.11.md)
took the GT2 frame from `54.05` to `41.90 ms`. Every per-stage share in
[.08](state-churn-encode-append-decomposition.08.md) and `.11` is against the
**old** frame, and the producer's remaining cost was inferred by subtraction
(`21.4 - 12.0 ≈ 9.4 ms`), not measured — with an instrument that was wrong twice
in one day. Where is the time now?

**Method.** The D3D9 entry decimation re-run on the hoisted build, `N=64` and
`N=16`, GT2, `perf`, `--keep-frontmost`, `120 s`, per-run gates on
`result.json`. `entry_draw` is corrected for the two parent-gated draw phase
timers still compiled in (`~671 ns`, priced in
[.10](state-churn-encode-append-decomposition.10.md)) — at a `~4.3 us` draw
entry that echo is now `15%` of the reading, against `1.6%` before the hoist, so
it can no longer be waved off. Per-thread CPU is taken from the same two runs so
nothing is compared across sessions.

**Result — the two rates agree to `0.1%` on the total.**

| | `N=64` | `N=16` |
|---|---:|---:|
| frame | `41.10 ms` | `42.70 ms` |
| `entry_draw` | `4,255 ns` → `7.20 ms/present` | `4,295` → `7.27` |
| `entry_const` | `79.9 ns` → `1.73` | `79.0` → `1.72` |
| `entry_state` | `107.0 ns` → `0.20` | `71.5` → `0.14` |
| **ENTRY total** | **`9.14 ms`** = **`22.2%`** | **`9.12 ms`** = **`21.4%`** |

**The hoist landed exactly where it was aimed.** The `swvp` phase reads
`193.0` / `179.8 ns` raw against null reads of `180.5` / `172.2` — the two SWVP
probes now cost **`~10-15 ns`**, down from `7,097`. The draw entry is now `86%`
the actual record append, which is what it should be with the probe gone.

Two independent consistency checks pass without being arranged: `entry_const`
reads `79.9` / `79.0 ns` against `.10`'s separately measured `79-86 ns`, and the
removal implied by the entry totals (`21.4 - 9.14 = 12.3 ms`) matches `.09`'s
predicted `12.02` within `2%`.

## A second, tighter estimate of the conversion ratio

`.11` put `c ≈ 1.0 ± 0.1-0.15`, from `.09`'s instrument-measured prediction
against a wall-clock delta. This run supplies an independent estimate with
**both endpoints measured the same way**: producer entry `21.4 -> 9.14 ms`
(`-12.27`) against frame `54.05 -> 41.90` (`-12.15`), giving **`c = 0.99`**.

Two estimates by different routes, `1.01` and `0.99`. That does tighten the
band, but not to two figures: both share the same `.08` entry measurement as an
input, and `.11`'s caution stands — `~1.4 ms` of the wall delta is reduced
producer *blocking* rather than removed work. **`c ≈ 1.0` for producer-thread
CPU on a producer-bound GT2 frame**, and nothing beyond that.

## Where the time is now

All from the `N=64` run, frame `41.10 ms`:

| | ms/present | share |
|---|---:|---:|
| **encode thread** (`encode_chunk_cpu`) | **`20.47`** | **`49.8%`** |
| — of which `encode_draw_cpu` | `16.25` | `39.5%` |
| replay worker (`offload_replay_cpu`) | `17.52` | `42.6%` |
| — `d3d9_snapshot_draw_submission` | `9.44` | `23.0%` |
| — `d3d9_snapshot_cache_lookup` | `7.92` | `19.3%` |
| **producer D3D9 entry** | **`9.14`** | **`22.2%`** |
| GPU (`gpu_command_buffer_time`) | `1.90` | `4.6%` |
| replay worker **idle** | `25.77` | `62.7%` |

**The bottleneck moved.** The producer was `~41%` of the old frame and the
single largest block; it is now `22%` and **the encode thread is `2.2x` it**.
Optimising the producer further is no longer the highest-value direction.

**But no thread is saturated, so this is still a serial-chain frame.** The
largest single consumer is `20.47 ms` against a `41.10 ms` frame, and the three
CPU figures sum to `47.1 ms` — they overlap, so they cannot be added. The frame
is set by the produce → replay → encode → present chain
([frame-lifecycle](../frame-lifecycle.md) §1), not by any one stage's
throughput. The replay worker idling `25.8 ms/present` (`63%` slack) is the
clearest evidence that capacity is not the constraint.

**And GPU is not the constraint by a wide margin**: `1.90 ms`, `4.6%` of the
frame. Anything that trades CPU for GPU work is nearly free on this workload.

## What this opens, and what it warns against

The two candidate directions are now:

1. **Encode-thread CPU**, `20.5 ms` with `16.25` in `encode_draw`. Largest single
   block, never decomposed at this level — the `.08`-style entry treatment has
   only ever been applied to the D3D9 producer side.
2. **The serial chain itself.** With every stage under half the frame and the
   worker `63%` idle, overlap is where the arithmetic says the remaining time
   is. That lane has a bad history: the `DXMT9_OPEN_CB_CARRIER` family
   (H183-H229) proved the mechanism repeatedly and never cleared the
   command-buffer/render-pass locality gates.

**Scope.** One run per rate, GT2 only, one thermal window. The per-thread CPU
counters overlap and are not a partition of the frame; treat them as sizes, not
shares of a budget. `entry_state` at `71-107 ns` remains at this instrument's
resolution and should not carry an argument, as `.10` records.

**Related.**
[append-decomposition.11](state-churn-encode-append-decomposition.11.md) ·
[append-decomposition.10](state-churn-encode-append-decomposition.10.md) ·
[append-decomposition.08](state-churn-encode-append-decomposition.08.md) ·
[frame-lifecycle](../frame-lifecycle.md) ·
[state-churn-encode](index.md)
