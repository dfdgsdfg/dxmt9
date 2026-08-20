---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 27
title: DOD Batch + Recorder-Mutex Gating — Correctness Kept, FPS Claim Retracted Into Layout Noise
date: 2026-08-20
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05-mutex-{a1,a2,a3,b1,b2,b3,m1,m2}
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.26.md
---

# DOD Batch + Recorder-Mutex Gating — Correctness Kept, FPS Claim Retracted Into Layout Noise

**What shipped.** From the [.26] PE-side DOD audit: per-call `Rect` heap
allocation removed at all seven sites (`08f09a5b`, `b56a3b44` — lock, present,
StretchRect, ColorFill; callee non-retention verified per site), dead
`lookupCachedWireObjectRef` removed, and the PE recorder `recursive_mutex`
gated on `D3DCREATE_MULTITHREADED` (`b96fdbda`) with native semantics: no flag
⇒ no lock, one cached-bool branch per site, a debug-only thread-confinement
`DXMT_ASSERT`, and the `DXMT9_PE_FORCE_RECORDER_LOCK` insurance env. 3DMark05
passes `behavior=0x40` (no MULTITHREADED), so GT2 takes the unlocked path.
Conformance: clean full suite 234/235 (known xyzhw only) at the gated head.

**The measurement, honestly.** Three-arm GT2 bracket
(A=head, B=`ed1d79db` pre-batch, M=`b56a3b44` Rect-only midpoint; prebuilt
worktrees, sha-verified, zero GPU errors):

| arm | mean fps (runs) | median fps |
|---|---|---|
| B (pre-batch) | 28.37 / 28.32 / 28.45 | 30.17 / 30.07 / 30.23 |
| M (Rect only) | 28.26 / 28.30 | 29.91 / 29.92 |
| A (Rect+mutex) | 28.24 / 27.99 / 28.07 | 29.90 / 29.61 / 29.68 |

A measures ~−1.0% mean / −1.4% median vs B, arms barely non-overlapping — but
M, whose change is strictly *less work* (heap allocations deleted, nothing
added), also lands below B. A strictly-less-work build measuring slower is the
signature of **per-build code-layout variance**, which at this scale (~±0.5-1%)
dominates any real effect of removing an uncontended lock (~5,400
lock/unlock ops/present ≈ 0.1-0.2 ms predicted, ≈0.3-0.5%). No mechanism-level
regression is attributable; no win is claimable either. Verdict: **keep the
changes for correctness and semantics, retract the performance expectation.**

**Attribution correction for the audit's #1.** The sampler row that sized the
mutex finding was `std::__1::mutex::unlock` — a *plain* mutex, while the
recorder lock is a `recursive_mutex`. That sampler run also ran at info log
level; the likely owner of the plain-mutex row is the logger's serialization
under info logging — a diagnostic-run artifact, not a production cost. Two
lessons for the ledger: (1) a symbol bucket must match the *exact type* of the
suspected lock before it sizes a finding; (2) self-PC shares from an
info-level sampler run carry logging overhead that does not exist in
production runs.

**Where this leaves the ledger.** The mutex lane is closed (correct-by-
semantics, perf-neutral). The remaining sized candidates from [.26] stand:
`mark` phase 0.888 ms, `surface_lock_rect` unix CPU 0.58 ms, retainer/builder
O(1) index ~0.25 ms — each predicts ≥2× the layout-noise floor, so their A/Bs
remain decidable; anything predicting &lt;0.3 ms on GT2 should not be measured
by whole-build A/B at all (pair it with counter or sampler evidence instead).
