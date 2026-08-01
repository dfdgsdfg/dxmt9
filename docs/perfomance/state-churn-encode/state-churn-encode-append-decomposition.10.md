---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 10
title: De-phasing Confirms The Nested-Instrument Echo Directly — 756 ns Was ~80
date: 2026-08-01
type: experiment-run
status: accepted-instrument-validation
source: experiments/output/app-d3d9-3dmark05-gt2-entry-dephased-n64; experiments/output/app-d3d9-3dmark05-gt2-entry-dephased-n64-r2; experiments/output/app-d3d9-3dmark05-gt2-entry-dephased-n64-r3
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.08.md
---

# De-phasing Confirms The Nested-Instrument Echo Directly — 756 ns Was ~80

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

**The echo's size is measurable two ways, and they bracket rather than
coincide.** This section first claimed `671` against `677` — a `0.9%` agreement
— and that figure was an artifact of mixing conventions. Every consistent
reading:

| method | convention | result |
|---|---|---:|
| A: add two phase timers to the **draw** entry (`N=64`) | raw | `671 ns` |
| A: same, `N=16` | raw | `779 ns` |
| A: `N=64`, normalized for the `0.78%` slower drawphase run | frame-normalized | `577 ns` |
| B: de-phase the **const** entry | raw | `672 ns` |
| B: same | frame-normalized | `677 ns` |

The published `0.9%` came from normalizing B, not normalizing A, and quoting A's
`N=64` value while `.08` publishes `779` for `N=16` on the same line. The honest
result is **`~580-780 ns` for four clock reads, `~145-195 ns` each** — which
brackets the `170-188 ns` null read rather than sitting below it, so `.08`'s
follow-on claim that the nested reads cost "`~169 ns` each, not the null's
`180`" and that its arithmetic "over-corrected by `45 ns`" is false precision:
that `45 ns` is inside the method's own spread.

What survives, and is the only thing this needed to establish: **the echo is
large enough to account for essentially all of `756 - 80`**, by two mechanisms
that do not share an error mode. `entry_draw` also carries `~1.3 us/call` of
chunk-flush tails whose sampled incidence wanders run to run (see Scope), which
is why method A cannot be tightened by more runs of the same kind.

**Verdict.** `entry_const` is `~79-86 ns/call`, `~1.8 ms/present`. The published
`756 ns` / `16.50 ms` was **`8.9x` high** and is fully explained. `.08`'s
corrected numbers stand, with its const figure now measured rather than
inferred.

**The ENTRY share is `~41%`, and the two methods agree once both are corrected.**
This section first reported a gap — direct `42.8-43.6%` against arithmetic
`38.7%` — and attributed it to ambient. Both halves of that were wrong.

*The direct numerator contains the echo it just priced.* These de-phased runs
are built on the **drawphase** build (their logs carry `draw_swvp_sampled`), so
every `entry_draw` span holds the two phase timers' four clock reads — the exact
`~671 ns/call` method A measures. At `~1,700` draw calls that is
**`~1.15 ms/present` of instrument inside the direct share**. And `38.7%` was
stale: it belonged to the withdrawn intermediate const figure, and `.08`'s
current table says `40.6%`.

| | direct, as measured | echo-corrected | `.08` arithmetic |
|---|---:|---:|---:|
| r1 | `42.9%` | **`40.9%`** | |
| r2 | `43.6%` | **`41.6%`** | |
| r3 | `42.8%` | **`40.7%`** | |
| `N=64` locked run | | | **`40.6%`** |

**The methods agree to within `1 pp`.** Read the entry share as **`~41%`**.

*And the ambient table was computed against the wrong build.* Comparing the
de-phased runs to the `entry` build charged them for phase timers the `entry`
build does not have. Like-for-like against `drawphase-n64`:

| | frame | `entry_draw` | `entry_const` |
|---|---:|---:|---:|
| r1 | `1.069x` | `1.070x` | **`0.116x`** |
| r2 | `1.052x` | `1.088x` | **`0.115x`** |
| r3 | `1.045x` | `1.052x` | **`0.111x`** |

`entry_draw` tracks the frame to within `1-4%`, as an ambient slowdown should.
The first version's `1.13x`/`1.25x`/`1.50x` column was partly build difference
and, for the `10-25 ns` scopes, partly null-subtraction noise. **The corrected
table makes the de-phasing attribution stronger, not weaker**: the frame and
every real scope move together at `~1.05-1.09x`, and `entry_const` alone moves
`0.11x` in the opposite direction. The verdict was right; the table offered as
evidence for it was not.

**Scope, and what is weaker than it looks.** Three runs, `N=64` only, GT2 only,
all inside one thermal window and not interleaved against a same-window control.

*The `const ÷ state` control is a ratio of a well-measured number to a badly
measured one.* `entry_state` reads `98.6 / 70.3 / 80.4 / 58.5` across the four
locked runs and `100.9 / 115.1 / 116.6` here — a **2x spread**, and `.08`
publishes only the two entry-build values (`99` / `80`). Take the drawphase
runs' `58-70 ns` instead and the "const < state" ordering flips. The const
figure does not depend on it: `entry_const` has 12x the samples and a `5%`
cross-run spread (`81.5-85.8`), so it stands on its own precision, not on the
control. A `~100 ns` scope is at this instrument's resolution and should not
carry an argument.

*The `append` scope does not replicate at fixed `N`.* Two `N=64` runs with
identical instrumentation in that scope read `2,121` and `2,496 ns` — `17.7%`
apart, larger than the `12.8%` cross-rate gap `.09` attributes to `N | 64`
aliasing. The sampled flush share wanders `1.27 / 1.67 / 1.70 / 1.77 / 1.88%`
across five `N=64` runs, so the bias is not a function of `N` alone and the run
`.09` was built on holds the lowest share of the seven. The aliasing mechanism
is real; its stated size is not established. Deriving the flush share from
`bridge_commit_chunk` counters, or choosing an `N` coprime to 64, would settle
it — neither was done here.

**Related.**
[append-decomposition.08](state-churn-encode-append-decomposition.08.md) ·
[append-decomposition.09](state-churn-encode-append-decomposition.09.md) ·
[attribution.04](../present-pacing/present-pacing-post-defselect-cpu-attribution.04.md) ·
[frame-lifecycle](../frame-lifecycle.md) ·
[state-churn-encode](index.md)
