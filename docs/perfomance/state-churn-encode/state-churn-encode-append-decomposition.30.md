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

**FPS evidence: prediction-matching, borderline-separable.** The first ABBA
batch was invalidated outright (Spotlight `mdworker` at 22% CPU, two baseline
runs dead with `missing_capture`); a quiet-guarded rerun was directionally
positive (+3.6%/+5.6%) but the afternoon machine ran ~10% slow with 5-8% arm
spread. The decisive step was **characterizing the environment with a
B-only×3 block first**: once B measured 27.90/28.02/28.06 (0.6% spread, back
at the overnight class), an A×3 block plus closing B bracket gave
**A 28.24±0.23 vs B 27.89±0.21 → +1.22% mean / +0.53% median** — squarely
inside the +1.1-1.4% the measured producer recovery (~0.4-0.5 ms/present)
predicts, with the closing-B drift working against A (conservative). Three
independent batches all positive; effect size ≈2σ of arm noise, so the
scheduled overnight guarded pair remains the confirmatory sample. Verdict:
mechanism accepted, fps consistent-with-prediction.

Method note for the ledger: when a wild window is suspect, **measure one arm
repeatedly first** — an arm-internal spread is a direct read of the window's
noise floor and costs half an A/B; the afternoon's contaminated batches would
have been recognized before spending any A runs.

**Ledger after T2a'.** Remaining queue-mutex holders: worker slot-append copy
3.4-3.8 ms/p (T2d reserve-copy-commit, model required), slot publish 0.83,
encode submit 0.43, map finalize 0.28, producer capture 0.08 (T2b). Also
open: a restamp-fire counter (the ticket/slot-seq window is modelled but
unmeasured in the wild), and the R-VERIF-7.3 interleaving harness for the
atomics ordering.
