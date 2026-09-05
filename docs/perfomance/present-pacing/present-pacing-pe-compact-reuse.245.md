---
domain: present-pacing
workload: 3DMark05 GT2
title: "PE compact attribution and qualified reuse"
type: evidence
status: partial
updated: 2026-09-05
source: src/d3d9/d3d9_pe_producer.cpp; src/d3d9/d3d9_pe_semantic_owner.hpp; experiments/output/app-d3d9-3dmark05-gt2-perf-compact-reuse-{base-off,base-on,candidate-off,candidate-on,reviewed-off,final-off,final-on}-20260905
related: specs/d3d9/recorder/requirements.md; present-pacing-pe-prepared-emission.244.md
---

# PE compact attribution and qualified reuse

R-CORE-REC-7.17–7.19 separate attribution from implementation: first measure
compact preparation, destination lookup, and family-qualified settlement;
then eliminate observer-off no-effect walks and duplicate destination/identity
interpretation. The sole production semantic owner, ABI, cadence, transport
defaults, and thread policy remain fixed. No stable FPS gain is presumed.

## Instrumentation-only baseline

The source is `90f584db` plus the instrumentation patch retained in the base
OFF artifact `analysis/baseline-implementation.patch`. Both runs use GT2,
1024x768, Sikarugir `sikarugir-cx-24.0.7`, `perf`, Info logging, and segmented
transport `0`. OFF selects PE stats decimation / owner phase split `0/0`;
ON selects `127/1`. No build, native/TLA test, other Wine workload, or GPU
trace overlaps the runs. A read-only implementation design worker is active.
FPS comes from the benchmark-owned `.3dr`, not process elapsed time.

| Output suffix | Presents | .3dr FPS | CB/P | Pass/P |
|---|---:|---:|---:|---:|
| `gt2-perf-compact-reuse-base-off-20260905` | 1,717 | 27.451 | 3.9994 | 15.7705 |
| `gt2-perf-compact-reuse-base-on-20260905` | 1,646 | 26.298 | 3.9994 | 15.7691 |

All suffixes use `experiments/output/app-d3d9-3dmark05-`. Both pass with zero
chunk rejects, GPU command-buffer errors, pipeline-library failures, and
no-pipeline skips. Loaded artifacts: PE32 `7beebfd1cf2c5181` (1,837,568
bytes), PE64 `0a85191ba1a9c968` (1,707,520 bytes), Unix `f7935d35db31eb12`
(9,779,896 bytes), bridge32 `e31242c3d6c67e9a` (140,800 bytes), and
bridge64 `cc46aabe66969513` (145,920 bytes). ON reuses the frozen OFF staging.
The OFF result is already higher than H244's 26.919 candidate without the
requested algorithmic changes. This is another reason to use fresh matched
cells and not attribute historical single-run deltas to one implementation.

## Baseline attribution

| Scope/family | Events | Samples | Calibrated ns/event |
|---|---:|---:|---:|
| Compact/state preparation (`draw_packet`) | 2,749,900 | 21,653 | 133.6 |
| Destination rebase, all families | 4,390,052 | 34,568 | 121.6 |
| Stream/IB context lookup | 2,734,222 | 21,530 | 95.9 |
| Settlement: sparse draw | 40,475 | 325 | 431.6 |
| Settlement: sparse indexed draw | 2,693,747 | 21,200 | 403.6 |
| Settlement: sparse APPLY_STATE | 15,678 | 116 | 1,041.0 |
| Settlement: constants | 1,624,259 | 12,777 | 39.7 |
| Settlement: other generic | 15,893 | 149 | 1,017.3 |
| Aggregate settlement | 4,390,052 | 34,567 | 274.1 |

The decimated empty pair is 238.4ns; owner-family calibration is 217.6ns.
Each value subtracts its corresponding null-pair mean. Family rows reuse the
exact aggregate settlement interval, including failure/rollback attempts;
they do not introduce another nested timer. Preparation/destination/context
are independent event populations with distinct sampling residues. Different
event counts can still cause nested samples to overlap, so these inclusive
short-scope figures are neither additive nor precise product-cost estimates.
In particular, constant settlement is much smaller than the instrument.
The last snapshot is near Present 1,620 of 1,646, not a complete run ledger.

This separates the previous mixed-family settlement finding: sparse indexed
draw dominates the settlement population and costs more per event than
standalone constants. It does not independently assign that time to bitmap
walks, validation, or actual PendingDelta consumption.

## Implementation and proof status

The normal compact observer-off settlement now skips the three render/TSS/
sampler value walks before entering them. Full snapshots and enabled semantic
tokens retain the original validation. Count/mask/schema, constant range,
pending-ticket checks and actual pending consumption remain intact.

The production compact path calls the same typed lazy stream/index resolvers
as the native truth table. Streams query retention only when bound and not
already selected. IB queries require indexed, unselected, known, unchanged,
bound state. Every CapacityPre retry resolves again under the recorder borrow;
there is no cross-call cache, stored callback, or opaque owner context. The
generic/full-snapshot context path remains unchanged. Candidate context timing
includes the fused selection as well as lookup, rather than moving the actual
query work outside the observed interval.

Prepared admission now owns one 80-entry qualified identity/object/pin/wire-
index table and a 64-entry wire-ordinal array. First-retain and first-wire
promotion order remain distinct without copying identities or sorting. The
unique-retain callback receives the just-created identity ordinal; assigning
the predicted pin adds no second record-local search. The obsolete prepared
pin-mapping type is removed. Standalone reference/oracle helpers are not a
second production admission witness.

All four canonical builds pass. Eighteen focused native specs and x86_64
owner/batch/producer differential plus install-name checks (4/4) pass. Tests
cover 512 three-stream selected/bound/retained combinations, all 64 index
boolean combinations and exact query counts, typed destination A/B/A,
observer-on/off bytes and PendingDelta, 80-identity/64-wire frontiers,
promotion order, collisions, rollback/retry and existing stale recorder/
ticket cases. A signedness warning in the new test was corrected before the
final receipts. Family totals reuse the exact aggregate interval, including
rollback; native tests pin this without relying on clock-duration thresholds.

Existing `dxmt9-verify-tla` passes in 144.02 seconds. No temporal model changed:
this is regression evidence for acceptance/settlement, not a new proof of
canonical values, the full Windows device envelope, or Wine/Metal pixels.
Native value refinement supplies the bounded new resolver/identity evidence.

## Release code generation

| Admission body | Baseline frame bytes | Candidate frame bytes |
|---|---:|---:|
| PE32 generic/compact, observed | 5,128 | 4,352 |
| PE32 generic/compact, unobserved | 5,112 | 4,336 |
| PE64 generic/compact, observed | 5,224 | 4,456 |
| PE64 generic/compact, unobserved | 5,200 | 4,432 |

These are generated prologue reservations, not `sizeof` estimates or total
call-chain stack depth. Stack probes **remain** in all four rows. The PE32
constant bodies remain 120/100 bytes observed/unobserved, with no probe; PE64
constant bodies remain 216/192 bytes. Consolidation reduces the prepared
caller frame by about 0.75KiB but does not prove a throughput improvement.

In the frozen PE32 producer object, `acceptSparseStatePlan` now branches at
`0x13b7` over all three `forEach` calls (`0x13e5`, `0x140a`, `0x1431`) when
fullSnapshot is false and tokens are null. The baseline token-null check was
inside the enumerated walk. Required count/range validation and consumption
still execute. Exact objects, full disassembly, source patches and summaries
are retained in the base OFF artifact `analysis/`.

## Candidate direction evidence

The candidate uses the same runtime/settings and freshly stages PE32
`2e9feb9696c26fb5` (1,838,592 bytes) and PE64 `690fb60e24252879`
(1,708,032 bytes). Both bridge DLLs and the Unix provider match baseline.
ON reuses the exact OFF artifacts. Only read-only review overlaps the runs;
no builds, tests, other Wine workloads, or GPU trace overlaps them.

| Output suffix | Presents | .3dr FPS | CB/P | Pass/P |
|---|---:|---:|---:|---:|
| `gt2-perf-compact-reuse-candidate-off-20260905` | 1,771 | 28.319 | 3.9994 | 15.7657 |
| `gt2-perf-compact-reuse-candidate-on-20260905` | 1,665 | 26.607 | 3.9994 | 15.7676 |

Both pass with zero rejects, GPU errors, library failures, and no-pipeline
skips. OFF moves +3.16% and ON +1.17%; these one-run cells are direction
evidence, not stable FPS recovery or complete post-v0.1.1 attribution. CB/P
remains about four and pass/P changes by less than 0.04%. Time-stepped record
and byte populations are not exact replay-equivalent traces: the native
fixtures, not rounded runtime counts, establish the bounded byte/cadence proof.
The inspected retained end-scene images show the expected forest; this is only
a limited visual smoke, not a full-scene pixel oracle.

| Scope | Baseline calibrated ns/event | Candidate calibrated ns/event |
|---|---:|---:|
| Compact/state preparation | 133.6 | 99.3 |
| Destination rebase, all families | 121.6 | 63.4 |
| Context lookup (candidate includes fused selection) | 95.9 | 87.2 |
| Settlement: sparse draw | 431.6 | 426.3 |
| Settlement: sparse indexed draw | 403.6 | 389.5 |
| Settlement: sparse APPLY_STATE | 1,041.0 | 1,166.7 |
| Settlement: constants | 39.7 | 22.8 |
| Settlement: other generic | 1,017.3 | 1,320.6 |
| Aggregate settlement | 274.1 | 260.5 |
| Prepare admission | 322.5 | 322.6 |
| Canonical materialization | 311.2 | 283.3 |

The candidate owner empty pair is 231.6ns versus 217.6ns baseline; the
independent scope pair is 239.9ns versus 238.4ns. Indexed settlement's raw
means are approximately 621.2ns and 621.1ns: virtually its entire calibrated
decrease tracks the 14ns null-pair shift. Aggregate settlement's raw means
are likewise about 491.7ns and 492.1ns. **Do not claim an isolated measured
settlement speedup from these cells.** Preparation also changes despite no
new preparation algorithm, underlining sampling/run variation. The destination
rebase estimate falls, but independent populations and inclusive overlap still
prevent adding these numbers as recovered producer time. Sparse copy stays
334.1 → 339.6ns; the inclusive admission parent is 3.741 → 3.791us.

The candidate final snapshot covers 4,402,476 admissions / 34,665 samples near
Present 1,620 of 1,665. Family events/samples are draw 40,475/266, indexed
2,700,817/21,319, apply 15,668/123, constants 1,629,629/12,823, and other
15,887/134. Small apply/other populations are particularly noisy. Full machine-
readable phase/scope populations and artifact hashes are retained in
`analysis/comparison.json`; sanitized native/x86_64/TLC receipts are alongside
the release code and source snapshots.

## Independent review and diagnostic boundary follow-up

The read-only review found no reachable ticket/destination/identity correctness
regression, but caught unconditional construction of the new no-op scope guards
on the disabled route. It also required explicit evidence of nested sample
collisions. The coordinator moved all four new call sites behind cached gates
into `dxmt9PeObserveCompactScope`, with noinline diagnostic wrappers and shared
always-inline production bodies. Guard construction and clock/nesting work stay
in the enabled wrapper. `compact_nested_samples` reports collisions among these
new scopes; it does not remove instrument bias or cover every older PE timer.

Native follow-up pins sampled/unsampled nesting and frontier restoration, and
all five family classifications plus exact aggregate interval conservation.
The generic OtherGeneric rollback fixture remains. Pure resolver truth tables
use retention spies, while qualified real-owner lookup has a separate fixture;
their complete Windows device-envelope composition is not a native proof.
The original candidate rows above are frozen pre-review artifacts, not silently
relabelled as the final diagnostic implementation. A preliminary reviewed-OFF
smoke (`reviewed-off`, 1,769 Presents, 28.291 FPS, zero errors) is retained
separately; the final build also forces production body inlining and constructs
diagnostic callback captures only inside the enabled branch.

The final PE32 preparation body branches at `0xc720` / `0xc726` directly to
production work at `0xc762`, skipping capture stores and the outlined observer
call. Generic context likewise branches at `0xbb13` / `0xbb19` into the inline
production body. Compact destination/context wrappers have the same cached-gate
separation. Admission frame sizes and small constant bodies remain as in the
table above. `analysis/final-gated-{x86,x64}-*.{obj,asm}` and the corresponding
source patch/JSON freeze this final build; earlier codegen labels are retained.
Native 18/18, x86_64 4/4, and the last two source-contract rechecks pass; sanitized
`*-final-test-receipt.txt` files accompany the earlier TLC receipt.

Final GT2 uses PE32 `55d0413e40745088` (1,843,200 bytes) and PE64
`a456988c2e6baa35` (1,712,128 bytes); Unix and bridge hashes remain identical to
baseline. The OFF run freshly stages this build; ON uses `--skip-stage`.

| Final output suffix | Presents | .3dr FPS | CB/P | Pass/P |
|---|---:|---:|---:|---:|
| `gt2-perf-compact-reuse-final-off-20260905` | 1,771 | 28.326 | 3.9994 | 15.7657 |
| `gt2-perf-compact-reuse-final-on-20260905` | 1,612 | 25.752 | 3.9994 | 15.7773 |

Both pass with zero rejects/GPU errors/library failures/no-pipeline skips.
OFF is +3.19% versus the instrumentation-only baseline; final ON is -2.08%
versus baseline ON and -9.09% versus final OFF. The diagnostic implementation
changed after the original candidate matrix, so final ON is not a matched
algorithm-only attribution cell. **ON performance neutrality is not established.**
These are single-run differences, not exact observer-cost measurements, but the
instrument cannot be called negligible. Qualifying lower-perturbation observed
execution remains open; do not use final ON FPS as product performance or sum
short scopes into an exact recovered-time claim. No builds/tests or another
Wine workload overlaps either final run.
Final OFF retains an inspected forest frame. Final ON's late `actual.png`
contains the desktop after the app exited, so its `.3dr`/counter completion is
usable but that image is not rendering-correctness evidence; ON visual closure
remains open rather than inferred from the runner's `pass` status.

Final ON reports 160 nested samples at Present 1,560 of 1,612, with 4,245,381
admissions / 33,428 parent samples. Independent scope event/sample counts are
preparation 2,658,554/20,934, destination 4,245,381/33,429, and context
2,643,452/20,815. Their calibrated means are 99.1 / 90.8 / 126.9ns respectively,
against a 248.3ns empty pair. Family calibrated means are draw 349.7ns,
indexed 364.2ns, APPLY_STATE 1,193.6ns, constant 42.8ns, and other 1,542.7ns;
the owner null pair is 228.3ns. Raw indexed settlement is about 592.5ns.
Sampling population, instrument envelope and null-pair changes still prohibit
isolated settlement-speedup or bitmap-cost attribution. All seven retained
cells, exact hashes, populations and error counters are in `analysis/comparison.json`.

Source count/mask recomputation is deliberately not elided while SparseStatePlan
remains publicly mutable; private producer-issued selection is a prerequisite
to trusting it. Broader workload/fault/pixel and repeat performance promotion
remain separate gates.
