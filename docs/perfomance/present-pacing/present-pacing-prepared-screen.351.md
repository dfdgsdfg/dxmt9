---
domain: present-pacing
workload: STALKER CoP Day
title: "H351: Skip ineligible prepared shadow allocation"
type: evidence
status: partial-attribution
updated: 2026-09-21
source: experiments/output/prepared-screen-h351/{stalker-on.csv,stalker-on.json,native-tla.log,x86-tests.log,h350-backcompat.json}; experiments/output/app-d3d9-stkcop-bench-prepared-screen-h351-on-day/{result.json,actual.png,daybenchmark.result}
related: present-pacing-stalker-prepared-shadow.350.md
---

# H351: Pre-allocation screening

Parent `aad716ab` plus the screening implementation. Same Day/high,
renderer_r2.5, 1280x720, Sikarugir cx24.0.7 perf configuration as H350 ON;
capture delay 150s, bounded encoded Presents 2450–2470. Unix artifact SHA-256
prefix `0b8c429efd39bdd2`; all five loaded hashes are in `result.json`.
Builds and tests completed before measurement. Partition remains `identity`;
the diagnostic shadow builds/discards private plans, with no Encode consumer.

`screenPreparedEncodeShadow` rejects frozen query scopes, empty sources and
single commands with no possible two-draw child work before builder allocation.
A multi-draw single DrawRun remains a candidate: the parallel producer can
subdivide it. Acceptance is not an execution certificate.

## Actual-input result

Runner PASS, zero GPU command-buffer errors. Trace closes with zero overflow
or incomplete writers. The in-scene screenshot has no obvious black/repeated
geometry; it is not a pixel-exact comparison or full-run flicker proof.

| Disposition | Sources | Inclusive diagnostic CPU |
|---|---:|---:|
| Query-scope skip | 1,062 | 6.199 ms |
| No-parallel-work skip | 22 | 0.124 ms |
| Preparation candidates | 42 | 4.959 ms |
| All | 1,126 | 11.282 ms |

1,084 sources (96.27%) allocate no plan storage. The 42 remaining attempts
split into 21 successful structural plans and 21 capacity rejections. They
request 6,282,192 bytes (5.991 MiB cumulative, not RSS); the previous H350
unscreened live run requested 172.46 MiB. Within this run the screen avoids
1,084 fixed-size allocation attempts. It does not establish what their CPU
cost would have been under an unscreened counterfactual.

Attempt-phase totals: allocation/initialization 0.556 ms, projection 3.810 ms,
seal 0.166 ms. All-source total is 0.537 ms per selected encoded Present,
including metadata, native clock observations and destruction. Skipped
sources still pay identity/endpoint tracing cost. Phase sums exclude that
metadata work and must not be presented as the total.

H350 reported 30.605 ms versus this run's 11.282 ms, but source populations
differ (1,209 versus 1,126). This is mechanism evidence, not a fixed-input
speedup. Native benchmark average is 18.976261 FPS, lower than H350 ON's
20.690126; neither isolated live run qualifies performance causality. No FPS
improvement, no regression attribution, and no policy promotion are claimed.

## Verification and remaining work

- Native plan, shadow analyzer, parallel-pass spec and TLA regression: 4/4
  pass (TLA 233.98s).
- x86_64 plan, shadow analyzer and install-name audit: 3/3 pass.
- Unix, PE64 and PE32 canonical builds pass.
- Native OFF/ON queue fixtures preserve publication sequences/counts, with
  18 attempts and three explicit skips. The screen predicate's bounded truth
  table preserves a single 128-draw DrawRun candidate.
- Historical H350 transcripts remain readable without the new screen event.

Next: reusable/right-sized private storage and exact over-bound source
cardinality. Fused final-Slot construction, joint async publication/reclaim,
independent Encode differential and non-degenerate GPU oracle remain open.
The existing TLA run is regression evidence, not proof of those future paths.
