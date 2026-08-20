---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 30
title: T2a' Lands — Both Mark Paths Leave The Queue Mutex, Model-First
date: 2026-08-20
type: experiment-run
status: accepted-mechanism
source: experiments/output/app-d3d9-3dmark05-qmutex-t2a-r1; experiments/output/app-d3d9-3dmark05-t2b-{a1,a2,a3,b1,b2,b3}
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.29.md; docs/superpowers/specs/2026-08-20-producer-queue-concurrency-design.md
---

# T2a' Lands — Both Mark Paths Leave The Queue Mutex, Model-First

**What shipped** (`ced79f73`, model-first per the track's discipline). The seq
ticket became an atomic (`markTicketAcquire()`, writes stay under the mutex
with release ordering); both mark paths stamp via the pool's arena-stamp
exception outside `CommandQueue::mutex_`; and every lock-free ticket pairs
with `restampIfTicketAdvancedLocked` — under the re-acquired mutex the ticket
is frozen, so one re-read plus a conditional monotone re-stamp is a fixed
point, generalizing `forceDrawResourceMarkingAfterSplit_`.

**The model caught what the framing missed.** The extension
(`WorkerStampMark`, `SlotAdvance`, `commitSeqId`; production cfg 276,840
distinct states, depth 29) showed the stale-ticket hazard is a **premature
reclaim after the pins are legitimately gone** — `releaseRetainedWrappers`
runs post-replay but pre-GPU-completion, and from that point the seq stamp is
the only protection. `PinDiscipline` can never see it; it needed its own
`RestampDiscipline` axis, whose Buggy cfg produces the 9-step
`SlotAdvance → stale stamp → append under later seq → watermark passes →
Reclaim` violation. Three expected-failure configs now run in `verify_tla.sh`.

**Mechanism proof** (`qmutex-t2a-r1` vs [.29]'s r2):

| metric | before | after |
|---|---|---|
| worker mark segment | 0.93 ms/p | **0 (gone)** |
| producer mark acquire-wait | 0.591 | **0.194 (−67%)** |
| commit mark phase / mark_lock | 1.01 / 0.617 | 0.62 / 0.195 |
| queue total acquire-wait | 2.03 | 1.32 ms/p |

Conformance 234/235 (known xyzhw only). Full host suite 723/723.

**FPS evidence: directionally positive, not yet clean.** The first ABBA batch
was invalidated outright (Spotlight `mdworker` at 22% CPU, two baseline runs
died early with `missing_capture`, survivors depressed). A quiet-guarded
rerun (load<2, mdworker<5% for 3 consecutive minutes) measured
**A +3.6% mean / +5.6% median** over 3+3 interleaved runs — but the afternoon
machine ran ~10% below the overnight sessions with 5-8% within-arm spread and
one A/B overlap, versus the 0.3% spreads the overnight windows give. Verdict
stands at *mechanism accepted, fps claim pending an overnight-quality pair*;
the measured producer-side recovery (~0.4-0.5 ms/present) predicts +1.1-1.4%,
inside what the noisy batch shows but not separable from its drift.

**Ledger after T2a'.** Remaining queue-mutex holders: worker slot-append copy
3.4-3.8 ms/p (T2d reserve-copy-commit, model required), slot publish 0.83,
encode submit 0.43, map finalize 0.28, producer capture 0.08 (T2b). Also
open: a restamp-fire counter (the ticket/slot-seq window is modelled but
unmeasured in the wild), and the R-VERIF-7.3 interleaving harness for the
atomics ordering.
