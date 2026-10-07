---
domain: present-pacing
workload: STALKER Day high 1280x720 default-policy API residency diagnostic
title: "API residency bounds: successful reload-safe observation, unresolved default CPU owner"
type: evidence
status: diagnostic-only-perturbation-unqualified
updated: 2026-10-07
source: traces/stkcop-api-residency-h453/{api_scope.hpp,read_marker.c,analyze.py,instrumentation-r3.diff,reload-before-state.json,reload-after-state.json}; traces/stkcop-api-residency-h453/r3/{receipt.json,cpu-marker.jsonl,identity-join.json,identity.sample.txt,guest-identity-guest.txt,guest-identity-identity.txt,allocation-bounds.json,tail.png}; experiments/output/app-d3d9-stkcop-bench-api-residency-h453-r3/result.json
related: present-pacing-primary-clock-qualification.452.md; present-pacing-default-tail-cpu.449.md; present-pacing-default-tail-running.450.md; present-pacing-run-construction-removal.451.md
---

# API residency does not yet close default-path CPU attribution

The H453 r3 catalogue run completed with `status: pass`. It establishes
reload-safe API observation and matched native CPU endpoints, not an
uninstrumented owner ranking, removable CPU cost, FPS result, or GPU exact
comparison. The late screenshot shows the building scene; one still image is
not a temporal rendering oracle.

## Measurement boundary

Diagnostic-only scopes cover the 350 API/forwarding bodies inventoried in
H452. Shared TLS depth counts outermost calls only. Root QPC durations include
synchronous Wine/provider work and waiting; they exclude asynchronous
Replay/Encode. Native libproc thread CPU remains the CPU authority.

An atomic sequence reader accepts endpoints only when the marker remains
unchanged, inactive, even, and fault-free across the native CPU snapshot.
Guest Primary identity is checked twice using the engine thread start RVA
`0x6cdc1`. Native thread handles are joined to sample thread IDs through
unique names and bracketing monotone CPU counters. The analyzer accepts
1,119 of 2,012 observations. An accepted endpoint is not a CPU sample inside
an API: cumulative root residency counts all completed calls between endpoints.

The run uses Sikarugir CX24.0.7, Day/high, 1280x720, `-max-threads 2`,
`snd_targets 16`, command limit zero, and product counters/frame sampling OFF.
The x86 D3D9 diagnostic SHA prefix is `9aced54d061af27a`; the unchanged provider
is `7f45561ddf0fd22f`. The result contains all five staged image hashes.

The four analysis windows exclude the full sample/identity action
(105.099–113.583 seconds), scene screenshot (150.463–151.128), and tail
screenshot (185.510–186.082), relative to engine discovery. They are
elapsed windows, not cross-run source-identity matches.

## Matched diagnostic observations

One core means one CPU-second per elapsed second. API-body CPU bounds use
H452's allocation inequality and conservative CPU snapshot endpoint bounds.

| Requested window, seconds | Primary CPU, cores | API-body CPU bounds, cores | Replay CPU, cores | Encode CPU, cores | Root calls/s |
|---|---:|---:|---:|---:|---:|
| 125–145 | 0.937 | 0.321–0.385 | 0.535 | 0.567 | 318,372 |
| 155–170 | 0.921 | 0.259–0.338 | 0.520 | 0.532 | 307,461 |
| 187–193 | 0.981 | 0.270–0.289 | 0.510 | 0.509 | 277,576 |
| 205–212 | 0.897 | 0.164–0.268 | 0.461 | 0.463 | 249,090 |

In this instrumented run, the measured API-body CPU upper bounds lie below
each worker's CPU. This does **not** establish the same ordering for default
product code. Scope boundaries omit prologue/epilogue and some instrumentation
bookkeeping. Scheduling and workload progress can change under instrumentation.
The residual Primary CPU must not be labeled exclusively game/Wine.

Earlier empty-root calibration costs 281–537 ns/call. At these call rates,
that corresponds to roughly 0.07–0.17 seconds per second of elapsed observer
cost sensitivity. It is neither a measured workload CPU correction nor a
worst-case bound, and the calibration predates the final reload fix. Subtracting
it would create false precision. No OFF/ON perturbation qualification exists.

## Observer failures resolved, not renderer fixes

Earlier attempts observed only three roots or no accepted markers. X-Ray's
advanced-rendering capability probe loads D3D9, creates the factory, reads
caps, releases it, then unloads the DLL. A later DLL incarnation failed to
reopen the existing mapping. A three-load test observed 10/30 roots before
the fix and 30/30 afterward, with three attachment epochs and no faults.
The successful workload has a stable second attachment in the analysis windows.

X-Ray's legacy thread-name exception sets a native thread name but does not
populate the guest thread-description query. Empty guest names were not
evidence of a missing Primary thread. Invalid earlier observations are retained
and must not be interpreted as zero PE CPU.

## Decision and stop conditions

The largest default-path dxmt9 CPU owner and its removable cost remain open.
H451's approximately 1% captured-body saving is still the only directly
measured bounded removal here, and is not matched to this late scene.

Do not extend full per-call instrumentation or promote another policy on this
evidence. A useful next comparison must distinguish a substantial backend
CPU difference on matched late-scene work: same workload settings and Wine,
dxmt9 versus D9VK, observer-light native Primary/worker CPU and scene progress.
That comparison can locate a difference, not prove every differing cycle is
removable. If the difference cannot be localized beyond noise, stop micro-copy
experiments rather than claim a large architectural saving.

The Encode scratch-initialization candidate is also not a measured large
opportunity: H450 contains no samples at its seven removable zero-fill
callsites. Samples at the retained effective-state copy or the return from
`encodeDraw` cannot be reassigned to those fills.

Tracked runtime source is restored. Canonical x86 outputs, the shared Wine
root, and the STALKER prefix are restored to baseline D3D9
`534ea6f17128354ea350b7d6000c750e82bfcc3556d827464ebf02e69e8f02d0`
and bridge `6ba973b4f7bdd8be4c63f86a28b00e51be31ab422a71e8cdab8703b9af40baa8`.
Diagnostic images and failed-attempt evidence remain archived; no runtime
default or README FPS is changed.
