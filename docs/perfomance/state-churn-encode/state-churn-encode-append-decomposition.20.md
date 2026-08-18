---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 20
title: Cadence Re-Verification — Aliasing Refuted, The Saving Is Real, FPS Conversion Is Environment-Split
date: 2026-08-18
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05--reverify-base; experiments/output/app-d3d9-3dmark05--reverify-cand
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.18.md; docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.19.md
---

# Cadence Re-Verification — Aliasing Refuted, The Saving Is Real, FPS Conversion Is Environment-Split

**Question.** [.19](state-churn-encode-append-decomposition.19.md) promoted
"the N=64 decimation overstated the flush saving (chunk-period aliasing)" to
the leading explanation of [.18](state-churn-encode-append-decomposition.18.md)'s
"2 ms removed, zero fps". Test it two independent ways in one pair of runs
(base 64/256 KiB vs candidate 256/1.25 MiB): decimation at the prime N=61
(no alignment with either chunk period), and the game thread's measured CPU
per frame from a mid-scene 20 s xctrace window.

**Aliasing: refuted.** N=61 reads the base flush at `84.6 µs`/seal,
`3.52 ms/present` — slightly *above* the N=64 readings (`66-68 µs`,
`2.91-3.47`), not below; candidate `218.6 µs`/seal, `2.28 ms/present`. The
N=64 numbers were not aliasing artifacts, and the cadence saving re-measures
at **`~1.2 ms/present`**.

**The saving is real on the thread.** Game-thread CPU per frame:
base `38.93 ms`, candidate `37.79 ms` — **`−1.14 ms/frame`**, agreeing with
the flush-scope estimate through a completely independent per-thread
measurement that constant background load cannot fake.

**FPS conversion: the two measurement days disagree.** In this pair the fps
moved with the CPU: `24.94 → 25.50` (`+2.2%`, expectation `+2.9%` from
`1.14/40 ms`), with the game thread at ~97% duty both sides. But the
[.18] clean ABBA (two runs per arm, within-config repeatability 0.1-0.3%,
no xctrace) measured `+0.2%` — flat — at `26.7-26.8` fps. Today's environment
is ~7% slower overall (heavier host contamination and the added N=61 +
xctrace overheads), and today's evidence is a single pair. Neither result
can dismiss the other: the .18 ABBA is statistically stronger; today's pair
carries the mechanically stronger instrumentation (per-thread CPU).

**Verdict.** The .18 puzzle's answer is NOT measurement error: the producer
saving exists on the game thread in both protocols. What remains open is
purely whether it converts to frame rate, and that answer appears
environment-sensitive. The deciding experiment is a clean-host ABBA
(three runs per arm, no decimation, no xctrace, storage daemons settled
after real disk headroom is restored). If the `+2-3%` conversion reproduces
there, the cadence knob graduates from diagnostic to a promotion candidate —
which then needs the standard visual/locality gates, plus an explicit
review of what a 4x publish granularity does to downstream pacing —
otherwise .18's flat result stands and the saving is confirmed as absorbed.
Until that experiment, the knob stays diagnostic and unpromoted.
