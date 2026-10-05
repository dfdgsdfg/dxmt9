---
domain: present-pacing
workload: STALKER CoP capture29
title: "Actual-Metal replay timing: first-run cost prevents naive policy ranking"
type: experiment
status: diagnostic-only-warm-policy-required
updated: 2026-10-04
source: experiments/output/stkcop-v4-metal-timing-18/verification.md; experiments/output/stkcop-v4-metal-timing-18/{l0-r1,l3-r1,l64-r1,l0-r2}/result.json; experiments/output/stkcop-v4-metal-timing-18/{l0-r1,l3-r1,l64-r1,l0-r2}/replay.stderr
related: docs/perfomance/present-pacing/present-pacing-v4-pass-localization.434.md; specs/experiments/harness/replay/spec.md
---

# Actual-Metal diagnostic cost boundaries

The private V4 diagnostic now exports existing prepared phase clocks and
runtime snapshot timing deltas. No production timer or rendering policy was
added. Unlike [H434](present-pacing-v4-pass-localization.434.md)'s CPU-only
comparisons, these executions reach actual Metal and independently match the
unchanged original source/output pixels and EVENT-query readiness.

Four fresh-device runs execute L0, L3, L64 and L0 again. The executable and
loaded-image identities are identical. Each retains 2,799 compatibility draws,
80 batches, zero unbatched draws and zero GPU errors. All four pass original
pixels, query, lifecycle and final reauthentication gates. They remain
diagnostic-only, not paired policy/timing qualification.

| Ordered run | Provider replay wall (ms) | Runtime encodeChunk (ms) | Encode chunks | Pipeline lookup/bind elapsed (ms) |
|---|---:|---:|---:|---:|
| L0 first | 1065.974 | 683.707 | 5 | 639.254 |
| L3 | 345.482 | 25.341 | 174 | 5.805 |
| L64 | 339.367 | 12.697 | 12 | 1.017 |
| L0 repeated | 393.962 | 11.532 | 5 | 0.472 |

These columns overlap and must not be summed. Prepared replay wall excludes
policy admission/reset, input preparation, factory creation and device teardown.
Runtime counters span factory creation through teardown. EncodeChunk includes
pass setup, PSO work and encoding; the existing pipeline lookup counter includes
depth-state setup and binding, not just shader compilation. Neither is thread
CPU time despite historical counter names. Exclusive GPU, pass planning,
encoder-only and completion-wait measurements remain null.

Identical L0 falling from 683.707 to 11.532 ms makes first-versus-second policy
ranking invalid. The first execution's long pipeline scope is observed; the
relative contributions of cache/driver warmness and scheduling are not isolated.
Disabled application shader archives and a fresh device do not establish a cold
system driver cache. Background `dasd` CPU activity also prevents treating these
single executions as clean performance samples. The first run is preserved.

Input owner copy/preflight takes 4.25--4.30 seconds per invocation, separate from
the provider replay. That is harness setup, not a live-game bottleneck. The next
experiment-enabling step is authenticated prepared-input reuse plus explicit
reset and warm-cache qualification under R-HARN-REPLAY-8.3, followed by balanced
repeated order and narrower encode attribution under R-HARN-REPLAY-9.5. No runtime
policy is promoted from these four smoke results.
