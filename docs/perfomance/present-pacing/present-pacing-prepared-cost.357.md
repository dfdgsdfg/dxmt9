---
domain: present-pacing
workload: 3DMark05 GT2 fixed capture
title: "H357: Prepared serial construction versus Encode savings"
type: evidence
status: partial-attribution
updated: 2026-09-24
source: experiments/output/prepared-cost-h357-cold/summary.json; experiments/output/prepared-cost-h357-cold/{aa0,aa1,ab1,ba1,ba0,timer_off1}/{result.json,provider.stderr}; experiments/output/prepared-cost-h357-cold/{dxmt9-h357-build.log,dxmt9-h357-build-final.log,dxmt9-h357-tests.log}
related: present-pacing-prepared-serial.356.md
---

# H357: Preparation adds cost; Encode savings are not demonstrated

Base `007f9309` plus the source-level timing overlay. The H356 serial consumer
remains experimental/default OFF. This experiment prices its current fresh
allocation and second publication walk, not a fused Replay implementation.

## Method and qualification

Run `scripts/tools/measure_prepared_serial_cost.py` through
`scripts/run_python.sh`, using the preserved GT2 bundle
`experiments/render-tapes/gt2-gpu-source-stream-20260914-r405/frame-1651063776832900-1`.
The source-plan SHA-256 is
`cc6b8312ea6abfc0a571741faca56aec729d8dac57e5585a3589bce29aac34f6`;
the provider SHA-256 is
`51148f9cd8f6796f6b190459fd557c314c6a8962aa7a502b902e3015eedbbc27`.
All six blocks preserve the binary, input components, authenticated source
boundaries, ProductionSlot exact route and identity-serial policy.

Execution order is OFF/OFF, ON, ON/OFF, then ON with the nested build timer
disabled. Each block uses two warmups and sixteen measured repetitions,
108 iterations total. Every iteration resets cold-device: capture-source
replay rejects warm-device before execution. That rejected preliminary attempt
is preserved separately in `experiments/output/prepared-cost-h357/` and is not
a timing sample. Builds and tests did not overlap the successful campaign.

All runners accepted their capture oracle. All source/output readbacks match
byte-for-byte across blocks, with SHA-256
`d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`.
Every ON iteration consumes 646 prepared DrawRun read hazards; OFF consumes
zero. Attached-owner row fallbacks and GPU command-buffer errors are zero.
This is non-vacuous bounded GT2
qualification, not STALKER or all-workload coverage.

## Results

Medians in milliseconds per captured-frame iteration. These are elapsed wall
scope sums, **not thread CPU time**. Prepare-slot includes resource marking
and prepared-build; chunk replay includes preparation. Do not sum nested or
concurrently executing scopes into a frame critical path.

| Block | Prepare-slot | Nested prepared-build | Resource mark | Encode chunk | Chunk replay |
|---|---:|---:|---:|---:|---:|
| aa0 OFF | 0.752 | 0 | 0.729 | 42.040 | 18.502 |
| aa1 OFF | 0.767 | 0 | 0.745 | 42.042 | 18.589 |
| ab1 ON | 3.009 | 2.249 | 0.719 | 42.209 | 20.965 |
| ba1 ON | 2.983 | 2.204 | 0.717 | 42.341 | 20.851 |
| ba0 OFF | 0.745 | 0 | 0.724 | 41.610 | 18.290 |
| timer_off1 ON | 2.968 | unmeasured | 0.724 | 42.391 | 20.724 |

The A/A Encode median difference is +0.002ms. ON minus OFF is +0.167ms in
the forward comparison and +0.730ms in the reverse comparison. Encode IQRs
overlap: aa1 41.573–42.538ms, ab1 41.881–42.505ms, ba1
41.952–42.757ms, ba0 41.374–42.507ms. There is no demonstrated Encode saving;
tiny read-hazard savings below this parent-scope noise remain possible.

Preparation adds approximately 2.2–2.3ms. The timer-OFF candidate still costs
2.968ms in the parent prepare scope, so the new nested clock cannot explain
that increase. Resource marking stays near 0.72–0.75ms. No per-draw clock was
added. `DXMT9_PREPARED_BUILD_TIMING=1` requires perf counters and times the
allocation/projection/seal/owner-attachment block; its disabled branch does
not construct a timer.

Service medians drift from 260.611 to 253.096ms in execution order despite
the candidate's higher preparation cost. Cold-device reset includes device,
pipeline and setup effects; block order and host/cache variation prevent a
net throughput or FPS conclusion. These numbers must not be relabelled live
game pipeline CPU shares.

## Decision and next gate

Keep the consumer OFF. Correctness passed, but current preparation economics
do not justify promotion. The next structural candidate is to reuse/fuse
existing Replay projection and avoid allocating/initializing a whole fixed
plan plus walking commands again merely to replace a small read-hazard helper.
Measure that candidate with the same boundaries before extending consumers;
this experiment does not implement that optimization.

Full Unix build and focused x86_64 prepared-plan, builder-comparison, hazard
and install-name checks pass (4/4). The overlay changes timing and reporting,
not H356 ownership or transition semantics; it adds no new TLA proof claim.
Counter table/callsite audits and `git diff --check` pass. The repository-wide
performance-source audit reports pre-existing missing build logs in
`capacity-ownership-recovery.05`, `gpu-position-invariance.04` and
`present-pacing-query-default-promotion.316`; these are unrelated to this run.
