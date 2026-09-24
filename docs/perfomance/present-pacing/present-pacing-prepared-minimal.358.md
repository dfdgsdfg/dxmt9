---
domain: present-pacing
workload: 3DMark05 GT2 fixed capture
title: "H358: Demand-specific serial read metadata"
type: evidence
status: partial-attribution
updated: 2026-09-24
source: experiments/output/prepared-cost-h358-cold/summary.json; experiments/output/prepared-cost-h358-cold/{aa0,aa1,ab1,ba1,ba0,timer_off1}/{result.json,provider.stderr}; experiments/output/prepared-cost-h358-cold/{dxmt9-h358-native-verified.log,dxmt9-h358-unix-final.log,dxmt9-h358-x86-verified.log,dxmt9-h358-tla.log}
related: present-pacing-prepared-cost.357.md; present-pacing-prepared-serial.356.md
---

# H358: Narrow metadata cuts preparation, not demonstrated frame time

Base `c2da3eb6` plus the demand-specific serial implementation. Publication
now builds `PreparedSerialReads`, reusing `makeDrawEntryReadSet` over immutable
final state. It stores raw handle offsets/counts and Bloom bits only. Canonical
alias lookup, write sets, attachment ranges, global dictionary construction and
whole-plan structural seal scanning are removed from this consumer path.
The general `PreparedEncodePlan` remains the separate scheduling foundation and
diagnostic representation; it is no longer built for the serial read consumer.

The raw representation exactly matches `makeDrawReadHazard`, including inactive
bindings. It grants no canonical alias or parallel execution authority. The
existing source/storage/sequence/command key and Slot publication/reclaim owner
remain unchanged. Missing/invalid rows use the reference calculation. Allocation
failure or bounded overflow discards the entire unpublished candidate.

Storage consists of one owner/control block, a bounded command-row vector and
a bounded raw-handle vector, reserved once per candidate. Only counted entries
are initialized, with no per-draw allocation. Bounds remain 1024 commands and
8192 accesses, but there is no global 512-identity limit because serial lookup
does not need that dictionary. This is **minimal metadata construction**, not
fusion into final-Slot emission: the publication walk and fresh allocation
remain and the lane stays default OFF.

## Fixed-input qualification and timing

H357's runner, GT2 bundle, source-plan hash, cold-device reset, authenticated
capture-source boundaries and ProductionSlot exact/identity policy are retained.
Six blocks execute sequentially: OFF/OFF, ON, ON/OFF, ON with nested build timer
OFF. Each has two warmups and sixteen measured repetitions. All builds, tests
and TLA finish before measurement. No GPU trace or per-draw clock is enabled.

Every block loads the same executable and libraries; the executable hash is
unchanged from H357 because implementation resides in the linked libraries.
The loaded native library SHA-256 is
`af3e1883498194535dda42acdaad733ee67dba74676bfded6f791397989da3e0`,
and Unix provider library SHA-256 is
`166428f697f4da2466fb58671d5b09652306fe5315203544dd5c4fa231859d97`.
Receipts record all loaded images and unchanged input components.

All 108 iterations pass the capture oracle, with every source/output image
byte-identical across OFF/ON and to H357. Readback SHA-256 remains
`d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`.
Every ON iteration consumes 646 prepared rows. Attached-row fallback and GPU
command-buffer error counters are zero in every block.

Medians in milliseconds per captured-frame iteration:

| Block | Prepare-slot | Nested build | Resource mark | Encode chunk | Chunk replay |
|---|---:|---:|---:|---:|---:|
| aa0 OFF | 0.758 | 0 | 0.733 | 42.040 | 18.586 |
| aa1 OFF | 0.743 | 0 | 0.719 | 41.577 | 18.325 |
| ab1 ON | 0.924 | 0.176 | 0.720 | 41.940 | 18.879 |
| ba1 ON | 0.921 | 0.170 | 0.715 | 41.981 | 18.633 |
| ba0 OFF | 0.768 | 0 | 0.744 | 41.685 | 18.281 |
| timer_off1 ON | 0.905 | unmeasured | 0.714 | 41.956 | 18.469 |

These are elapsed wall scopes, not thread CPU; nested and concurrent scopes
must not be added. Against H357's separate-build result, nested preparation
falls from 2.204–2.249ms to 0.170–0.176ms, about 92%. This is a historical
same-input comparison, not a same-binary full-plan/minimal-plan selector A/B.
Within the new binary, candidate preparation still adds about 0.15–0.18ms
relative to reference; the timer-OFF result confirms the parent-scope shape.

Encode savings are again not demonstrated. A/A Encode median drift is
-0.464ms; ON minus OFF is +0.363ms forward and +0.295ms reverse, with overlapping
IQRs. No overall regression or speedup is attributed from these noisy parent
scopes. Cold-device service medians span 253.889–258.902ms and include setup,
so they do not establish live FPS or CPU supply critical-path share.

## Verification and next gate

- Native prepared-plan and resource-hazard tests: 2/2.
- x86_64 prepared-plan, resource-hazard, lifecycle, session-join and install-name: 5/5.
- Full Unix build: pass. PE ABI is unchanged; this test uses the native provider.
- Existing TLA regression: pass, 255.64 seconds.
- Counter table/callsite audits and `git diff --check`: pass.

The actual new consumer is compared to the independent old helper over 256
binding patterns and all 1024 rows of an exact 8192-handle fixture. Tests cover
all key mutations, alias/raw identity, original versus local ordinal, empty
reads, non-draw fallback, each allocation stage, late capacity failure, the
command bound plus one and owner release at Slot clear. Existing exhaustive
publication/refinement tests remain active; no new queue protocol or TLA proof
claim is introduced by changing metadata representation.

Preparation narrowing is worthwhile relative to the previous candidate, but
does not yet justify enabling the feature. Next consider constructing these
rows during an existing final-state projection or eliminating the remaining
publication walk. General framegraph/PSO preparation, Arena/parallel consumption,
STALKER capture coverage and default promotion are not closed by this result.
