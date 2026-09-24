---
domain: present-pacing
workload: 3DMark05 GT2 fixed capture
title: "H356: First immutable prepared-plan serial consumer"
type: evidence
status: partial-attribution
updated: 2026-09-24
source: experiments/output/prepared-serial-h356/{native-tests.log,native-model-detail.log,x86-tests.log,lifecycle-tests.log,tla.log,unix-build.log,pe64-build.log,pe32-build.log,comparison.json}; experiments/output/prepared-serial-h356/{reference,candidate}/{result.json,provider.stderr,provider.stdout}
related: present-pacing-prepared-comparison.355.md
---

# H356: Serial read-hazard consumption with Slot ownership

Base `794f6458` plus this implementation. The first executable consumer replaces
DrawRun read-hazard reconstruction, not the entire Encode coordinator. Replay's
final ordinary Slot publication constructs a bounded plan after resource marking
under the queue mutex. The Slot owns immutable CPU metadata through reclaim;
Encode borrows without per-draw refcount operations. The existing source key,
residency and submission/completion protocol remain authoritative.

The source projection preserves full index-buffer/stream/texture reads, including
inactive bindings, because that is what the existing serial helper consumes.
Only rows whose alias-canonical read sequence equals the raw read sequence can
replace serial reconstruction. Changed aliases, unresolved rows, stale source
keys, absent plans and unsupported modes retain the reference calculation.
The consumer reads dense indices and precomputed Bloom bits instead of scanning
bindings, deduplicating handles and hashing them again. Attachment hazards,
resource pins/backing snapshots, pass actions and ordering remain unchanged.

The selector `DXMT9_PREPARED_SERIAL_READS=1` remains experimental/default OFF.
This is an initial publication-time second walk and fresh allocation, not fused
Replay construction or pooling. A plan is 168,008 bytes (up 16 KiB from H355),
plus builder scratch and an immutable owner/control block. No-draw and sources
above the command bound skip construction. No performance improvement is claimed.

## Native and lifecycle evidence

- Native plan, builder comparison and resource hazard tests: 3/3.
- x86_64 plan, builder comparison, hazard and install-name audit: 4/4.
- x86_64 Tape, serial partition, session lifecycle and session join: 4/4.
- Existing TLA regression: PASS, 281.66 seconds.
- Full Unix and both PE builds: PASS.

The actual serial consumer is compared against `makeDrawReadHazard` over 256
binding patterns, including duplicate/zero/full-generation handles and inactive
texture bindings. All source/storage/sequence/range key components reject stale
values. Tests cover missing/non-draw rows, actual alias-changing projection,
immutable owner release at Slot clear and existing capacity/allocation cases.
The publication reducer now models prepared serial execution separately from
parallel execution: one consumer completion, but no metadata release before
join. Its exhaustive graph has 93 states and 113 accepted edges. This is not
a claim that the value reducer itself performs C++ queue publication; the
adapter inherits the existing queue mutex and source-residency implementation.

## Same captured input, actual GPU consumer

Input is the preserved GT2 bundle
`experiments/render-tapes/gt2-gpu-source-stream-20260914-r405/frame-1651063776832900-1`.
STALKER capture remains blocked by the previously documented volume/query
grammar gaps; this GT2 result must not be relabelled STALKER qualification.

Both runs use the same x86_64 provider, inputs and loaded image fingerprints,
ProductionSlot exact route, authenticated capture-source boundaries,
identity-serial partition mode, cold-device reset, zero warmups and two repeats.
Only the prepared serial selector differs. The renderer environment is recorded
in each receipt. The reference overlaps correctness tests, so these runs are
correctness evidence only, not performance samples.

| Gate | Reference OFF | Candidate ON |
|---|---:|---:|
| Runner accepted / completed | yes / yes | yes / yes |
| Prepared serial read consumptions, whole process | 0 | 1292 |
| Attached-plan row fallback counter | 0 | 0 |
| GPU command-buffer errors | 0 | 0 |
| Source/output readbacks | 4 | 4 |

All eight readbacks are nonuniform and **byte-identical**, each 3,145,728 bytes.
The SHA-256 is
`d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`.
Each runner also accepts its original capture oracle. The provider SHA-256 is
`cd8aa0ea14532dc584909bbce00fe2ba3c6939dcd0856db4262d373f0b2218b8`;
source-plan SHA-256 is
`cc6b8312ea6abfc0a571741faca56aec729d8dac57e5585a3589bce29aac34f6`.
Fallback counter zero does not mean every Slot has a prepared plan: it counts
only DrawRuns with an attached owner. Nonzero consumption prevents a vacuous
pixel pass through reference-only execution.

Recheck receipts, image bytes and nonzero consumption with:

```sh
scripts/run_python.sh scripts/check/check_prepared_serial_pair.py \
  experiments/output/prepared-serial-h356/reference \
  experiments/output/prepared-serial-h356/candidate
```

## Remaining scope

This closes the first ordinary-Slot serial consumer and the GT2 same-capture
correctness gate, not general prepared execution. Fused/pool-backed preparation,
publication cost versus Encode savings, broader wild qualification, Arena,
framegraph/PSO/resource-closure reuse and parallel admission remain open.
Do not enable by default from a single capture or infer FPS gains from the
earlier synthetic indexed/linear builder benchmark.
