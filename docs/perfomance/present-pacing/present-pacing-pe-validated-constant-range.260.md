---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Public constant range authority removes duplicate Live guards"
type: evidence
status: verified-work-removal-gain-unproven
updated: 2026-09-08
source: experiments/output/pe-validated-constant-range-20260908/analysis; experiments/output/app-d3d9-3dmark{05,06}-pe-validated-range-58c67f27-gt2-{perf,sampler}-r1-20260908; experiments/output/app-d3d9-3dmark06-pe-validated-range-58c67f27-gt2-perf-user-rerun-r2-20260908
related: present-pacing-pe-settlement-refinement.259.md; ../../../specs/d3d9/recorder/spec.md
---

# Public constant range authority removes duplicate Live guards

## Mechanism and scope

H259's linked PE32/PE64 bodies retain null/empty/extent checks both in the
public constant setter and its outlined fixed-width Live helper. The current
stopped-PC profiles locate both bodies, but do not price these guard blocks.

R-CORE-REC-7.62 introduces `ValidatedConstWrite<ElemSize>::withCheckedRange`
in `src/d3d9/d3d9_pe_const_shadow.hpp`. A synchronous visitor issues a
noncopyable, private-constructed range only for nonempty, non-null input whose
64-bit end fits the family cap. The range binds source/start/end/width; the
private Live kernel consumes those facts without another range check. No
heap-owned token, persistent shadow field, raw unchecked public entry, wire,
transport, queue or Metal change is introduced.

All six public setters retain poison-first and diagnostic routing.
`D3D9DeviceImpl::applyConstStateWriteValidated` retains the existing
`applyConstStateWriteImpl` write-permission/Recording envelope. Empty calls
still allow arbitrary start and null input, but do not bypass phase permission.
Recording still copies through `recordConstStateWrite`. The generic checked
fixed-width entry still validates unqualified inputs. Growth, zero extension,
changed-only bitwise copy and dirty membership are unchanged.

## Bounded verification

`dxmt9-pe-chunk-record-value-spec` exercises 55 factory calls per family,
330 total: a complete small start/count domain, full-cap and last-register
boundaries, overflow, nonempty null, unaligned source, A/A/B/A, zero extension,
exact self-alias and warm storage reuse. The actual factory and Live kernel
are checked against the independent prior transition's values/extent/dirty
effects. Invalid and empty callbacks must execute once without Live effects.
Compile-time pins reject raw/default/copy/move construction and assignment.

Deliberately changing `end > cap` to `end >= cap`, or restricting empty writes
to `start == 0`, makes the native target fail. Both mutants were restored
before final builds. Full canonical builds pass; restored native focused
12/12 and x86_64 focused 6/6 pass, including source-route, negative ABI-audit
and Unix install-name checks. Logs retain the two negative controls and the
final positive runs. The route audit now checks each of the six setter bodies
independently; a later setter cannot satisfy an earlier missing route marker.
That audit is source ownership evidence, not a semantic proof.

This is a local value refinement, not a changed thread or lifecycle protocol.
Existing phase-model/native tests retain their original scope. The new native
factory test does not invoke the private PE adapter through a poisoned or
Terminal transaction. No new concurrency or whole-scene pixel proof is claimed.
Ordinary C++ references can still escape if a future callback is miswritten;
the production synchronous call sites, not noncopyability alone, close that
borrow lifetime.

## Linked economics and final identity

An initial outlined visitor materialized three closures per public call. The
retained `outlined-prototype-*` images rejected that code shape. The factory
and public adapter now explicitly inline, while the Live kernel remains
outlined. Neither PE image emits a factory/witness symbol or adds a setter
call: all six setters retain 15 static direct calls across all branches.

The manifest's raw F-setter body grows 5 bytes/one instruction on PE32 and
shrinks 7 bytes/three instructions on PE64. Symbol-to-next-symbol extents in
the separate audit include padding and must not replace those raw metrics.
The two validated kernels remove 16/11 decoded instructions on PE32 and
17/15 on PE64. Kernel stack reservations are unchanged except PE64 width 4,
which drops 16 bytes. Observed checked wrappers grow, retaining validation
for their unqualified callers. There is no universal code-size reduction:
the DLL grows 2,048 bytes on PE32 and 1,024 on PE64. Existing device/recorder
layout pins, exports, vtable/RTTI owners and build options remain equal.

Final restored PE32 SHA-256:
`23c2b44524fded748e6926f7c8f2b9ece3fed255b056a2209208bd58c7147f29`;
PE64: `0eb59521085213c1a82e9d48d13f696d89e6a9d1184762ac68029d95467b1ba3`.
`qualify_linked.py` confirms that each differs from its independently audited
first link only at four COFF/debug timestamp bytes. All other image bytes
and raw metrics are identical. The Unix provider and both bridge DLLs retain
H259's exact hashes. `candidate-linked-validated-audit.txt`,
`candidate-linked-validated-symbols.txt` and `final-linked-qualified.json`
retain the disassembly, timestamp and metadata evidence.

## Actual PE correctness

Actual final-image Wine tests pass five cases per PE width: vertex constants,
pixel shader get/set, pixel constants, constant Apply and cross-stage
StateBlock. New public checks cover null/empty/arbitrary-start, the last valid
register and one-past-cap for all six families, plus invalid/empty VS-F writes
while Recording. The existing recorded-value checks remain intact. All ten
cases have zero failures, skips, timeouts or missing verdicts; every staging
sidecar matches the final images. This is getter/StateBlock evidence, not a
draw or full poisoned-adapter matrix.

## Sampler-OFF CPU/FPS spots

Committed `58c67f27` uses Sikarugir cx-24.0.7, named GT2, 1024x768 for 05
and 1280x720 for 06. The perf/Warn recipe enables frame sampling and a
PID-bound two-second process CPU/RSS observer. Inline constants, segmented
transport, opaque-depth index reorder, PE sampler/module map, decimation/full
recorder stats, owner phase split and copy ledger are OFF. Offload stays ON.
No build, test suite or other benchmark runs during the scenes. These are not
observer-free measurements or proof of host isolation.

The original 06 perf r1 (18.205 FPS) is excluded from performance decisions
after the user reported computer use during 06 GT2 and requested a repeat.
Its raw artifacts and original `perf-qualified.json` report remain preserved;
`measurement-qualification.json` marks the exclusion and replacement. The
table below uses the same-binary, same-recipe 06 `user-rerun-r2` at `223f840b`
(documentation-only changes since `58c67f27`), not an average of the two runs.
05 and the separate sampler runs are unchanged.

| Product | Scene FPS | Rows / wall ms | CB / frame | Pass / frame | Draw / frame |
|---|---:|---:|---:|---:|---:|
| 05 GT2 | 29.810 | 1,926 / 64,608.214 | 4.001 | 15.786 | 1,681.702 |
| 06 GT2 | 18.277 | 1,170 / 64,015.071 | 4.002 | 19.859 | 1,880.217 |

05 includes every positive-wall frame 1–1,926. 06 includes positive-wall
rows with more than 100 draws, frames 999–2,168; only loading is excluded,
not slow scene frames. These are renderer frame-log averages, not a decoded
`.3dr` or the catalogue process-elapsed FPS. All five staged hashes match
the final images and all eight chunk/GPU/pipeline/no-pipeline failure
counters are zero. 05 exits normally; 06's permitted timeout closes the
completed-scene result GUI. Summed GPU CB time is 1.998/11.979 ms/frame,
not GPU critical-path latency. No whole-scene pixel comparison is claimed.

| Product | Partial CPU frame increments | Process CPU seconds / elapsed seconds | CPU ms / frame | Average cores | RSS peak KiB |
|---|---:|---:|---:|---:|---:|
| 05 GT2 | 1,892 | 132.17 / 62.318 | 69.857 | 2.121 | 3,310,404 |
| 06 GT2 | 1,116 | 106.33 / 60.325 | 95.278 | 1.763 | 3,799,072 |

These are approximate partial joins across all process threads, not
PE-exclusive times. `qualify_perf.py`, the 05 entry in `perf-qualified.json`
and the replacement `user-rerun-qualified.json` retain the exact populations,
arithmetic, profile, health and binary qualification. The original 06 entry
in `perf-qualified.json` is no longer performance-eligible.
H259's qualified references are 29.682/18.220 FPS and partial CPU of
70.153/95.472 ms/frame; its original interfered 06 perf r1 is excluded.
Current differences are +0.43%/+0.31% FPS and -0.42%/-0.20% partial CPU.
Single sequential spots, different partial intervals and no controlled
cooldown/order counterbalance do not establish gain, neutrality or reduced
CPU consumption. RSS does not establish a memory regression or saving either.

Historical README 30.1/18.1 also differs in the index-reorder setting. Current
05 remains 0.96% below it and 06 is numerically 0.98% above; that is context,
not a matched comparison or repeatable recovery. README is unchanged.

## Complete current PE profiles

Separate 250-Hz sampler runs use the same five images, with Info logging,
module mapping and full self-PC output. Detailed PE phase clocks stay OFF.
The sampler perturbs execution; neither its FPS nor its frame time is
performance evidence.

| Product | Last reported Present | Thread samples | PE samples | Stored / printed buckets | Cross-symbol samples |
|---|---:|---:|---:|---:|---:|
| 05 GT2 | 1,860 | 13,534 | 1,420 | 636 / 636 | 369 |
| 06 GT2 | 2,100 | 14,126 | 866 | 478 / 478 | 213 |

All 31/35 cumulative groups pass coverage and conservation checks: no omitted
or overflow samples, suspend/context/resume failure or target-thread mismatch.
Module containment probes pass, with all eight renderer failure counters zero.
Last reports are cumulative, not sums. For 06, subtracting the pre-heavy
Present-960 report leaves 862 PE samples, 13,388 thread samples and 474
buckets; the first heavy frame is 997. 05 has no earlier report to subtract.
These are approximate scene prefixes; the final unreported tail remains
outside the collection. Complete printed buckets do not mean whole-run coverage.

The exact frozen PE32 image joins all 64-byte buckets. Cross-symbol buckets
remain unassigned; interior padding, inlining and asynchronous Wine PC mapping
still limit attribution. The H260 copy of `join_full_selfpc.py` classifies the
new `ValidatedConstWrite` symbol with constants; H255's older lexical family
classifier does not know that name. Narrow unambiguous counts below exclude
nested lambdas and are not inclusive CPU costs or a performance ranking.

| Linked symbol | 05 samples | 06 samples |
|---|---:|---:|
| `ValidatedConstWrite<16>::applyNonEmpty` | 88 | 48 |
| `SetVertexShaderConstantF` | 55 | 36 |
| `acceptSparseStatePlan` | 24 | 16 |
| `buildSparseStatePlan` | 36 | 22 |
| `preparePeSemanticSparsePlanLayout` | 41 | 17 |
| `PeSemanticPreparedIdentityWitnessT<45>::add` | 21 | 13 |
| `accumulateRetentionDelta<45>` | 57 | 42 |
| `writeSparsePlanPayloadQualified` | 32 | 25 |

`profiles-qualified.json`, full CSV/Markdown reports, symbol JSONs and
scene-prefix reports retain the join. The small counts and changed symbol
boundaries do not independently attribute CPU savings to removed guards.

## Decision and next bounded gate

Linked work removal, bounded native negative controls, actual PE correctness
and fresh wild health pass. Both CPU/FPS spots and both full profiles are
collected. Repeatable FPS recovery and reduced CPU remain unproven; no backend,
provider or experimental default changes follow.

Current source inspection does not justify deleting `accumulateRetentionDelta`:
it runs once per record-unique identity and its `findOrValidateExistingPin`
lookup proves current owner lifetime/identity. The near-ceiling aggregate
capacity check and boundary source requalification are also authoritative.
H255 already rejected a record-local hash table for the observed low unique
count; a new stopped-PC sample is not a reason to revive that design.

One conditional investigation remains in `src/d3d9/d3d9_pe_semantic_owner.hpp`:
compact emission calls `makeQualifiedBinding` and
`pinPreparedQualifiedBinding` for every occurrence, including duplicate
record identities. Before adding per-attempt validation reuse, establish the
actual duplicate-occurrence distribution and prove the typed pin prefix cannot
change across retainer callbacks. Native witnesses must cover all seven binding
families, A/B/A and null, generation/pointer aliases, acquisition failure,
rollback/retry, exact bytes and retain counts. No elimination or gain is
authorized by the present samples alone.
