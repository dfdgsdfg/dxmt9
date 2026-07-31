---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 07
title: Gating Two Wasted Clock Reads Removes 12% Of Append — And The FPS A/B Could Not Have Seen It
date: 2026-07-31
type: experiment-run
status: accepted-mechanism; fps-inconclusive-underpowered
source: experiments/output/app-d3d9-3dmark05-gt2-clockgate-before-r{1,2,3}; experiments/output/app-d3d9-3dmark05-gt2-clockgate-after-r{1,2,3}; experiments/output/app-d3d9-3dmark05-gt2-mech-before; experiments/output/app-d3d9-3dmark05-gt2-mech-after
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.03.md; docs/perfomance/frame-lifecycle.md
---

# Gating Two Wasted Clock Reads Removes 12% Of Append — And The FPS A/B Could Not Have Seen It

**Question / hypothesis.** An adversarial review of the frame model found that
`appendRecordV2` called `steady_clock::now()` twice per append
**unconditionally**, while all four consumers of those timestamps early-out on
`dxmt9PeRecorderStatsEnabled()` — off in every perf and production run. Predicted
cost: `2,707` appends/present x 2 x `~187 ns` = `~1.0 ms/present`, `1.9%` of the
frame. Gated in `3c67770b`. Does it convert?

**Method.** Two experiments at `bb109a91` (before) against `3c67770b` (after),
both prebuilt in separate worktrees and switched with `--build-root` so no
compile lands before a run, both trees verified `ninja -n` clean.

1. **FPS**: interleaved A/B/A/B/A/B, three runs per side, `240 s` cooldown each,
   GT2 `perf`, frame sampling, no gputrace.
2. **Mechanism**: one run per side with `DXMT9_PE_STATS_DECIMATION=64`. The
   return-side clock read sits *inside* the decimated append scope, so gating
   must drop the measured per-append cost by about one clock call. The
   entry-side read is outside the scope and is invisible to this method.

**Result — the mechanism works, and by more than predicted.**

| | appends/present | append corrected | null scope |
|---|---:|---:|---:|
| before | `2,729` | `2,798 ns` | `199 ns` |
| after | `2,700` | **`2,463 ns`** | `192 ns` |

`-335 ns` per append, **`-12.0%`**. That is `~1.7` clock calls' worth against one
predicted — same order, right direction, and more than the naive estimate
(removing the read also removes a branch and changes what inlines). Against
`2,700` appends/present this is **`0.90 ms/present` measurably removed**, plus
the entry-side read this method cannot see (another `~0.5 ms` on the same gate).

**Result — and the FPS A/B is a non-result, not a null.**

| run | median scene fps |
|---|---:|
| before r1 / r2 / r3 | `19.327` / `18.980` / `18.365` |
| after r1 / r2 / r3 | `19.402` / `18.808` / `18.539` |

Median-of-medians `18.980 -> 18.808` (`-0.91%`), mean-of-medians `+0.14%`,
spreads fully overlapping, exact permutation `p = 9/20 = 0.45`.

**The honest reading is that this experiment could not have detected the effect
it was testing for.** Within-side spread was `0.96 fps` (~`5%`); the predicted
effect is `1.7-2.6%` of frame. The measurement floor is twice the signal. For
contrast, the Step 7 A/B
([.02](state-churn-encode-append-decomposition.02.md)) resolved `+2.1%` because
its within-side spread was `0.22 fps` (~`1.2%`) — these six runs came after
eight hours of continuous benchmarking on the same machine, and it shows.

**Verdict.** Mechanism ACCEPTED: `~0.9 ms/present` of provably wasted work is
gone, at zero risk and zero behaviour change (the gate is a cached static bool;
the stats path is untouched when the flag is on). FPS INCONCLUSIVE — and
specifically **not** evidence for or against a CPU-to-wall conversion ratio,
which is what this run was also meant to settle. Reporting it as a null would
repeat exactly the error this leaf exists to correct: H214's `+1.6%` was called
"inside the noise band" without establishing that the band was narrower than the
effect.

**What this does settle.** The frame model's claim to have priced the remaining
cost "item by item" was false: this was `~1.9%` of frame sitting in the
production hot path, larger than the queue-mutex contention that received a
document of its own, and **half of it was structurally invisible** — the
entry-side read is outside the decimated append scope, so it never appeared in
the `8.07 ms` PE recording figure at all. Any remaining-cost claim built on
those four scopes is a floor, not a total.

**What would settle the FPS question**, in order: re-run the same A/B on a cool
machine with at least five runs per side; or measure on a workload whose
within-side spread is smaller than `2%`.

**Scope.** Mechanism is n=1 per side — acceptable because the effect (`-12%`) is
far outside the `N=64`/`N=16` agreement band that
[.03](state-churn-encode-append-decomposition.03.md) established for this
instrument (`2.2%`), but it is still one run. The `-335 ns` is measured inside
the append scope only; the total removed is inferred, not measured.

**Related.**
[append-decomposition.03](state-churn-encode-append-decomposition.03.md) ·
[frame-lifecycle](../frame-lifecycle.md) ·
[state-churn-encode](index.md)
