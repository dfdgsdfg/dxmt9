---
domain: present-pacing
workload: STALKER CoP Day high 1280x720 late spatial window
title: "Clear fusion passes the matched live Encode CPU screen"
type: evidence
status: matched-live-cpu-screen-passed-default-owner-open
updated: 2026-10-16
source: traces/stkcop-clear-live-h559/{contract.md,build-receipt.json,verification.json,preflight-verification.json,inputs.json,clocks.json,run.py,analyze_run.py,compare.py,comparison.json,verify_results.py,raw-verification.json}; traces/stkcop-clear-live-h559/{A0,A1,A2,B1,B2,A3}/{receipt.json,analysis.json,catalogue-result.json,post-tail.png,host-before.txt,host-after.txt,restore/restored.json,recovered.json}
related: present-pacing-clear-failure-boundary.558.md; present-pacing-clear-net-economy.557.md; present-pacing-boundary-comparison.534.md; ../../../specs/backend/gap.md
---

# Positive live Encode CPU saving, not an FPS or global-owner verdict

The frozen six-run population completes in order **A0, A1, A2, B1, B2, A3**.
A uses the H551 serial image; B includes H558's Clear fusion, typed failure
relays and shared failure drain. Both use the same prefix-OFF runtime,
boundary-only PE and existing sparse Replay / per-source Encode self-CPU
observers. The runtime archive differs in exactly the three candidate objects.
This is a two-image net comparison, not a same-binary switch comparison.

Whole late-window Encode CPU passes the predeclared finite screen:

| Comparison | Encode CPU reduction, ms/observed completed frame |
|---|---:|
| A2 minus B1 | **1.579–2.010** |
| A3 minus B2 (reverse order) | **1.614–2.072** |
| Maximum A0/A1 endpoint difference | **1.189** |

Both lower reduction bounds exceed the A/A screen threshold. The bounds reflect
raw endpoint uncertainty, **not confidence intervals or a universal guaranteed
saving**. This is new live evidence; it does not reuse H557's offline 1.9955 ms
figure. No attempts were added, retried or excluded after seeing CPU results.

## Measured owners

All values below are cumulative **calling-thread CPU** over spatial knots 1..6,
divided by observed completed frames. Threads overlap; do not sum their costs
as frame latency. Mixed Primary includes game, Wine, PE and synchronous Unix.

| Run | Frames | Replay ms/frame | Encode ms/frame | Mixed Primary ms/frame | Observed late cadence FPS |
|---|---:|---:|---:|---:|---:|
| A0 | 124 | 27.546–27.859 | 28.243–28.302 | 46.623–47.409 | 18.202–18.209 |
| A1 | 123 | 29.062–29.357 | 29.368–29.432 | 50.189–51.009 | 18.321–18.327 |
| A2 | 122 | 29.756–30.157 | 30.143–30.317 | 49.971–50.819 | 18.074–18.082 |
| B1 | 121 | 29.560–30.013 | 28.307–28.564 | 49.579–50.430 | 17.699–17.707 |
| B2 | 117 | 30.153–30.583 | 29.669–30.052 | 52.866–53.773 | 17.125–17.133 |
| A3 | 115 | 31.163–31.525 | 31.665–31.741 | 52.844–53.778 | 16.414–16.420 |

The baseline dedicated workers are close, not separated by a large factor.
Encode exceeds Replay's upper bound in A0/A1/A3; A2 bounds narrowly overlap.
In both candidate runs, Replay's lower bound exceeds Encode's upper bound.
This supports **shifting attention toward Replay after the Encode removal**,
not a claim that Encode was the sole throughput limiter.

Neither Replay nor mixed Primary passes its own reduction screen. All five
smaller component windows are retained; none passes the Encode screen in both
orders. Do not select a favorable component or claim uniform savings across
every part of the tail. Whole-scene CPU totals and camera-window durations are
also retained in `comparison.json`, rather than only normalized values.

## Qualification and limits

- Sikarugir CX24.0.7, Day/high 1280x720, max-threads 2, snd_targets 16,
  commandlimit 0, production-counter query. These are controlled experiment
  policies, not every untuned application default.
- All six profiles report perf, validation 0 and perf-counters 0. Fusion witnesses
  and product per-command clocks are OFF during timing. Both linked preflight
  arms preserve original capture pixels/source/query/wrappers; B witnesses 65
  fusions there. Live timing does not log fusion counts.
- All seven spatial knots qualify, with process/birth/TID identity and zero
  dropped/faulted worker records. `verify_results.py` independently recomputes
  **108 raw thread intervals**, paired reduction arithmetic and the finite
  screen without importing the analysis helpers.
- All games exit 0. Catalogue exit 1 contains only `missing_capture`, covered by
  the frozen administrative qualification. This is not a catalogue pass.
- Each run restores 14 shared files hash-identically and recovers 4 temporary
  service files after natural process/server exit. No restoration amendment or
  forced engine termination was needed.
- A0 has a one-second loading sample before the running scene and far before
  knot 0. No sampling, build or test runs inside the selected timing windows.
  Post-tail A0/B1 screenshots are visually coherent, not a whole-run pixel oracle.

There is material run-order drift: baseline Encode rises from 28.24 to 31.74
ms/frame and cadence falls across the population. Outside-window host censuses
also show `mds_stores` near 80% before B1, 52% before B2 and 88% before A3. They do
not establish in-window contention, nor exclude it. No host CPU is subtracted,
and the paired CPU screen does **not** establish a clean FPS improvement,
thermal independence or an observer-free canonical effect. No README update
or default promotion follows.

## Remaining full objective

This closes the previously missing **matched-observer live net-cost screen**
for Clear fusion. It does not close the global default-path owner question.
Current Primary reports cannot isolate renderer CPU; H534's older all-root
upper envelope is useful prior evidence, not a current measurement. Other
renderer workers and the common observer's transfer still need coverage.

The next ownership check should bound current Primary renderer work and
remaining workers against the near-equal Replay/Encode costs, then qualify
low-frequency/endpoint observer transfer. Do not repeat H557's offline timing
or H559's unchanged six-run population to manufacture a stronger result.
The goal remains active; canonical sources, defaults and release binaries are
unchanged.
