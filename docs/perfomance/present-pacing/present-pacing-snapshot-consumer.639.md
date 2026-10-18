---
domain: present-pacing
workload: STALKER H502/r5 immutable captured input, private actual queue consumer
title: "H639 — shared snapshots reach the actual Slot consumer and pass a CPU screen"
type: evidence
status: captured-cpu-screen-passed-default-live-transfer-open
updated: 2026-10-16
source: traces/stkcop-snapshot-consumer-h639/{contract.md,prepare.py,derivation.json,build.py,native.cpp,test_native.py,test_integration.py,prepare_timing.py,verify.py,verification.json}; traces/stkcop-snapshot-consumer-h639/build-initial/build.json; traces/stkcop-snapshot-consumer-h639/native-linked/results.json; traces/stkcop-snapshot-consumer-h639/native-integration/results.json; traces/stkcop-snapshot-consumer-h639/oracle-initial/runs.json; traces/stkcop-snapshot-consumer-h639/timing/{build.json,native-tests.json,measure-contract.md,run.py,parse.py,analyze.py}; traces/stkcop-snapshot-consumer-h639/timing/qualify/runs.json; traces/stkcop-snapshot-consumer-h639/timing/measure/{runs.json,analysis.json}
related: present-pacing-snapshot-owner.638.md; present-pacing-run-sharing.637.md; present-pacing-minimal-owner.636.md; present-pacing-owned-run-warm-replay.598.md
---

# Actual consumer, not a shadow-only result

H639 connects H638's qualified snapshot owners to the real compatibility queue
and final Slot storage. The private candidate preserves the original captured
pixels, query transcript, source mappings and wrapper conservation. Its frozen
same-input CPU screen passes. **Default-live removable milliseconds per frame
and the largest default dxmt9 CPU owner remain unproved.**

## Representation and build boundary

`H639_SHARE=0` retains per-run owned state/uniform storage. ON retains 726 base
owners for 2,343 runs / 2,828 draws / 84 batches, using the exact qualified H638
owner/epoch/index primitive. Each run still owns debug/binding/range data.
The production queue implementation in the private mirror consumes borrowed
hot/layout/debug views synchronously; ordinary and query append copy directly
into final SoA. Arena creates its existing direct input from the same views.
The generic virtual fallback may materialize; the actual queue does not.

All 173 linked project objects are rebuilt against one private mirrored header
universe because Device and batch-entry layouts change. No canonical archives
are mixed in. The ordinary owned/borrowed append algorithms compare identical
after normalizing their input type and PSO-view expression. Queue ordering,
resource marking, batch/Slot boundaries and Encode scheduling are unchanged.
This treatment combines snapshot sharing and borrowed queue delivery; their
individual CPU contributions are not separately identified.

Canonical source hashes remain unchanged; no PE schema change, Wine staging,
default promotion, commit or push occurs. The first standalone native link
failure (missing diagnostic/perf symbols) is retained under `native-initial/`;
the corrected consumer tests link the coherent private runtime.

## Correctness layers

| Evidence | Result |
|---|---|
| H638 ownership model, ARM64 and x86_64 rerun | 78,125 words / 546,875 transitions per architecture; 25 omission dimensions, two reserve faults, six consumer faults |
| Actual owned/borrowed Slot differential, x86_64 | 243 five-step sequences / 1,215 transitions; input overwrite and complete-entry checks |
| Actual shader-bytecode ownership variant | Another 243 sequences retain equal final states after source destruction/reset |
| Actual query-range allocation faults | 90 sites per owned/borrowed arm; all lengths, lookup chains, END receipts and retry preserved, also with dedup disabled |
| Actual Device publication failure | OFF/ON reject and allocation-throw paths preserve Clear/Present fail-stop |
| First image original-input oracle | O0/N0/N1/O1 pass; ON owner counts match the qualified census |
| Root-observer qualification | 12 native controls and 22 corruptions; observer OFF/ON × candidate OFF/ON original-input controls pass |
| Fixed CPU population | All 42 fresh-process runs retain exact source/output pixels, seven source mappings, query9/5/4/5 and 670 created/released wrappers |

Every pixel digest is
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
Queries use the existing production-counter diagnostic, **not an actual Metal
occlusion-result oracle**. Native Slot/query tests and captured equivalence do
not establish arbitrary live generation-writer coverage or Arena failure
coverage. Those remain promotion obligations.

## CPU result and its denominator

Fixed order: two OFF/ON warmups, eight OFF shams, eight OFF/ON/ON/OFF blocks.
Each process replays once on a fresh Device. Warmups affect host caches, not
same-Device retained capacity. H598's repeated-reference pixel failure is not
retried or hidden by calling fresh-Device tests warmed reuse.

The metric sums **260 inline-provider Replay roots per captured input**, not
seven queue publication identities or a live frame. All 5,211 record visits
and root identities match qualification. The roots cover inline planning,
replay, queue append and settlement, including provider replays used during
bootstrap. Work outside these roots, including other resource setup/import,
Encode-worker CPU and GPU execution, is not counted. Root CPU excludes thread
waiting; wall-bracket uncertainty and the inherited 2µs endpoint allowance
remain in the bounds. These are enclosures, not statistical confidence intervals.

| Metric | Milliseconds per captured input |
|---|---:|
| Reference CPU point mean, 16 measured runs | 63.918 |
| Candidate CPU point mean, 16 measured runs | 59.587 |
| Median paired block reduction | 4.1775 |
| Median paired reduction lower bound | 2.7870 |
| Largest absolute adjacent-sham bound | 2.6277 |
| Lowest paired block lower bound | 1.7673 |

All eight block lower bounds are positive. The median lower exceeds the sham
bound by only **0.1593ms**, so this is a passing but narrow screen, not a robust
live-effect estimate. No outliers are removed or extra runs added. Independent
raw decoding checks 10,920 roots and 4,061 hashes; integrated postflight checks
9,128 hashes, unchanged canonical sources, the coherent object universe and
no remaining target processes.

## Consequence

Unlike the closed H597/H600 constructor and H626/H627 Encode candidates, this
combined representation change now has an actual-consumer, same-input CPU
reduction. Do not multiply it by live batch counts or report it as FPS gain.
Next qualify live generation/value agreement and ordinary/query/Arena failure
boundaries, then compare minimal-observer default-live OFF/ON episodes with
matched images and camera boundaries. H636's overlapping original Replay and
Encode bounds still prevent declaring a unique global default winner.
