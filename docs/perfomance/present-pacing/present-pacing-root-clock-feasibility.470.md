---
domain: present-pacing
workload: Standalone arm64 and Rosetta x86_64 short-interval native CPU accounting
title: "The libproc clock is too intrusive and quantized for an uncorrected short-root estimator"
type: evidence
status: direct-short-root-estimator-not-qualified-no-product-instrumentation
updated: 2026-10-08
source: traces/stkcop-root-clock-h470/{contract.md,probe.c,run.py,analyze.py,receipt.json,summary.json,arm64.jsonl,x86_64.jsonl,arm64-build.log,x86_64-build.log}
related: present-pacing-visibility-budget.468.md; present-pacing-primary-clock-qualification.452.md; present-pacing-pe-native-thread-cpu.268.md
---

# Do not add a PE clock bridge on the strength of coarse native accounting

H468 leaves complete Primary root CPU attribution missing. H470 checks a
necessary capability before adding instrumentation: whether the already-used
`PROC_PIDTHREADID64INFO` user-plus-system counter can directly price short
API-like bodies without assuming an observer-cost correction. No Wine process,
renderer build/staging, product source, runtime default or ABI changes.

Both standalone architectures complete four rounds of five fixed dependent-work
sizes, 4,096 operations each, in alternating plain/bracketed order. Plain mode
reads CPU around the whole batch; bracketed mode additionally reads before and
after every operation. All 40 rows per architecture are retained. Counts,
checksums within pairs and across rounds, nonnegative deltas, and the necessary
`sum(inner deltas) <= outer batch CPU` condition pass. Compilation emits no
warnings. Source, compiler commands and binary hashes are in the receipt.

## Rosetta observations

Times below are CPU microseconds. Plain values are batch CPU divided by 4,096,
including loop/call and outer-read cost; they are not exact individual-body
measurements. Inner means include part of the observer's CPU.

| Dependent iterations | Plain batch CPU/operation | Mean inner bracket delta | Bracketed batch CPU/operation |
|---|---:|---:|---:|
| 0 | 0.0020–0.0024 | 0.428 | 0.849–0.856 |
| 16 | 0.0251–0.0256 | 0.462 | 0.881–0.891 |
| 128 | 0.3086–0.3135 | 0.737 | 1.153–1.165 |
| 1024 | 2.432–2.448 | 2.854 | 3.289–3.307 |
| 8192 | 19.295–19.367 | 19.774 | 20.239–20.262 |

All observed inner deltas have a 1,000 ns greatest common divisor. This is
observed delta granularity, not a newly proved intrinsic clock resolution.
61.7% of empty Rosetta brackets report zero; their 95th percentile is 1 us.
The 128-iteration case still has 37.1% zeros and a 2 us 95th percentile.
arm64 shows the same observed granularity: empty inner mean 0.283 us and empty
bracketed batch cost 0.557–0.583 us/operation. Its detailed rows remain in the
summary. The two architectures are not pooled into a game-cost estimate.

## Decision and limits

Sparse sampling could reduce the observer's *total* CPU load, but does not
remove each selected sample's read cost or quantization. Scaling these short
brackets by a game call count would also scale their observer charge. Do not
subtract an average empty bracket, assume additive independent error, or
extrapolate these synthetic bodies into actual D3D9 costs.

A trustworthy uncorrected delta may still be an upper bound on an instrumented
body; positive observer cost alone does not invalidate an upper bound. This
experiment does not establish sufficiently tight error/default-transfer bounds
to use it for the requested owner ranking. The current probe also does not
retain absolute inter-sample counter values, so it cannot independently prove
every interior monotonicity relation. No positive qualification follows.

The direct short-root estimator using this counter is therefore not adopted,
and no PE/native clock bridge is implemented. This does not invalidate existing
coarser native thread totals or prove every aggregate/higher-resolution method
impossible. Largest default dxmt9 CPU ownership and a correctness-qualified
removable amount remain open. H468/H469's independent rejection/indeterminate
results are unchanged.
