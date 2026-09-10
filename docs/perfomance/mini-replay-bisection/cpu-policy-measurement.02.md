---
domain: mini-replay-bisection
workload: 3DMark05 GT2 fixed Render Tape
title: "Qualified low-observer CPU replay policy comparison"
type: experiment
status: bounded-qualified
updated: 2026-09-10
source: experiments/output/fixed-input-harness-20260909/cpu-policy-gt2-aa-r159/result.json; experiments/output/fixed-input-harness-20260909/cpu-policy-gt2-batch-r156/result.json; experiments/output/fixed-input-harness-20260909/cpu-policy-gt2-capacity-r157/result.json; build/meson-logs/cpu-policy-native-r151.txt; build/meson-logs/cpu-policy-lifecycle-r154.txt; build/meson-logs/cpu-policy-receipt-final-r160.txt; build/meson-logs/cpu-policy-tla-r148.txt
related: specs/experiments/harness/replay/fixed-input-experiment.md; docs/perfomance/mini-replay-bisection/fixed-input-replay.01.md
---

# Qualified low-observer CPU replay policy comparison

## Result and scope

The CPU Tape harness now separates full semantic qualification from
low-observer execution and varies existing batch/capacity policies without
rewriting the captured input. This closes R-HARN-REPLAY-8.7–8.9 for the
declared immediate-completion, general-frame CPU lane, not game FPS or GPU
rendering. The measurements use the working tree based on `e9cbbca2`; each
artifact fingerprints its actual native executable, validator and loaded
renderer images. No renderer default changed.

All three comparisons use the same GT2 bundle:
`experiments/render-tapes/gt2-v2-20260909-r4-capture64/frame-1292677716873100-1`.
Each policy receives two detailed verification iterations, followed by two
retained warm-ups and 16 measured iterations in a separate process. Each
iteration resets runtime owners; the CPU-visible native resource device is
shared within its process. Processes execute serially; no Wine, encoder or GPU
work is submitted. Timing excludes import/image hashing outside the iteration
but includes runtime resource setup, replay, controlled completion and reset.

| Comparison | Reference / candidate median wall (ms) | Reference / candidate process CPU (ms) | Executed mechanism |
|---|---:|---:|---|
| A/A, compatibility exact, r159 | 84.05 / 84.39 | 84.02 / 84.33 | Both 39 submitted sources; no DrawLimit publications |
| Compatibility command limit 0 / 4, r156 | 80.11 / 83.38 | 80.11 / 83.39 | Sources 39 / 130; DrawLimit publications 0 / 102 |
| Production exact / 5 MiB per-slot, 64 MiB aggregate, r157 | 87.35 / 82.22 | 86.88 / 82.13 | Both 39 sources; candidate provisions 2 payloads and reuses retained capacity 34 times |

Every detailed run preserves 1,420 ordered draw observations and final digest
`11633069703464745659`, with 277 objects created/released and complete
source submit/completion/reclaim conservation. Compatibility executes all
1,420 draws through compatibility; ProductionSlot executes 1,410 Direct and
10 compatibility draws. Positive headroom records zero provisioning failures
and aggregate denials. Source-count equality is deliberately not required
between different policies; it is required between qualification and measured
execution of the same policy, alongside all capacity outcome counters.

Measurement iterations report zero full-oracle calls, no per-draw observation
vector, digest `0`, false logical/physical oracle flags and zero phase clocks
labelled `iteration-only`. Those zeros mean unmeasured, not free. Full-oracle
GT2 verification here costs roughly 657–724 ms per iteration; low-observer
execution is roughly 80–87 ms. This difference removes the harness's proof
work, not production work, and is not an FPS improvement. Existing runtime
perf counters and source summaries remain enabled. The policy timings are
one serialized comparison each, not a robust policy-promotion verdict.

## Evidence contract

The parent owns independent immutable input, canonical policy JSON files,
`verify/{reference,candidate}` and `measure/{reference,candidate}` child
artifacts. Native stdout/stderr, wrapper output and failed prefixes are kept.
Measured child receipts remain `accepted=false`, `semantic_qualified=false`
even when `execution_valid=true`. Only the parent grants qualified cost
acceptance after ordered semantic equality and same-policy identity binding.
The aggregate records byte hashes and relative paths for each verification
and measurement receipt, retained warm-up indices and the measured suffix.
Input, executable, validator and loaded images are checked again at final
publication. Missing identity, stale images, failed reset, changed execution
counts and failed warm-ups reject the aggregate without successful partial
timing totals.

Native r151 passes five policy/measurement/CLI/source-projection tests; r154
passes 17 lifecycle, grammar, snapshot, counter and install-name tests. R160
passes three final Python/public-CLI tests, including actual fixture A/A,
same-route batch/capacity comparisons and receipt byte-hash checks. Python
negative tests invoke the actual shared receipt predicate, not a bypass.
The verification-pair identity gate runs before measurement dispatch; a
provider-identity mismatch test proves that only the two verification children
run and no measurement directory or successful timing summary is created.
The exhaustive policy test covers 128,960 admitted record/limit/queue tuples
and concrete limit-1/2 exhaustion counterexamples. Compile-time assertions
bind the grammar's 64-record bound, queue's 31 credits and production minimum
headroom. Full unchanged TLA passes in r148 (180.74 s).

## Remaining boundary

This interface uses compatibility DrawLimit and Direct empty-slot capacity
provisioning. It does not introduce cross-raw deferred Direct publication,
worker scheduling, arbitrary concurrent admission or a broader Tape grammar.
General policy comparisons deliberately use immediate completion and one
pending submitted source. Richer lifetime/schedule tests remain in the
separate existing `cpu-compare` lane. Real encode, pixels, Wine supply,
GPU backpressure and wild FPS remain independent qualification gates.

Usage and policy bounds:
[fixed-input experiment](../../../specs/experiments/harness/replay/fixed-input-experiment.md#qualified-low-observer-measurement-and-cpu-policies).
