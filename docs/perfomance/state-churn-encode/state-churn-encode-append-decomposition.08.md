---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 08
title: dxmt9's D3D9 Entry Points Are ~38% Of The Frame — The 15.1% Floor Was ~2.5x Low
date: 2026-08-01
type: experiment-run
status: accepted-attribution; const-setter figure corrected 2026-08-01 after review
source: experiments/output/app-d3d9-3dmark05-gt2-entry-n64; experiments/output/app-d3d9-3dmark05-gt2-entry-n16
related: docs/perfomance/frame-lifecycle.md; docs/perfomance/present-pacing/present-pacing-post-defselect-cpu-attribution.05.md
---

# dxmt9's D3D9 Entry Points Are ~38% Of The Frame — The 15.1% Floor Was ~2.5x Low

> **Corrected 2026-08-01, same day, after adversarial review.** This leaf first
> published `68%` and `4.5x`. The const-setter entry scope was inflated ~13-30x
> by a nested instrument that samples the same calls it does — see
> [The const-setter number was mostly clock](#the-const-setter-number-was-mostly-clock)
> below, which is the more useful half of this document. The draw and state
> figures survive unchanged; the headline does not.

**Question / hypothesis.**
[frame-lifecycle](../frame-lifecycle.md) reports dxmt9's PE recording at
`8.07 ms/present` (`15.1%`) and calls it a **floor**: four instrumented scopes,
with everything else in the producer thread a residual.
[attribution.05](../present-pacing/present-pacing-post-defselect-cpu-attribution.05.md)
then established the floor cannot be tightened from outside — xctrace names 564
images and neither the game nor our `d3d9.dll` is among them, because both are
translated PE code. Inside is the only direction left. How far is the floor from
the truth?

**Method.** Three new decimated scopes at the **D3D9 entry** level — the whole
call the application made — covering the hot families: const setters (VS/PS
F/I/B), draws (`DrawPrimitive`/`DrawIndexedPrimitive` + UP), and state setters
(`SetRenderState`, `SetTexture`, `SetTextureStageState`, `SetSamplerState`), 14
entry points in total. `entry - (inner scopes it contains)` is then the PE layer
nothing has ever measured.

A helper, `dxmt9PeArmDecimatedScope`, now arms a scope in one statement
*including the null-scope calibration read*. That is deliberate: the instrument
costs `~180 ns` per sample, which is `92%` of `touchConstShadow`'s reading
([attribution.04](../present-pacing/present-pacing-post-defselect-cpu-attribution.04.md)),
so a scope added without calibration is wrong by more than it measures.

**Result**, after subtracting both the scope's own calibration and any nested
instrument (see below):

| entry scope | calls/present | ns/call | `N=64` | `N=16` |
|---|---:|---:|---:|---:|
| draws | `1,696` | `11,493` / `11,583` | **`19.49 ms`** | **`19.50 ms`** |
| const setters | `21,827` | `34` / `57` | `0.74` | `1.22` |
| state setters | `1,903` | `99` / `80` | `0.19` | `0.15` |
| **ENTRY total** | | | **`20.42 ms`** | **`20.87 ms`** |
| mean frame | | | `52.74 ms` | `54.61 ms` |
| **share of frame** | | | **`38.7%`** | **`38.2%`** |

**Verdict.** Against a `~53 ms` frame, **time inside dxmt9's D3D9 entry points is
`~38%`**. The four scopes behind the `15.1%` figure captured `5.9-6.8 ms` of the
`20.4-20.9 ms` actually spent there — **the floor was low by about `2.5x`**, and
the PE layer nothing had ever measured is **`~14.5 ms/present`**, `~27%` of the
frame.

The two decimation rates agree within `1.3%` on the total. And the workload is
**not perturbed beyond noise**: mean scene fps `18.96` (`N=64`) / `18.31`
(`N=16`) against three uninstrumented baselines at `18.35` / `17.79` / `18.18` —
the `N=64` runs are on the *fast* side of every baseline. The `N=64`→`N=16`
gradient, `-3.4%`, is the instrument's real cost at 4x the sampling rate, and it
is the same size as the baseline's own run-to-run spread (`18.35`→`17.79`,
`-3.1%`), which is why neither shows up as a perturbation against the baseline.

## The const-setter number was mostly clock

`entry_const` first published `756 ns/call` = `16.50 ms/present`. It is **under
`60 ns`**. The cause is a property of deterministic decimation that the
[04 correction](../present-pacing/present-pacing-post-defselect-cpu-attribution.04.md)
did not anticipate, and it is the durable lesson here.

`shouldSample` is `++events; return events % n == 0` — deterministic, not
random. `touchConstShadow` is called exactly once per const-setter entry, so its
event counter advances in **lockstep** with the entry scope's. The logs prove
the lockstep exactly: `entry_const_events == const_setter_events` (`24,883,144`)
and `entry_const_sampled == const_setter_sampled` (`388,799`), at both rates.

So every sampled entry span contains the inner scope's **entire instrument** —
its calibration pair, its `t0`, and its destructor read, four clock reads — while
the entry's own null subtraction removes **one**. Varying `N` cannot expose it:
the coincidence is total at every `N`, so the `1.9%` cross-rate agreement the
first version cited as evidence was agreement between two equally-biased numbers.

The echo's price is measurable, not assumed. The `drawphase` build differs from
the `entry` build by exactly two phase timers inside the draw entry — four clock
reads. The draw entry moved `11,493 → 12,164 ns` (`N=64`) and
`11,583 → 12,362` (`N=16`): **`+671` / `+779 ns` for four reads**, i.e.
`168-195 ns` each, matching the null readings (`170-183 ns`). Subtracting four
of those from `entry_const` leaves `3-57 ns` across the four available runs.

Two independent checks agree it is that small. `entry_state`, a scope of nearly
identical shape with **no** nested instrument, measures `99 ns` — a const setter
whose only extra work is a `24 ns` `touchConstShadow` could never have cost
`7.6x` a state setter, and the first version never confronted its own number.
And the `~3 ms/frame` of game-side CPU that `68%` implied was never plausible.

**Fixed, not just documented.** `PeDecimatedScopeStats` now carries a
`phaseOffset`, `shouldSample` tests `events % n == phaseOffset % n`, and
`peConstSetterDecimatedStats()` is phase `1`. Two lockstep scopes with distinct
phases can never sample the same call for any `N >= 2`. Pinned by
`dxmt9-core-device-com-spec`, which also asserts the *old* shape coincides on
every sample so the regression is visible rather than silent.

Deliberately parent-gated sub-scopes (`DxmtPeDecimatedPhaseTimer`, used by `.09`)
are a different thing and stay: they cost their parent one clock pair each,
which is a known constant, not a hidden whole-instrument echo.

**What did not change.** `entry_draw` nests the append scope at a ratio of
`1.606:1`, not `1:1`, so its phase is quasi-random and the expected echo is
`~27 ns` against `11,493` — under `0.3%`. `entry_state` nests nothing. Those two
figures, and everything `.09` derives from the draw entry, stand as published.

## The H212 reconciliation is withdrawn

The first version argued this result "reconciles with H212 rather than
contradicting it", since `36 ms` of entry time against a `73.5%` guest blob made
the PE side most of the blob. That paragraph is retracted, and not only because
the `36` is now `20`. It compared across three axes at once:

- **Workload.** H212 is **GT1**. This is GT2, and `frame-lifecycle`'s own header
  says the per-stage numbers do not transfer between them. There is no GT2 blob
  measurement to reconcile against — the one attempt returned 100%
  `<unknown-binary>` ([attribution.05](../present-pacing/present-pacing-post-defselect-cpu-attribution.05.md)).
- **Currency.** H212 is xctrace CPU-sample attribution; these scopes are
  on-thread wall time, which includes preemption and blocking.
- **Bucket.** Entry wall includes the chunk seal and the wow64 crossing —
  `append_flush` is `65.7 us` × `~34.6` flushes/present ≈ `2.3 ms/present`
  *inside* `entry_draw`, of which `~1.44 ms` is the unix synchronous commit half
  already counted separately as `offload_commit_app_cpu_ms`. H212 put that class
  in its wow64 and winemetal buckets, not the guest blob.

**What this does not say.** `38%` is *time inside our entry points*, not `38%`
of removable overhead. Argument validation and state bookkeeping are work any
D3D9 implementation performs; the question this opens is how much of the
`~14.5 ms` residual is necessary —
[.09](state-churn-encode-append-decomposition.09.md) answers `12 ms` of it.

**Where to look next, quantified.** The draw entry point is `11,493 ns/call` —
`19.5 ms/present`, `37%` of the frame **on its own**, against `1,760 ns` for the
`appendRecordDirect` inside it. Ruled out already by reading the code: the
per-call bookkeeping (`notePeDeviceCallAfterPresent`, `recordPeBetweenCallsEntry`,
`logPeCallMilestoneAfterPresent`) is gated with early returns, and
`dxmt9DeviceDebugLog` early-outs on `shouldLog` before formatting — neither is
a repeat of the `.07` ungated-work pattern.
[.09](state-churn-encode-append-decomposition.09.md) takes it from here.

**Scope.** One run per decimation rate, GT2 only. Per-call figures are
differences against a `~180 ns` calibration *and*, where a scope nests another,
against the nested instrument: at `11,493 ns` the correction is `1.6%` of the
reading and at `936 ns` it is `96%`, so the draw figure is solid and the
const-setter figure should be read as "too small for this instrument to
resolve", not as `34 ns` precisely. `append`'s own reading disagrees `12.8%`
across rates because both `64` and `16` divide the 64-record chunk period, which
aliases the once-per-64 flush tail; the flush-free means agree within `1%`.
Deterministic every-Nth sampling of anything with period-64 structure is biased
whenever `N | 64`.

**Related.**
[attribution.05](../present-pacing/present-pacing-post-defselect-cpu-attribution.05.md) ·
[append-decomposition.09](state-churn-encode-append-decomposition.09.md) ·
[append-decomposition.07](state-churn-encode-append-decomposition.07.md) ·
[frame-lifecycle](../frame-lifecycle.md) ·
[state-churn-encode](index.md)
