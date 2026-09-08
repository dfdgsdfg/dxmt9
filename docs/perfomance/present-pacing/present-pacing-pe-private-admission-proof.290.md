---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Private compact admission: reuse the callback-free source proof"
type: evidence
status: measured-runtime
updated: 2026-09-08
source: experiments/output/pe-owner-private-admission-20260908/analysis
related: present-pacing-pe-wire-publication-fusion.289.md; present-pacing-pe-retainer-epoch-shapes.287.md; ../../../specs/d3d9/recorder/requirements.md
---

# Private compact admission: reuse the callback-free source proof

H290 removes one redundant compact source qualification, not an ownership or
failure boundary. Against `8c3b9466`, seven warm matched-header append shapes
are cheaper in both actual PE widths. These component QPC intervals are not
exclusive CPU, loaded production-parent timing, workload-weighted savings,
or FPS. H289's rejected first-use wire-publication fusion remains absent.

## Contract and implementation

In `src/d3d9/d3d9_pe_semantic_owner.hpp`, each public append constructs its own
private witness through `prepareAdmission`. That pass rejects stale/null sources,
unfinalized destinations, full snapshots and source/record-type mismatch.
It performs no retain or caller callback. The observed path and admission
ledger perform synchronous bookkeeping, not source mutation.

The compact `validAdmissionHeader` therefore reuses that source proof instead
of calling `PeSemanticSparsePlanRecordInput::qualifiedSource` again. It still
checks producer/type/flags and zero or nonmonotonic source/record ordinals.
Private destination-frontier binding remains; no public mutating prepared
entry, extra witness, storage, branch, or runtime flag is added.
`writeSparsePlanPayload` still freshly qualifies its source before payload
reads. Retain-time binding identity, physical pins, canonical layout, rollback,
settlement and all generic/constant paths are unchanged.

R-CORE-REC-7.65 records this callback-free proof reuse. It refines an existing
synchronous operation rather than changing a queue/lifetime transition; the
existing recorder models remain applicable. Native public-entry counterexamples
and canonical differentials bind the implementation; TLA and pixels are
complementary evidence, not proof of execution time.

## Actual PE append economy

`economy-qualified.json` conserves eight sequential processes: baseline and
candidate, PE32 and PE64, two repeats each. Each measures seven shapes after
eight warm groups, then 2,000 timed groups of 64 appends per shape. The
fixture imports the immutable H289 public-append fixture and compiles against
the corresponding frozen production headers. Real resources and C-ABI
callbacks come from the matching staged DLL/provider images.

Arithmetic mean of the two process means, ns/append:

| Shape | PE32 baseline | PE32 candidate | Delta | PE64 baseline | PE64 candidate | Delta |
|---|---:|---:|---:|---:|---:|---:|
| Empty | 168.64 | 162.87 | -3.42% | 114.26 | 104.35 | -8.68% |
| One texture | 266.55 | 260.06 | -2.44% | 164.03 | 157.28 | -4.11% |
| Two unique textures | 325.73 | 321.25 | -1.37% | 195.82 | 185.57 | -5.23% |
| Duplicate textures | 307.08 | 301.10 | -1.95% | 188.05 | 178.14 | -5.27% |
| Null texture | 223.48 | 220.20 | -1.47% | 149.70 | 139.54 | -6.79% |
| Two streams | 322.93 | 318.87 | -1.26% | 199.57 | 184.09 | -7.76% |
| 20 distinct textures | 1,579.91 | 1,558.68 | -1.34% | 811.38 | 794.44 | -2.09% |

The group starts with existing physical pins. Reset, seed append, expected
owner construction, checksums and byte oracles are outside the timed group;
the accepting callback is counted. Empty-QPC calibration is reported separately,
not subtracted. Independent record/handle counts, remaps, identities and payload
checks and first/final ExactFixed-versus-segmented checks pass; checksums agree
across both versions/repeats within each width. These are not production
PendingDelta/capture callbacks, allocation-miss shapes, or game-arrival traces.
There is no statistical confidence interval or extrapolated ms/Present claim.

## Linked code and verification

`codegen-report-qualified.json` reads the exact H290 frozen DLLs and actual
next-symbol bounds, including generated append lambdas. The duplicate helper
disappears (baseline body 308 B PE32, 243 B PE64). This does not mean every
caller shrinks: the PE32 unobserved nested append body grows 483 to 535 B and
its stack subtraction grows 176 to 188 B; PE64 body shrinks 493 to 483 B,
with its 344 B stack unchanged. A checkpoint constructor call replaces the
source call. The writer and compact materializer are unchanged in body size,
stack and call counts. Component measurements price this code-generation change.

- Four canonical builds pass without warnings; configurations match. Only
  the semantic-owner header differs in `src/include/cross`; bridge/provider
  images remain byte-identical. `baseline.json` and `candidate.json` preserve
  source/header and five-image identities, with build logs alongside them.
- `native.jsonl`: eight focused native targets pass. The new
  `compactPublicEnvelopeRejectsAndRetries` exercises seven rejection shapes in
  observed/unobserved modes, classification, settlement non-entry, prefix bytes,
  clean retry, ref balancing and ExactFixed/segmented parity. Existing stale-plan,
  retain-mutation and maximum-family differentials remain green.
- `tla.jsonl`: full `dxmt9-verify-tla` passes. No new lifecycle model is claimed.
- `pixels-{x86,x64}-{baseline,candidate}/pixels.json`: 24 four-pixel signatures
  per process, exact candidate/baseline match, independent channel tolerance 3,
  clean device release. This is a bounded actual-PE oracle, not all-family or
  full-scene pixel equivalence.
- `parser-tests.log`: valid input and 16 negative parser controls pass. Fixture,
  manifest and staged-image hashes are checked around every run.
- `final-audit.json`: rechecks the integrated source/image set, native/TLA,
  component/pixel receipts, both scene intervals and the final staged candidate.

## Integration and remaining scope

The current candidate was then staged through `run_experiment.py`, once per
GT2 workload, without concurrent builds/tests or other benchmarks. The Sikarugir
`sikarugir-cx-24.0.7` perf profile uses warn logging and frame sampling; PE
phase/sampler diagnostics are off. Offload replay is on; inline constants,
segmented transport, CPU-ready tape and index reordering are off, matching H287.

| GT2 | Resolution | Selected frames | Wall ms | FPS | README | Difference |
|---|---|---|---:|---:|---:|---:|
| 05 | 1024x768 | 1–1926, all positive-wall rows | 64,610.683 | 29.8093 | 30.1 | -0.97% |
| 06 | 1280x720 | 965–2134, `draw_calls > 100` | 64,061.985 | 18.2636 | 18.1 | +0.90% |

`{05,06}-candidate-perf-r1-measurement.json` binds the selected contiguous
interval, recipe, exact sources and five staged images. These are elapsed scene
averages, not benchmark-owned `.3dr` FPS or an A/B attribution. 05 exits normally;
06 finishes its scene and uses the catalogue-permitted post-scene GUI timeout.
All eight health counters are zero at the last full snapshot: through frame
1927 for 05 and 2100 for 06. The last 34 logged 06 frames are outside that full
snapshot; do not extend the eight-counter health claim across them. Both saved
forest frames render visible geometry/effects without an obvious gross defect;
neither snapshot proves full-scene temporal correctness.

Keep the narrow product change for removed work and measured append economy.
The spots are within about 1% of README, but do not establish a repeatable
whole-game gain, a complete FPS recovery, or a CPU ceiling. README stays
unchanged. The next optimization must retain the same distinction between
removed work, actual PE economics, and end-to-end scene throughput.
