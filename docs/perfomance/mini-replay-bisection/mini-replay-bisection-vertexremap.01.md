---
domain: mini-replay-bisection
workload: 3DMark05 GT1
subcategory: vertexremap
order: 01
title: Vertex Remap Is A Null On Row 60/1, And Its Positive Control Failed
date: 2026-07-28
type: experiment-run
status: lanec-null-positive-control-failed
source: traces/app-d3d9-3dmark05-vertexremap-enc1-r1/analysis/laneA-counters-xcode.csv; traces/app-d3d9-3dmark05-vertexremap-enc1-r1/analysis/laneB-counters-xcode.csv; traces/app-d3d9-3dmark05-vertexremap-enc1-r1/analysis/laneC-counters-xcode.csv; traces/app-d3d9-3dmark05-vertexremap-enc1-r1/analysis/laneD-counters-xcode.csv; traces/app-d3d9-3dmark05-vertexremap-enc1-r1/analysis/frame60-mini-replay-manifest-enc1.json; docs/superpowers/specs/2026-07-25-vertex-remap-design.md
related: docs/perfomance/mini-replay-bisection/mini-replay-bisection-replay.03.md; docs/perfomance/hidden-backend-storage/overview.md
---

# Vertex Remap Is A Null On Row 60/1, And Its Positive Control Failed

**Question / hypothesis.** `replay.03`'s `3.86x` hidden VS-write density spread
(`1710.0` vs `442.6` B/VS invocation) came from `sort-min-index`, which changes
both index reference monotonicity and primitive order. Which one owns it? If
index locality owns it, a pure vertex permutation recovers the win with no
final-color oracle requirement, because triangle order and triangle composition
are preserved exactly.

**Method.** Four lanes over one GT1 frame60 mini-replay manifest, same build,
same machine, same depth sidecar, one Xcode counter export each. Lane A
original; lane B `sort-min-index`; lane C original primitive order with a
first-reference vertex permutation; lane D `sort-min-index` with a scattered
vertex permutation. Recovery is `(d(A) - d(X)) / (d(A) - d(B))` over VS write
bytes per invocation.

**Deviation from the design.** The design targets encoder2 (row `60/2`), the row
`replay.03` measured. That dump does not exist; the available captures are
row `60/0` (`enc0`, 156 draws) and row `60/1` (`enc1`, 229 draws). Row `60/0`
renders to R32F, which `run_3dmark05_mini_replay.py` now rejects outright rather
than silently reinterpreting through `RGBA8Unorm`
(`specs/experiments/harness/replay/requirements.md` R-HARN-REPLAY-2.1). So all
four lanes ran on row `60/1`, 229 draws, ordinals `31685..31913`.

**Result.** Render-encoder row, one export per lane:

| Lane | GPU ms | VS write MiB | TVB MiB | VS invocations | B / invocation | recovery |
|---|---:|---:|---:|---:|---:|---:|
| A | 21.092 | 1206.8 | 13.4 | 794,896 | `1591.9` | `-0.000` |
| B | 23.312 | 1412.6 | 15.1 | 912,443 | `1623.4` | `1.000` |
| C | 21.145 | 1206.7 | 13.4 | 794,896 | `1591.8` | `-0.002` |
| D | 23.952 | 1412.7 | 15.1 | 912,443 | `1623.4` | `1.001` |

Lane C's color output SHA-256 matched lane A, as a pure permutation requires.
Lane C VS-invocation drift versus lane A was `0.000%`, inside the `1%` gate, and
its replay and original LRU32 miss counts were equal (`854,524`, delta
`0.0000%`). Lanes B and D moved LRU32 misses to `947,664` (`+10.9%`), as a
primitive reorder legitimately may.

**Lane C is an exact null, not a small win.** Against lane A it changes VS
invocations by `0.000%` (`794,896` both), Tiled Vertex Buffer bytes by `0.000%`
(`14,057,472` both), TVB primitive-block bytes by `0.000%` (`13,139,968` both),
and VS buffer device-memory bytes written by `-0.005%`
(`1,265,398,976 -> 1,265,341,376`, a `57,600`-byte difference out of `1.27 GB`).
GPU time is `+0.251%`. Vertex storage layout is not a lever on this row.

**The positive control failed.** The design's own sanity gate says: if lane B's
B/invocation is not near `442.6`, the manifest does not represent the same row
as `replay.03` and no lane may be interpreted. Lane B measured `1623.4` — not
near `442.6`, and `+2.0%` *above* lane A rather than `3.86x` below it. On row
`60/1`, `sort-min-index` is actively harmful: `+14.8%` VS invocations, `+17.1%`
VS device writes, `+12.4%` TVB bytes, `+10.5%` GPU time, with primitive count
unchanged at `486,670`. That is the opposite sign from the encoder2 observation,
so the effect of `sort-min-index` on hidden write density is row-dependent.

The `recovery` column is therefore not meaningful here. Because lane B raised
B/invocation, the recovery denominator is negative (`1591.9 - 1623.4 = -31.5`),
so `recovery(D) = 1.001` means only "lane D matches lane B", not "lane D
recovered a win". The largest B/invocation deviation from lane A across all
lanes is `2.0%`, inside the `10%` Xcode replay-noise floor
(`agents/rules/metal_debugging.rules.md` §5) — the four lanes do not separate on
the metric the discriminator scores.

**Verdict.** Two separable claims. (1) On row `60/1`, a pure vertex permutation
recovers nothing: VS invocations, TVB bytes, and TVB primitive-block bytes are
bit-identical and VS device writes move `-0.005%`. Hidden VS/parameter-buffer
write volume on this row tracks VS invocation count — `~1591.9` B per invocation
for both lanes that share lane A's index stream, `~1623.4` for both that share
lane B's — and a permutation cannot change invocation count. (2) This run does
**not** answer the `replay.03` question, because its positive control did not
reproduce on this row. Do not read it as "primitive order owns the `3.86x`
delta"; the discriminator never ran on the row where that delta was observed.
Answering it still requires the encoder2 (row `60/2`) dump the design's Task 3
specifies. Do not retry the vertex-remap lane on row `60/1`.

**Related.** [mini-replay-bisection](index.md) · [mini-replay-bisection-replay.03](mini-replay-bisection-replay.03.md) ·
[hidden-backend-storage](../hidden-backend-storage/index.md) · [overview-3dmark05-gt1](../overview-3dmark05-gt1.md)
