---
domain: present-pacing
workload: STALKER frozen H502/r5 original input, captured source 5
title: "Current Clear candidate: source-boundary self CPU without supply tracing"
type: evidence
status: local-net-cpu-qualified
updated: 2026-10-16
source: traces/stkcop-current-clear-h572/{contract.md,build.json,harness.json,capture-A-receipt.json,capture-B-receipt.json,capture-C-receipt.json,capture-D-receipt.json,warm-0-A-receipt.json}; traces/stkcop-current-clear-h572/warm-0-A/analysis.json; traces/stkcop-current-self-h573/{contract.md,build.json,arm.hpp,run.py,preflight-A-receipt.json,preflight-B-receipt.json,rows.jsonl,summary.json,raw-verification.json}
related: present-pacing-default-owner.571.md; present-pacing-clear-live.559.md; present-pacing-clear-failure-boundary.558.md
---

# Current-image local removal is positive with source-boundary observation

H573 measures the current canonical encoder against the H558 failure-safe Clear
candidate, with the H570 shader-count repair common to both images. It removes
the product supply trace from the timing experiment. This is a new local
measurement, not a transfer of H559's historical live ms/frame values.

## Rejected first attempt

H572 builds four matched images: canonical/candidate with thin H514/H516
observers, and canonical/candidate without private observers. Archive parity
checks bind the candidate to exactly three changed members: encoder chunk,
CPU-ready session and framegraph backend. The no-private-observer baseline
runtime archive is byte-identical to canonical; the provider differs only in
the repaired shader-count member. All four original-input GPU/semantic
preflights pass; both candidates witness 65 Clear fusions.

H572's local timing population stops on its **first warmup**. Enabling the
product supply trace in the canonical provider also enables 646 Slot-validation
scopes and 3,378 queue-lock scopes. Its predeclared coarse-only gate rejects
`slot_validation_begin`. No A/A or candidate timing is admitted from this
population, and the failed artifact is preserved. This is a measurement-scope
failure, not a rendering failure or evidence of a runtime regression.

## Replacement observation contract

H573 uses H572's thin-observer image pair and one byte-identical private probe.
The probe maps the already-created H514 header after device creation, verifies
PID/birth/schema/extent, arms before prepared replay, then disarms after replay
returns. It does not activate product supply/planning tracing. Both sides use
the same original source boundaries and the same unchanged H514 queue scopes.

Every execution contains exactly 19 private rows: one binding and two endpoints
for each of nine Encode iterations. Seven iterations map to original captured
sources; two initialization iterations are excluded from the selected sum.
The primary captured source 5 maps to sequence 8. Measurements cover the full
Encode iteration, including its normal submission/settlement, not only the
inner `encodeChunk`. No ReplayOffloadWorker measurement is asserted for this
synchronous capture harness. No supply CSV is created.

The fixed population is two warmups per arm, four A/A pairs, then eight
alternating AB/BA pairs: 28 executions. All pass original source/output pixel
SHA, seven-source mapping, query transcript and 670/670 wrapper lifetime
checks. Preflight alone witnesses 65 candidate fusions; witness output is OFF
during timing. There are no retries, exclusions or population extensions.

## Result

| Metric | Baseline median | Candidate median | Paired median saving | Maximum A/A difference | Positive pairs |
|---|---:|---:|---:|---:|---:|
| Source 5 full Encode iteration | 35.8305 ms | 33.5950 ms | **2.2910 ms** | 0.2490 ms | 8/8 |
| Seven mapped Encode iterations | 60.4240 ms | 57.8865 ms | **2.5885 ms** | 0.3210 ms | 8/8 |

Source-5 paired savings are 2.291, 3.139, 2.291, 1.876, 1.446, 1.615, 2.524,
and 2.703 ms in declared order. The primary paired median is about 6.4% of
the baseline median; this is not an FPS percentage. The sum is sequential
same-Encode-thread work for these seven sources, not a concurrent pipeline sum.
Both metrics pass the predeclared finite screen. These are measured paired
distributions, not confidence intervals or universal savings bounds.

An independent verifier imports no timing analyzer: it checks image and
artifact hashes, process identity, source bindings, 252 raw Encode pairs,
original pixels and both metric calculations across all 28 executions.
No lost/faulted rows or incomplete pairs occur. The private probe builds
without warnings. No shared Wine/prefix staging or production code changes
are made by H572/H573.

## Remaining goal scope

This establishes **current-image local net removal**, including the three-member
failure handling, with only source-boundary clocks. It does not establish the
same saving per live STALKER frame, an observer-free CPU maximum, FPS gain or
default promotion. Native CPU endpoint cost still exists; disabling thousands
of unrelated trace events does not make the remaining clocks free.

H571's observed-owner result remains the live owner evidence. Next compare the
current image pair in the same late scene with the high-frequency PE root
observer removed, plus an explicit observer/default control. Foreign dispatch
and closure coverage remain independent scope gaps. H558's finite failure
tests and this original-input GPU oracle do not close every reset/wake or
hardware-query cancellation scenario.
