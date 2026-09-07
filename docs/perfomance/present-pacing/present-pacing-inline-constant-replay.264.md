---
domain: present-pacing
workload: Native PE producer to Unix replay refinement
title: "Inline constants: bounded effective-state equivalence through final Slot bytes"
type: evidence
status: correctness-prerequisite
updated: 2026-09-08
source: experiments/output/pe-inline-constant-refinement-20260908/analysis
related: present-pacing-inline-constant-current.258.md; present-pacing-pe-retain-fault.263.md; ../../../specs/backend/requirements.md
---

# Inline constants through final Slot bytes

The default-off inline policy removes standalone constant admissions, but
[H258](present-pacing-inline-constant-current.258.md) did not prove current
producer-to-final-draw equivalence or reduced CPU consumption. This increment
adds the bounded native effective-state prerequisite for R-BACK-2.52. It
changes no production representation, selector, ABI or scheduling policy.

## Production-bound seam

`tests/native/backend/inline_constant_replay_spec.cpp` drives real
`PeConstShadowBlock` transitions, compact `SparseStatePlan` construction,
`PeSemanticBatchOwner` admission/settlement and ExactFixed emission.
OFF emits standalone records; ON folds the same changes into ordinary
Draw/DrawIndexed records. Real registered `D9CShader` wrappers, retention,
`prepareOffloadChunk`, `replayRawChunk` and the current `DeviceReplaySink`
feed the queue. A test-local completion seam drains the represented source
without Metal and resolves each draw's final VS/PS byte spans and counts.

The oracle independently assembles expected register bytes after every input
write; it does not compare only handles, hashes, PE getters or two outputs
which might share the same error. Indexed flag, start, base vertex, topology
and primitive count distinguish every logical draw, including the repeated
constant state. Exact receipts require four `Appended` direct submissions
with no fallback/fail-stop, or four ordinary compatibility submissions.

## Bounded domain and negative control

Each of 64 six-family masks runs inline OFF/ON and direct/compatibility:
**256 executions, 1,024 draws**. The trace is A, repeated A, partial B, then A.
For selected families, writes to registers 1 and 3 carry unchanged register 2
inside the merged dirty range; rewriting register 1 pins last-write-wins.
VS/PS bools use opposite true/false transitions and independently normalized
one-byte final values, rather than arbitrary nonzero values which all mean true.
Every draw checks all six files. The scanner fixture conservatively retains
256/224 float registers, 16 int registers and four bool registers; untouched
tail bytes must remain zero. These are source-payload bytes, not Metal ABI proof.

Wire receipts distinguish the paths: ON has four records and no standalone
constants; OFF has `4 + 6 + 2*popcount(mask)` records, of which
`6 + 2*popcount(mask)` are standalone constants. A separate malformed
standalone count/byte-size case pins rejected admission followed by retry,
without claiming a post-retain or late-import rollback test.

Main temporarily removed the production replay of later VS_BOOL sections
whose range starts at register 1. The test fails at mask 4, compatibility
draw 2: byte 4,353 is stale while every reported count/extent still matches.
`native-inline-negative-vs-bool.log` preserves that expected failure. The
mutant is restored; `device_c_chunk_replay.cpp` has no remaining diff.
This is one deliberately broken production transition, not independent
mutation coverage for every family or for the direct-only path.

Restored focused tests pass **arm64 7/7** and **x86_64 8/8**, including the
production routing and install-name checks. Artifacts are
`native-inline-final-7.log` and `x86_64-inline-final-8.log`. The bounded target
alone ran in 0.64 seconds before the negative control; parallel suite timings
are not performance measurements. Existing full TLC checks also pass in
[H263](present-pacing-pe-retain-fault.263.md). No new concurrent model is claimed:
this is bounded state refinement through real value/queue operations, while
existing models continue to own ordering, leases and completion.

## What remains open

This does not compile/execute the Windows-only PE COM adapter or its cached
environment selector. It calls real producer code natively and replay
synchronously, without exercising worker interleavings. Scanner-only shader
refs and a null-Metal queue do not establish valid executable shaders or pixels.
It does not enumerate register-cap boundaries, shader-layout changes,
UP/ProcessVertices/full-snapshot/chunk-barrier consumers, or full fault/capture
and cross-source traces. The 12 actual PE fault/getter checks in H263 are
complementary and do not fill these scopes.

The next promotion layers are deterministic real-PE/Metal consumer-boundary
and pixel evidence, followed by repeated matched observer-OFF CPU/FPS runs.
Default stays OFF. The historical removal of roughly 1,002 standalone
records per observed 05 Present remains a mechanism, not a measured current
CPU saving. This test-only change has no new wild FPS claim; README is unchanged.
