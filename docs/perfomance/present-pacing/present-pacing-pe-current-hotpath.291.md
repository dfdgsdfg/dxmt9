---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Current PE hotpath profiles and a fresh 06 GT2 remeasure"
type: evidence
status: measured-runtime
updated: 2026-09-09
source: experiments/output/pe-owner-current-hotpath-H291-20260908/analysis
related: present-pacing-pe-private-admission-proof.290.md; present-pacing-pe-stream-binding-reuse.285.md; present-pacing-pe-layout-dynamic-economy.284.md
---

# Current PE hotpath profiles and a separate 06 GT2 remeasure

H291 refreshes actual PE32 attribution at committed `627d9840`. It does not
change production code. The old H285 image is not used as current source
evidence. Owner admission/emission and constant handling remain investigation
targets, but PC samples do not establish their exclusive CPU time or the
saving available from a particular rewrite.

## Observer-off FPS remeasure

A fresh run follows the user's report of desktop activity during an earlier
06 GT2 measurement. Do not use the activity-affected run for comparison or
combine it with this run into an A/B population.

`app-d3d9-3dmark06-pe-current-H291-gt2-off-perf-r2-20260909` reports
**18.2167 FPS** over 1,167 consecutive selected frames, 968–2134,
64,062.218 ms. Selection is positive `wall_ms` with `draw_calls > 100`,
including the same loading-tail mixture as H290. This is renderer-side
frame sampling, not benchmark-owned `.3dr` FPS. README's 18.1 is 0.64% lower;
one spot establishes neither a repeatable improvement nor causal attribution.

Conditions: 1280×720, Sikarugir `cx-24.0.7`, perf/warn, validation off,
offload replay on, inline constants/segmented transport/CPU-ready Tape/index
reorder off. PE sampler, phase diagnostics and CPU observer are off. No build,
native test or second Wine benchmark overlaps the render interval.

The catalogue permits the post-scene GUI timeout. Eight health counters are
zero through ordinal 2100; the remaining 34 frame rows are outside that full
snapshot. The saved forest/effects image was inspected, but is not proof of
whole-scene temporal correctness. The authoritative receipts are
`06-perf-r2-recipe.json` and `06-perf-r2-measurement.json`. README is unchanged.

## Exact-image diagnostic profiles

Separate 05/06 GT2 runs use the full-self-PC sampler at 250 Hz with info
logging. Suspending the device-creating thread perturbs execution: these runs
are **not FPS or CPU-consumption evidence**. They run serially, at 1024×768
and 1280×720 respectively, with the same provider settings as the FPS spot.

`runtime-build.json` binds the current tracked `src/include/cross` identities
and five images to the frozen H290 candidate. The candidate manifest's parent
HEAD is `8c3b9466`; its then-uncommitted source is exactly what `627d9840`
commits. All three staging builds report no work to do. PE32 image SHA-256 is
`dbd909eecd09b712bf973956fbc74eb9e1cb1265e4a6c1c7855881798de3ef86`;
the companion PE64/bridge/provider hashes and source checks are preserved
before and after each run. Only PE32 execution is sampled, not native ARM64
or PE64 execution.

Run suffixes are
`pe-owner-current-hotpath-H291-627d9840-gt2-off-fullselfpc-r1-20260909`
under the respective `app-d3d9-3dmark05-` and `app-d3d9-3dmark06-` roots.
Every emitted sampler group passes completeness/conservation checks: no
omission, histogram overflow, allocation fallback, target-thread mismatch,
suspend failure, context failure or resume failure. The qualifier rejects a
malformed final group rather than silently selecting an older report.

| Diagnostic scope | 05 GT2 | 06 GT2 |
|---|---:|---:|
| Full groups checked | 31 | 35 |
| Final sampler Present ordinal | 1860 | 2100 |
| Prefix subtraction ordinal | None | 960 |
| Thread samples in resulting prefix | 13,518 | 13,641 |
| `d3d9.dll` samples in prefix | 1,339 | 808 |
| Cross-symbol samples left unassigned | 352 | 217 |
| Last selected scene frame | 1873 | 2114 |
| Frames beyond full health snapshot | 0 | 14 |

Sampler and runtime-health coverage are different. The 05 runtime health
report covers its final scene, but its sampler stops 13 frames earlier. The
06 sampler/health reports both stop at 2100. Prefix subtraction uses Present
ordinals, not exact scene timestamps; startup and a gap before the first
heavy frame are not perfectly isolated. Module rows conserve these observed
prefixes, but the reporter is still a bounded top-20 module report.

## Remaining named PE work

Exact-name, non-crossing bucket labels from `05-scene_prefix-symbols.json`
and `06-scene_prefix-symbols.json`:

| Named body, not inclusive call-tree cost | 05 samples | 06 samples |
|---|---:|---:|
| `ValidatedConstWrite<16>::applyNonEmpty` | 64 | 41 |
| `D3D9DeviceImpl::SetVertexShaderConstantF` | 49 | 34 |
| `preparePeSemanticSparsePlanLayout` | 43 | 31 |
| `acceptSparseStatePlan` | 37 | 13 |
| Compact `accumulateRetentionDelta` | 29 | 26 |
| Compact `writeSparsePlanPayloadQualified` main body | 28 | 24 |
| `PeSemanticPreparedIdentityWitnessT<45>::add` | 21 | 15 |

The broader lexical owner family contains 578/338 samples, and the constant
family 192/116. Template argument names can influence family classification;
these are navigation groups, not semantic partitions of measured CPU work.
64-byte PC buckets are not stacks. Cross-symbol buckets stay unassigned,
interior padding still needs disassembly, and neither family nor named-body
counts may be converted into additive ms/Present or compared with H285 as a
CPU-regression delta.

The linked main writer's 28/24 samples include header validation and active
section dispatch. Per-section copy and binding work also lives in outlined
generated helpers. Main-body samples therefore do not isolate the cost of
the proposed descriptor-table change.

## Bounded next candidate

Source audit of `src/d3d9/d3d9_pe_semantic_owner.hpp` finds a concrete remaining
duplication in `writeSparsePlanPayloadQualified`. Preparation already creates
the active prefix of `PeSemanticPreparedSection` in canonical order, with
header/table-biased payload offsets. The writer nevertheless computes each
descriptor destination and copies each descriptor separately after its payload
visitor. A single exact active-prefix copy after successful section emission
could remove repeated table-offset checks and small copies.

This is **not implemented or economically qualified**. It must preserve
writer-time source qualification, all kind/count/size/offset checks, padding,
identity/pin/remap checks, section visitors, final cursor checks and rollback.
Compile-time size/alignment/field-offset and trivially-copyable layout locks
must bind the prepared descriptor to `D9CCommandChunkWireSectionDesc`; never
copy the inactive 25-entry tail. This is independent of H289's rejected
first-use handle-publication fusion, which remains absent.

The existing seven H290 append shapes do not price descriptor-count scaling:
two texture rows and twenty texture rows still occupy one descriptor. A
candidate needs genuine 0/1/2/4/many/25-section cases, independent scalar,
FFP, constant and UP byte oracles, malformed layout/failure/retry pins,
and separate loaded-writer versus public-append economics in both PE widths.
Preparation and reference emission stay outside timed regions. H284's old
shape distribution is useful context, not a current workload weighting.
The safe candidate is small; no FPS benefit follows from fewer source loops.

## Receipts and limits

`profiles-qualified.json` verifies source/image/recipe bindings and all sampler
groups. `symbols-qualified.json` binds the exact CSV/image joins and explicit
prefix/tail scopes. `final-audit.json` rechecks their hashes, the separate FPS
receipt, and the repository sampler parser tests. The latter include malformed
or truncated final coverage, fallback and conservation counterexamples.

No production edit, new lifecycle proof, pixel differential, runtime promotion
or PE64 economy measurement is claimed by this evidence-only step. H290's
native/TLA/pixel evidence remains historical evidence for the unchanged code;
it is not reported as rerun here.
