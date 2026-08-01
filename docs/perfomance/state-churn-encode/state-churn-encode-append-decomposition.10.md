---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 10
title: De-phasing Confirms The Nested-Instrument Echo Directly — 756 ns Was 79
date: 2026-08-01
type: experiment-run
status: accepted-instrument-validation
source: experiments/output/app-d3d9-3dmark05-gt2-entry-dephased-n64; experiments/output/app-d3d9-3dmark05-gt2-entry-dephased-n64-r2; experiments/output/app-d3d9-3dmark05-gt2-entry-dephased-n64-r3
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.08.md
---

# De-phasing Confirms The Nested-Instrument Echo Directly — 756 ns Was 79

**Question / hypothesis.**
[.08](state-churn-encode-append-decomposition.08.md) was corrected the same day
it was published: `entry_const` read `756 ns/call` because it sampled the exact
calls `touchConstShadow` sampled, so its span swallowed the inner scope's whole
instrument. That correction was *arithmetic* — subtract four clock reads at the
null price. The fix (a per-scope `phaseOffset`, so lockstep scopes cannot
coincide) makes the same quantity measurable **directly**. Does the instrument
agree with the arithmetic?

**Method.** Three GT2 runs at `N=64` on the de-phased build, `perf`,
`--keep-frontmost`, `120 s`, 180 s caffeinated cooldown between runs. Nothing
else changed. `peConstSetterDecimatedStats()` is phase `1`, `entry_const` phase
`0`, so a sampled entry span now contains an *unsampled* `touchConstShadow`.

**Result — it agrees, and the residual collapses by 9x.**

| | phase-locked (`N=64`) | de-phased r1 / r2 / r3 |
|---|---:|---:|
| `entry_const` ns/call | `755.8` | **`85.8` / `84.7` / `81.5`** |
| `entry_state` ns/call (control, nests nothing) | `98.6` | `100.9` / `115.1` / `116.6` |
| **const ÷ state** | **`7.67x`** | **`0.85` / `0.74` / `0.70`** |
| `entry_const` ms/present | `16.50` | `1.89` / `1.83` / `1.79` |

The internal control that the first version never ran now reads correctly: a
const setter costs **less** than a state setter, which is the only defensible
ordering — `SetVertexShaderConstantF` validates a range and calls a `24 ns`
shadow touch, while `SetRenderState` validates, compares, and dirties.

**The echo's size is now measured two independent ways, and they agree within
`0.9%`.**

| method | what it prices | result |
|---|---|---:|
| add two `DxmtPeDecimatedPhaseTimer`s to the **draw** entry (`.08`, the `drawphase` build) | 4 clock reads inside an unrelated scope | **`671 ns`** |
| de-phase the **const** entry (this run) | the 4 clock reads of the nested arm+destructor | **`677 ns`** |

The second figure is `755.8` minus the de-phased reading normalized to the
locked run's frame time (`79.0 ns`; see Scope). Two different scopes, two
different builds, two different mechanisms, `671` against `677`. The nested
instrument costs `~169 ns` per clock read — slightly under the `180 ns` null
read, which is why `.08`'s arithmetic (`4 × 180 = 722`) over-corrected and
predicted `3-57 ns` where the truth is `~79`. **Right by an order of magnitude,
low by `45 ns`.** Direct measurement beats subtraction, which is the point of
building the fix rather than only documenting the bias.

**Verdict.** `entry_const` is `~79-86 ns/call`, `~1.8 ms/present`. The published
`756 ns` / `16.50 ms` was **`8.9x` high** and is fully explained. `.08`'s
corrected numbers stand, with its const figure now measured rather than
inferred.

**The ENTRY share is `~40%`, and these runs cannot tighten it.** Measured
directly here it is `42.8-43.6%`; `.08`'s corrected arithmetic on the faster
locked run gives `38.7%`. The gap is ambient, not de-phasing — **every** scope
except `entry_const` reads higher in these three runs:

| | `entry_draw` | `append` | `const_setter` | `const_flush` | `draw_packet` | frame | `entry_const` |
|---|---:|---:|---:|---:|---:|---:|---:|
| de-phased ÷ locked | `1.13x` | `1.25x` | `1.34x` | `1.50x` | `1.20x` | `1.06x` | **`0.11x`** |

The machine is simply slower across the board in this window — the null read
itself moved `180.5 → 183-189 ns`. De-phasing cannot make an unrelated scope
`1.25x` more expensive; it can only change `entry_const`, and that is exactly
the one column that moves the other way. Treat the entry share as **`~40%`**,
not either endpoint, and do not read the `1.13x` on `entry_draw` as a finding.

**Scope.** Three runs, `N=64` only, GT2 only, all inside one thermal window and
not interleaved against a same-window control — which is why the ENTRY share is
given as a range and only the *within-run* quantities (ns/call, the const÷state
ratio, the 9x collapse) are treated as solid. The normalization that produces
`79.0 ns` assumes `entry_const` scales with frame time like everything else in
the table; at this magnitude the assumption moves the echo estimate by `~7 ns`
and cannot change the conclusion. `entry_state` drifting `98.6 → 116.6` in the
same window is a reminder that a `100 ns` scope is near this instrument's
resolution in either direction.

**Related.**
[append-decomposition.08](state-churn-encode-append-decomposition.08.md) ·
[append-decomposition.09](state-churn-encode-append-decomposition.09.md) ·
[attribution.04](../present-pacing/present-pacing-post-defselect-cpu-attribution.04.md) ·
[frame-lifecycle](../frame-lifecycle.md) ·
[state-churn-encode](index.md)
