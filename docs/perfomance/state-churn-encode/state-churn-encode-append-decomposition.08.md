---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 08
title: dxmt9's D3D9 Entry Points Are 68% Of The Frame — The 15.1% Floor Was 4.5x Low
date: 2026-08-01
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05-gt2-entry-n64; experiments/output/app-d3d9-3dmark05-gt2-entry-n16
related: docs/perfomance/frame-lifecycle.md; docs/perfomance/present-pacing/present-pacing-post-defselect-cpu-attribution.05.md
---

# dxmt9's D3D9 Entry Points Are 68% Of The Frame — The 15.1% Floor Was 4.5x Low

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
so a scope added without calibration is wrong by more than it measures. Making
it a helper rather than a comment is what stops the next scope from skipping it.

**Result.**

| | `N=64` | `N=16` |
|---|---:|---:|
| **ENTRY total** | **`36.18 ms/present`** | **`35.52`** |
| inner total (the four original scopes) | `6.94` | `7.68` |
| **residual — never before measured** | **`29.24`** | **`27.83`** |
| scene fps | `19.56` | `18.88` |

Per family, `N=64`:

| entry scope | calls/present | corrected ns/call | ms/present |
|---|---:|---:|---:|
| draws | `1,696` | `11,493` | **`19.49`** |
| const setters | `21,827` | `756` | `16.50` |
| state setters | `1,903` | `99` | `0.19` |

**Both discipline checks pass.** The two decimation rates agree within `1.9%`,
which is this instrument's stability criterion. And the workload is **not
perturbed**: scene fps `19.56` / `18.88` against a `18.35-18.51` uninstrumented
baseline — the instrumented runs are, if anything, on the fast side of noise.

**Verdict.** Against a `~53 ms` frame, **time inside dxmt9's D3D9 entry points is
`~68%`**. The four scopes behind the `15.1%` figure captured `6.9-7.7 ms` of the
`35.5-36.2 ms` actually spent there — the floor was low by about `4.5x`.

This reconciles with H212 rather than contradicting it. That attribution put the
"guest blob" at `73.5%` of the producer wall and said in its own text the blob is
"game x86 code **plus our 32-bit PE d3d9.dll recording path**". Measured from
inside, the PE part turns out to be most of the blob — which is exactly what
attribution.05 predicted would be invisible from outside.

**What this does not say.** `68%` is *time inside our entry points*, not `68%`
of removable overhead. Argument validation and state bookkeeping are work any
D3D9 implementation performs; the question this opens is how much of the
`27.8-29.2 ms` residual is necessary. The entry scopes also span whatever
blocking happens inside a call (a draw that seals a chunk pays the bridge
commit), and nested sampling inflates the outer scope by the inner scope's own
clock pairs — small against `11.5 us`, but not zero.

**Where to look next, quantified.** The draw entry point is `11,493 ns/call` —
`19.5 ms/present`, `37%` of the frame **on its own**, against `2,121 ns` for the
`appendRecordDirect` inside it. Reading the entry: every draw constructs a
`SoftwareFfpDrawData` and a `std::vector<std::uint8_t>` and runs two SWVP
fallback probes (`trySoftwareFfpDrawIndexedPrimitive`, then
`trySoftwareProgrammableDrawIndexedPrimitive`) before any recording happens.
Ruled out already by reading the code: the per-call bookkeeping
(`notePeDeviceCallAfterPresent`, `recordPeBetweenCallsEntry`,
`logPeCallMilestoneAfterPresent`) is gated with early returns, and
`dxmt9DeviceDebugLog` early-outs on `shouldLog` before formatting — neither is
a repeat of the `.07` ungated-work pattern.

**Scope.** One run per decimation rate, GT2 only. The corrected per-call figures
carry the usual caveat that they are differences against a `~180 ns`
calibration; at `756 ns` and `11,493 ns` the correction is `24%` and `1.6%` of
the reading respectively, so the const-setter figure is the softer of the two.

**Related.**
[attribution.05](../present-pacing/present-pacing-post-defselect-cpu-attribution.05.md) ·
[append-decomposition.07](state-churn-encode-append-decomposition.07.md) ·
[frame-lifecycle](../frame-lifecycle.md) ·
[state-churn-encode](index.md)
