---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 19
title: Thread Attribution — The Game Thread Is Genuinely Saturated, And The Measurement Host Is Not Clean
date: 2026-08-18
type: experiment-run
status: accepted-attribution
source: traces/app-d3d9-3dmark05-threadattr-r2; experiments/output/app-d3d9-3dmark05-threadattr-r2
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.18.md
---

# Thread Attribution — The Game Thread Is Genuinely Saturated, And The Measurement Host Is Not Clean

**Question.** [.18](state-churn-encode-append-decomposition.18.md) ended with
two hypotheses for "2 ms of producer CPU removed, zero fps": game logic owns
the frame, or a pacing wait absorbs the savings. Which?

**Method.** Mid-scene xctrace Metal System Trace (`--record-delay-sec 35
--time-limit-sec 20` — the sidecar's default 75 s delay lands after GT2's
scene ends near 81 s and captures nothing; that first empty attempt is
`traces/app-d3d9-3dmark05-threadattr-r1`), `time-profile` table parsed
directly because the CPU summarizer's `3DMark05\.exe` process filter does not
match this environment's process naming. An earlier `/bin/ps -M` sampling
attempt cross-checked the encode thread but is line-index-keyed and breaks
when threads come and go; treat the xctrace numbers as authoritative.

**Result 1 — the game thread is ~100% busy mid-scene** (20.3 s of CPU in the
20 s window; `dxmt9-encode` 22%, a second dxmt9 worker 19%). The
pacing-idle hypothesis is dead: the thread is not waiting, it is computing.

**Result 2 — what it computes is 95% opaque to the profiler.** 46% of samples
land in unsymbolized 32-bit code (game logic AND the PE d3d9.dll side are
both 32-bit and indistinguishable here) and 49% carry no backtrace at all.
Unix-side dxmt9 symbols on the game thread total under 1% (buffer lock,
commit sync half, upload) — consistent with the offload having moved replay
off this thread. QPC/`mach_continuous_time` reads are only 1.9%, which argues
against a spin-wait pacer (a time-polling spin would dominate the histogram).

**Result 3 — the measurement host is contaminated.** In both trace windows
macOS storage housekeeping burns about two cores continuously:
`StorageManagementService` (17.0 s in the 20 s window), 
`ApplicationsStorageExtension` (6.5 s), and five `fseventsd` client threads
(~3.2 s each). This is the session's phantom disk consumer (the volume has
been pinned near 100% for days) and a standing operational hazard for
future fine-grained comparisons (see the verdict for why it does not
explain the .18 result).

**Verdict.** The .18 puzzle narrows to two explanations, and they are not
equally live. The contamination-masking hypothesis is undercut by the .18
data itself: within-config ABBA repeatability was 0.1-0.3%, so the run-time
noise floor was low — a constant background load slows both arms equally and
cannot hide a 5% delta, and a bursty one would have scattered the repeats.
The leading explanation is therefore that the decimated flush-savings
estimate overstates the removal (the documented N=64 chunk-period aliasing:
at the default cadence the sampled flush population aligns with the 64-record
chunk period). Host contamination remains an operational hazard rather than
the explanation of record. Both still point the same way operationally: (a) no further
producer-CPU experiments until the host is clean (free tens of GB so the
storage daemons settle), and (b) separating game logic from the PE layer on
this thread requires symbolizing the 32-bit PE side in traces or extending
the decimation entry coverage; the unix side is already proven negligible on
this thread. The cadence knob stays unpromoted; the parallel-lane parking
verdict is unaffected (encode-side facts were measured on the same
contaminated host but with within-config repeatability that the conclusion
does not exceed).
