---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Actual-image compact layout economics and sampled workload shapes"
type: evidence
status: accepted-bounded-helper-economy
updated: 2026-09-08
source: experiments/output/pe-layout-dynamic-economy-20260908/analysis; experiments/output/app-d3d9-3dmark{05,06}-pe-layout-distribution-c03e0fd7-gt2-n127-20260908; experiments/output/pe-three-register-equality-20260908/analysis/h284-default-pixels-{x86,x64}
related: present-pacing-pe-layout-category-rebase.283.md; present-pacing-pe-constant-economy.280.md; present-pacing-pe-post-lifetime-profile.276.md
---

# Actual-image compact layout economics

H284 prices [H283](present-pacing-pe-layout-category-rebase.283.md)'s
replacement of the fixed category-count prepass with active-descriptor rebasing.
The actual PE32 DLL helper is cheaper over the measured count-shape set.
The projected saving is only **15–17 microseconds/Present**, not a demonstrated
whole-frame CPU or FPS improvement. README and provider defaults stay unchanged.

## Separate evidence populations

| Evidence | Population and purpose |
|---|---|
| Frozen before/after DLL microbenchmark | Complete loaded `preparePeSemanticSparsePlanLayout`, not separately compiled timed code |
| Current-image GT2 observer | Every 127th admission's successful compact preparation, before append/settlement |
| Count-shape weighted microbenchmark | Selected vectors covering at least 99% of each sampled interior population |
| Native/default-path codegen/pixels | Observer safety and compatibility, not an economy oracle |

The runtime change adds only the bounded distribution to the existing cold
`PeSemanticOwnerPhaseObserver`. The outlined
`PeSemanticBatchOwner::tryAppendOwnedRecordObserved` consumes
`prepared.sparseLayout`; it does not change the production layout function,
admission witness, retention, settlement, wire or replay policy.

`PeSemanticOwnerCompactLayoutMetric` holds 26 section-count buckets and up to
256 distinct vectors of counts in `kSectionRules` order. It retains existing
keys after saturation, records new-key overflow explicitly, and never evicts
evidence. Conservation is `prepared = complete + invalid + boundedOverflow`;
stored occurrences sum to `complete`. The histogram includes bounded declared
counts even when a descriptor is invalid; a declared count above 25 has no
histogram bucket. Invalid and overflow populations cannot establish complete
shape coverage. This classifier checks kind/count/element-size, not byte-layout
validity; production supplies already validated descriptors.

The table adds about 29 KiB to the optional `PeDiagnosticsState` owner, including
when that owner exists for a different PE diagnostic. With all PE diagnostics
disabled, that owner is absent. No new hot storage, timer, descriptor scan,
allocation, vtable, runtime knob or TLA lifecycle transition is added. Enabled
classification is inside the inclusive admission timer and perturbs it; these
timings must not be reported as product CPU time.

## Actual-image helper method

`analysis/run_layout.py` runs the auxiliary fixture through the canonical Wine
conformance runner. It qualifies the exact DLL/bridge/provider SHA-256s,
PE machine and executable section, unique internal symbol RVA, and initial
instruction prefix. The fixture calls that loaded RVA with the production
C++ types (x86 cdecl / Win64 ABI). No production export or instruction patch
is introduced. PE32/PE64 RVAs are `0x35470`/`0x36360` in these frozen images,
not stable public API addresses.

Before and after each timed batch, the independent generic sparse-layout
oracle must accept and exactly match descriptors, offsets, quotas and bytes.
The timed loop contains only the indirect helper call, validity accumulation,
fixed-width payload-size sink and loop overhead. Full descriptor checksums,
input allocation and layout construction are outside timing. Linked fixture
inspection confirms the repeated indirect call in both PE widths.

The images are H283's frozen `10672a25` baseline and `c03e0fd7` candidate.
Per architecture, process order is baseline-1, candidate-1, candidate-2,
baseline-2; x86 precedes x64, without overlapping build/test/benchmark work.
All eight built-in-shape processes pass, with 20 million calls per shape.
Mean QPC wall nanoseconds/call (selected rows; all ten are in
`verified-helper-economy.json`):

| Shape | PE32 before → after | PE64 before → after |
|---|---:|---:|
| Empty | 19.703 → 16.432 | 18.554 → 13.092 |
| One scalar | 21.857 → 17.322 | 19.501 → 13.633 |
| All scalar families | 46.828 → 45.447 | 84.327 → 29.665 |
| VS float, 256 registers | 31.269 → 29.405 | 33.920 → 19.884 |
| Mixed, 14 sections | 128.081 → 83.811 | 291.027 → 54.269 |
| Dense, all 25 sections | 251.984 → 210.955 | 550.416 → 120.316 |

QPC includes loop/indirect-call overhead and host scheduling; warm repetition
does not reproduce workload cache state. Raw `threadCpuNs` fields are retained
but **not used** for economics: `GetThreadTimes(GetCurrentThread())` is not
qualified as thread-exclusive on this Wine runtime, and its observed resolution
is coarse. See [H280](present-pacing-pe-constant-economy.280.md)'s Wine timing
scope finding. These are not PE-exclusive CPU measurements.

## GT2 input distribution

Both actual PE32 scenes run on Sikarugir `sikarugir-cx-24.0.7`, with offloaded
replay ON; inline constants, segmented transport, streaming and index reorder
OFF. Resolution is 1024×768 for 05 and 1280×720 for 06. The recipe uses
`perf` with Info logging, frame sampling 1 and semantic-owner phase sampling
127. These runs collect distribution, **not qualified FPS**. 06 uses the
focus-free autorun selector. Both runs pass with zero chunk rejects, GPU errors,
pipeline-build failures or no-pipeline skips, and all five staged hashes match.

Each cumulative header/row/footer group carries a Present ordinal and totals.
The parser rejects truncated groups, kind-order drift, invalid/overflow evidence
or failed conservation. It differences the first and last complete report
inside a contiguous scene (`wall_ms > 0`, `draw_calls > 100`), excluding both
startup and the uncovered scene tail. This is an interior-report population,
not all frames or all emitted records.

| Quantity | 05 GT2 | 06 GT2 |
|---|---:|---:|
| Scene frame IDs | 1–1765 | 927–2041 |
| Complete report endpoints | Present 60 → 1740 | Present 960 → 2040 |
| Differenced Presents | 1680 | 1080 |
| Exact compact admission attempts | 2,879,795 | 2,056,848 |
| Rejected attempts in interval | 0 | 0 |
| Attempts/Present | 1714.164 | 1904.489 |
| Sampled successful preparations | 22,655 | 16,276 |
| Mean active sections | 2.528 | 2.556 |
| Zero-to-two-section sample share | 67.049% | 65.968% |
| Distinct interior count vectors | 110 | 168 |
| Priced vectors / coverage | 48 / 99.0068% | 84 / 99.0231% |

No shape-table overflow or invalid sample occurs. The sampling is deterministic,
not randomized; periodic aliasing and diagnostic influence remain limitations.
The selected vectors reproduce bounded kind counts using low mask bits and
constant starts at zero. They do not preserve original mask positions, register
starts, payload data, destination arrival sequence or cache history. Standalone
constant/generic admissions are not part of this compact population.

## Weighted result and size of the opportunity

Eight further actual-image processes pass all 132 selected cases, at two million
calls per case, in the same counterbalanced order. Checksums, schema order,
case sets, iteration counts, fixture/input hashes and raw receipts reconcile.
The means below are conditional on the selected shapes; the unpriced roughly
1% tail is not silently treated as an exact full-population estimate.

| Workload weights | PE32 before → after, ns/call | PE32 local change | PE64 before → after, ns/call |
|---|---:|---:|---:|
| 05 GT2 | 30.459 → 21.436 | −29.62% | 37.293 → 16.389 |
| 06 GT2 | 30.772 → 21.710 | −29.45% | 37.821 → 16.464 |

Multiplying the selected-shape mean saving by its sampled coverage and exact
attempts/Present projects **15.313/17.090 microseconds/Present** for PE32.
This is dimensional pricing of the covered count-shape contribution, not a
measured frame-time saving, statistical bound or whole-PE CPU attribution.
The much larger PE64 percentage cannot be applied to these PE32 games.

The local economics gate is therefore answered for this bounded warm domain:
rebasing is cheaper than the removed prepass. Remaining CPU work must be priced
separately, particularly complete compact admission/materialization/retention
and constant handling from the [current PE attribution](present-pacing-pe-post-lifetime-profile.276.md).
Further layout-only polishing is a small-budget candidate; it cannot plausibly
account for the entire remaining README gap at the measured scale.

## Verification and provenance

- Final arm64 native 9/9 and x86_64 native 4/4 pass. Tests cover zero/full
  layouts, repeated/invalid kinds, declared-count overflow, all 256 shape keys,
  new-key saturation, duplicate-after-full, compact-only adapter accounting,
  and existing enabled/disabled final-wire equality.
- Both PE widths preserve all 12 audited public hot-entry metrics, exports
  and vtable/RTTI archive ownership. The complete priced layout helper and
  lambdas retain their normalized instruction sequence. Two PE64 schema-table
  address changes are accepted only after matching all 500 schema bytes and
  checking the exact RIP-relative relocation delta; this is not blanket masking.
- Final default-path PE32/PE64 readbacks match all 192 H283 baseline pixel
  samples; both device teardown paths return zero. This is bounded pixel
  evidence, not a new concurrency or exhaustive rendering proof.
- `final-audit.json` binds the runtime patch, test logs, distributions,
  weighted receipts, pixel logs and all five restored current Wine images.
  Initial fixture-staging and native-test setup failures remain in the artifact
  directory; only explicitly qualified rounds enter the result.

Observer runtime source is `c03e0fd7 + analysis/observer-runtime.patch`
(SHA-256 `0e4e1d1c6070602d0b7299590f0367e10a8b7d8176b465d4219e8b34f10c537c`).
Recipes retain full image hashes. The original built-in fixture is frozen as
`layout_economy_builtin.cpp` (SHA-256
`8e3deb631cb37fc6f568d7f2b0da476aeda28d23721fd303568e2d19a64b4417`);
the file-driven fixture and all selected vectors remain beside it. Recompute
with `scripts/run_python.sh` and `summarize_distribution.py`,
`summarize_layout.py`, `summarize_weighted.py`, then `final_audit.py` in that
analysis directory. No new TLC run, whole-workload CPU saving or README
recovery is claimed.
