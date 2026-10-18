---
domain: present-pacing
workload: STALKER Day high 1280x720 max-threads2 snd_targets16 late spatial interval
title: "Ordinary queue CPU exceeds individual cache/run ceilings in one accepted episode"
type: evidence
status: one-live-rank-second-identity-rejected
updated: 2026-10-16
source: traces/stkcop-queue-cpu-owner-h601/{contract.md,trace.hpp,prepare.py,derivation.json,native.cpp,build.json,preflight.json,parser-tests.json,inputs.json,run.py,analyze_live.py,verify.py,verify-initial.py,verification.json}; traces/stkcop-queue-cpu-owner-h601/{O0,O1}/{receipt.json,self.bin,phase.bin,post-tail.png,restore/restored.json}; traces/stkcop-queue-cpu-owner-h601/O0/analysis.json
related: present-pacing-owned-run-local.600.md; present-pacing-replay-cpu-owner.596.md; present-pacing-live-batch.592.md
---

# Queue attribution advances; the second episode fails identity qualification

O0 directly measures ordinary compatibility queue CPU at **6.888–8.483ms/frame**.
Its lower bound exceeds the same episode's cache lookup CPU ceiling of
6.353ms/frame and owned-run construction ceiling of 5.726ms/frame. This ranks
three measured envelopes in one instrumented episode, not every Replay
operation, an observer-free global maximum, or removable CPU.

The fixed second episode O1 completes normally but the frozen parser rejects
`owner reuse`. Do not extend or replace this population, relax the parser to
accept it, or claim replicated ranking. No product optimization or promotion
is made.

## Boundaries and qualification

Two private translation units extend the H584 sparse worker image. The ordinary
`CommandQueue::submitDrawRunBatch` scope starts after the Arena early return
and includes scratch preparation, binding capture, segmentation, reservation,
resource marking, append, settlement, commit, teardown and mutex unlock.
Unlike H596's exclusive queue group, **this scope includes resource marking**.
`Device::cachedBaseDrawStateForSubmissionBatch` and the new-run lexical block
in `Device::submitCompatibilityReplayDrawBatched` receive wall endpoints only.
The three scopes must be mutually exclusive on one qualified queue/thread.

Only queue batches receive two caller-self CPU reads; no per-draw CPU syscall
is introduced. Lower bounds subtract endpoint-read wall and 4us allowance;
upper bounds are capped by body wall. Full-window bounds also account for
missing-start work and admitted work finishing after the end knot. Existing
diagnostic overhead remains. CPU-read duration is reported separately, not
subtracted as though it were the observer's complete CPU cost.

Six ARM64/Rosetta native controls pass mapped/absent/open-failure behavior,
71,754 bounded CPU trajectories each, identity/nesting faults, failed CPU
endpoints, exception unwinding and durable output. Parser positive and twelve
negative controls pass. Four capture/ordinary preflights pass OFF/ON effects.
Original source/output SHA-256 is
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`, with seven
sources, query transcript, 670 wrapper lifetimes and normal restoration.
The ordinary fixture skips draws and is not a pixel oracle.

## Accepted O0

| Quantity | O0 |
|---|---:|
| Completed frames | 121 |
| Replay CPU, ms/frame | 30.133–30.265 |
| Encode CPU, ms/frame | 29.065–29.256 |
| Ordinary queue CPU, ms/frame | 6.888–8.483 |
| Cache lookup CPU upper, ms/frame | 6.353 |
| Owned-run CPU upper, ms/frame | 5.726 |
| Ordinary batches | 34,328 |
| Ordinary draws / input entries | 492,339 / 414,494 |
| Cache calls / owned-run calls | 143,458 / 414,514 |
| CPU endpoint read wall, ms/frame | 0.638 |
| Selected late pacing, FPS | 17.333–17.341 |

Queue's lower exceeds cache's upper by 0.535ms/frame and owned-run's by
1.162ms/frame. Cache and owned-run have wall-derived ceilings, not measured
CPU lower bounds. Worker CPU scopes overlap across threads; do not add Replay
and Encode as frame latency. This is attribution, not an OFF/ON speed trial.

## Rejected O1 and preserved evidence

Both live runs reach all seven active spatial knots, exit0 naturally and
restore fourteen shared files each. Both post-tail screenshots are visually
coherent, not exact whole-game oracles. Catalogue failure is solely the
predeclared `missing_capture` waiver. Computer-use is limited to startup focus
and post-tail capture; no UI input is added during the selected interval.

O1 raw sparse rows bind owner `140241690198176` and controller
`140241690202080` first to domain1, then domain2, before the four CPU samples.
Encode samples name domain2; Replay samples carry a pointer but no domain.
The frozen H584 parser forbids owner/controller reuse across domains and raises
`ValueError: owner reuse`. Header fault/loss counters are zero. This is an
observer identity qualification failure, not evidence of rendering corruption
or a product lifetime violation. Pointer reuse or lifecycle rebinding must be
distinguished with explicit generation-qualified evidence in a future observer.

O1 has no admitted timing analysis. The initial two-success verifier is retained
as `verify-initial.py`; the outcome verifier explicitly records O1 rejection
instead of manufacturing a successful analysis. It independently decodes O0
bytes and reproduces bounds without importing the analyzer, verifies four
preflights, both restoration inventories (28 hashes), the O1 conflicting raw
bindings and absence of Wine processes. `complete=true` means the outcome audit
finished; `two_run_rank_certified=false` remains explicit.

## Next gate

Fix diagnostic identity composition before another attribution population:
carry lifecycle domain in Replay samples and phase ownership, or record explicit
birth/retirement intervals. Add positive address-reuse controls and negative
cross-generation/overlap controls. Do not reinterpret H601 O1 retroactively.

Then split the queue envelope into binding capture, capacity/segment work,
resource marking, actual SoA append and settlement/teardown, reusing the H592
boundary lessons. Queue total is not an optimization budget; semantic resource
stamps and ownership remain required. A removal candidate needs matched-input
effect verification and CPU contrast, not another unqualified FPS repetition.
