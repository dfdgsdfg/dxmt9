---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Bounded changed-constant copy"
type: evidence
status: correctness-verified-no-demonstrated-fps-gain
updated: 2026-09-07
source: experiments/output/pe-constant-copy-20260907/analysis; experiments/output/app-d3d9-3dmark05-pe-constant-copy-5d764f37-gt2-perf-r1-20260907; experiments/output/app-d3d9-3dmark06-pe-constant-copy-5d764f37-gt2-perf-{r1,r2,r3}-20260907; experiments/output/app-d3d9-3dmark{05,06}-pe-constant-copy-5d764f37-gt2-sampler-r1-20260907
related: present-pacing-pe-autogen-bound-slots.256.md; present-pacing-pe-constant-bitcompare.254.md; ../../../specs/d3d9/recorder/spec.md
---

# Bounded changed-constant copy

## Decision and scope

At `741a416a`, the frozen [H256](present-pacing-pe-autogen-bound-slots.256.md)
PE32 image still copies each changed 16-byte constant with four sequential
32-bit load/store pairs after vectorized equality. Its unambiguous helper
samples are 93/55 for 05/06 GT2. The `0x23080` buckets hold 13/9 samples and
include both copying and dirty bookkeeping; `0x230c0` holds 9/3 and includes
the post-copy loop. Neither bucket is copy-exclusive time. Counts from an
older DLL or an inclusive/cross-symbol bucket cannot establish copy cost.

Commit `5d764f37` expresses this transfer as `__builtin_memcpy` with a constant
16-byte extent in `constShadowFixedElemCopy`. The four-byte path, bitwise
equality, growth, dirty range, Recording behavior and canonical wire remain
unchanged. No new storage, cache, runtime flag, queue policy or bridge ABI is
introduced. This lets the x86 PE compiler choose SIMD under Rosetta; it is
not a native ARM64 PE implementation or a direct Darwin call.

Caller input and private Live storage are separately owned. Exact self-alias
without growth is handled by equality before copying. Partial overlap and
internal aliases across resize are not supported contracts of the old or new
path; the generic reference already uses `memcpy`. No alignment requirement
or floating-point normalization is added.

## Verification

All four canonical builds pass. Native focused tests pass 7/7, including
constant value/flush, producer differential, shadow, semantic owner and ABI
surface checks. Fresh-process decimation-enabled value/flush checks pass 2/2;
x86_64 value/flush and provider install-name checks pass 3/3.

The direct helper matrix checks all 16 x 16 source/destination byte offsets,
four raw-bit patterns including NaNs and signed zero, and every single-bit
variant of a 128-bit pattern: 33,792 copies. It compares against independent
bytes and verifies destination canaries and unchanged source storage. These
canaries detect excess writes, not overreads; no guard-page or sanitizer
claim follows. Linked instructions independently show the intended exact
16-byte load/store. Existing generic-reducer differential cases retain all
six families, equal/changed/mixed values, dirty-prefix, self-alias, null,
overflow and growth checks. This changes byte materialization, not the state
transition model; it adds no new TLA transition or concurrency proof.

Actual Sikarugir PE32 and PE64 conformance each pass five cases with zero
skips: vertex/pixel F/I/B value roundtrips, pixel shader set/get, constant
StateBlock Apply/Capture and cross-stage Apply. Runs use binary table ranges
`[36,39)` and `[68,70)`, with matching staged images. These are public D3D9
value/getter proofs, not a new GPU pixel-differential oracle.

## Linked evidence and identity

Both actual PE images replace eight scalar copy instructions with one
unaligned SIMD load and store. The source is reloaded after comparison; the
comparison register is not reused. The outlined helper shrinks from 624 to
610 bytes in PE32 and 707 to 687 in PE64. Existing cold growth calls remain;
there is no new per-element library call, not an absence of all `memcpy`.
Exports, vtable/RTTI ownership, build options and the 12 manifest hot-entry
metrics match H256. The manifest does not include this outlined helper.
DLL sizes remain 1,905,664/1,771,520 bytes; no object layout changes.

Final PE32 SHA-256:
`43b7da5b28e223b8bd5a52c2c63cfa985a27ae1f541736d6e3e604086dfce722`.
Final PE64 SHA-256:
`cb503ce41826308d31997ff71437f12de757e11c540cbe00191d840e438de241`.
`candidate-linked-final.json`, `linked-comparison.json`,
`constant-linked-metrics.json` and frozen DLL/disassembly pairs retain the
comparison. Provider and both bridges are byte-identical to H256.

## Sampler-OFF runtime spot checks

Committed `5d764f37` uses the same Sikarugir cx-24.0.7 perf recipe as H256:
05 GT2 at 1024x768 and 06 GT2 at 1280x720; frame sampling ON, index cache OFF,
PE sampler/module map, detailed timers, phase split and copy ledger OFF. A
read-only process CPU/RSS observer polls every two seconds. No build, test
suite or GPU trace runs alongside either benchmark. This is not completely
observer-free or an isolated host. The current index-cache selection differs
from the historical README configuration; README is a reference, not a fresh
matched baseline.

| Product / run | Scene FPS | README FPS | Delta | Rows / wall ms | CB / frame | Pass / frame |
|---|---:|---:|---:|---:|---:|---:|
| 05 GT2 r1 | 29.052 | 30.1 | -3.48% | 1,881 / 64,745.589 | 4.001 | 15.792 |
| 06 GT2 r2 | 18.083 | 18.1 | -0.09% | 1,160 / 64,148.167 | 4.002 | 19.861 |

05 includes all positive-wall rows, frames 1–1,881. 06 includes positive-wall
rows with more than 100 draws, frames 1,012–2,171; loading is excluded but
slow scene frames remain. Both results pass, with matching five-artifact
staging receipts and zero recorded chunk rejections, GPU CB errors, pipeline
build failures and no-pipeline skips. 05 exits normally; 06's allowed timeout
closes the result GUI after the complete scene. The N/A score dialog is
expected for a single graphics test without the full graphics/CPU suite.
Captures at 1:00.35 / 0:41.32 show rendered forest frames, not whole-scene
pixel equivalence. Draws per frame are 1,680.007/1,877.385; summed GPU CB times
are 2.051/12.038 ms per frame, not GPU critical-path latency.

The first 06 GT2 perf run reports 16.584 scene FPS and passes renderer health,
but the user confirmed using the computer during it. This run is excluded
from performance, regression, neutrality and README comparisons; its raw
artifacts remain preserved. `06-perf-r1-qualification.json` records the reason
and identifies r2 as its replacement. The invalid run is not used to claim a
speedup for unchanged code. `perf-measurements.json` now selects only 05 r1
and 06 r2; the earlier aggregate is explicitly retained as
`perf-measurements-includes-invalid-06-r1.json`.

Partial, approximate process/frame joins retain 1,799/1,104 frame increments:
128.00/105.91 process CPU seconds over 60.287/60.253 elapsed seconds,
71.151/95.933 CPU ms per frame and 2.123/1.758 average cores. Sampled RSS
peaks are 2,915,860/3,103,132 KiB. These include all process threads and
different partial intervals, not matched or PE-exclusive costs. Host samples
are retained, but do not prove an interference-free run or CPU reduction.

06 rounds to README 18.1 in this spot check; 05 remains below 30.1. H256's
contended 06 value is not a causal baseline. Fewer static copy instructions
have not demonstrated FPS gain, repeatable recovery or lower CPU consumption.
The separate profiles below close collection for the committed change, not
exact copy-cost attribution. README remains unchanged.

### Additional requested 06 repeat

At documentation HEAD `718472a5`, r3 repeats the unchanged `5d764f37`
runtime at 1280x720 with the same perf profile, five staged artifact hashes
and two-second process observer. Canonical build checks were no-ops before
launch; no build, tests or separate benchmark ran during the scene. The
existing r2 remains preserved, and user-interfered r1 remains excluded.

R3 measures **18.254 FPS**, 1,170 positive-wall heavy-draw rows, frames
990–2,159 over 64,094.613 ms. This is +0.85% versus the historical README
18.1 reference, not a matched code-gain result. CB/pass/draw counts per frame
are 4.002/19.862/1,879.119; summed GPU CB time is 11.889 ms/frame. Status is
pass with zero retained renderer failure counters; the permitted timeout
closes the completed-scene result GUI. The capture at 0:36.01 shows the
rendered forest, not whole-scene pixel proof. No `.3dr` was emitted, so FPS
comes from the frame log rather than the process-elapsed estimate.

`06-perf-r3-measurements.json` retains the calculation and identity checks.
The partial process join covers 1,146 frame increments and 109.80 CPU seconds
over 62.396 seconds: 95.812 CPU ms/frame, 1.760 average cores and sampled RSS
peak 3,469,608 KiB. As above, these are approximate all-thread measurements,
not a PE-exclusive reduction or an isolated-host claim. R3 is a separate
same-binary spot check; it does not replace r2 in `perf-measurements.json` or
change README.

## Complete current PE profiles

The same frozen images have separate 250-Hz full self-PC runs, with module
mapping and Info logging enabled. Detailed timers, phase split and the copy
ledger remain OFF. The sampler suspends the game thread; its FPS and derived
sample weights are not performance or measured CPU-time evidence.

| Product | Last reported Present | Thread samples | PE samples | Buckets | Omitted / overflow | Cross-symbol ambiguous samples |
|---|---:|---:|---:|---:|---:|---:|
| 05 GT2 | 1,800 | 13,555 | 1,378 | 633 | 0 / 0 | 358 |
| 06 GT2 | 2,100 | 14,256 | 872 | 484 | 0 / 0 | 214 |

All 30/35 report groups validate, module probes pass, and no target-thread
mismatch or suspend/context/resume failure is present. Both catalogue results
pass with matching five-artifact receipts and zero retained renderer failure
counters. 06 uses the permitted completed-scene GUI timeout. The approximate
06 scene-prefix subtraction starts at report 960 before first heavy frame
998, retaining 869 PE/13,515 thread samples in 481 buckets through report
2,100. The unreported tail is absent; this is not an exact scene interval.
`profile-verdicts.json` records all checks and narrow symbol totals against
the frozen PE32 hash, excluding nested lambdas from named parent totals.

| Unambiguous outlined symbol | 05 samples | 06 samples |
|---|---:|---:|
| `applyConstShadowLiveFixedWidth<16>` | 86 | 44 |
| `accumulateRetentionDelta<45,...>` | 66 | 25 |
| `SetVertexShaderConstantF` | 48 | 35 |
| `acceptSparseStatePlan` | 48 | 21 |
| `buildSparseStatePlan` | 43 | 15 |
| `PeSemanticPreparedIdentityWitnessT<45>::add` | 26 | 25 |
| `writeSparsePlanPayloadQualified` | 26 | 24 |
| `preparePeSemanticSparsePlanLayout` | 25 | 28 |

These are PC counts, not inclusive stacks or an additive timing ledger. A
64-byte bucket can also contain padding; source/disassembly inspection still
matters. The fixed16 helper has 86/44 samples versus H256's 93/55, while its
`0x23080` copy-plus-bookkeeping buckets have 8/3 versus 13/9. Different full
populations, host load and moved instructions prohibit a causal saving claim.
The comparison/setup bucket `0x23040` still holds 50/26 samples; copying is
not the entire constant loop's cost. Module shares also cannot separate
active CPU from waits or Wine's saved cross-boundary context.

## Next bounded investigations

The source audit identifies possible duplicate work, not proven FPS gains:

- `acceptSparseStatePlanWithSources` checks the same PendingDelta ticket
  before validation and again before consumption. The intervening constant
  and concrete token-ledger operations cannot mutate `PeHotStateShadow`;
  retain the initial authority check and test stale-owner/epoch, transaction
  identity, token failure and retry before consolidating the later check.
- The default inline-constant-OFF route still constructs two six-entry pointer
  arrays and visits absent constant ranges during acceptance/clear. An
  immutable presence guard could bypass that scaffolding, but ON mutable
  dirty-range validation must remain: constants do not share the PendingDelta
  generation witness. Linked code must establish that scaffolding survives
  optimization before assigning value to its removal.
- A public-entry-produced constant-range witness could avoid repeating
  nonzero/null/overflow checks in the Live kernel. It must be bound to the
  exact call and preserve the `writeAllowed`/Recording/zero-count envelope;
  the generic checked helper remains the reference. This is conditional on
  linked code and tests, not permission to bypass terminal state handling.

The canonical retain lookup and identity validation are necessary ownership
work. Their profile residency is not a reason to remove them. Actual CPU
reduction and repeatable recovery of both README targets remain open.

A larger mechanism probe should precede further instruction-only tuning:
remeasure the existing default-off `DXMT9_PE_INLINE_CONST_DELTA` against its
OFF control on these same binaries. OFF admits standalone constant records;
ON folds ranges into the sparse draw's semantic-owner transaction and settles
them through `acceptSparseStatePlanWithSources`. The generic compatibility
route uses `acceptInlineConstantDelta`; those are not the same call site.
Setter shadow comparison/copy does not disappear, and ON adds range
preparation and mutable-constant validation. Native producer differential,
record-value, binding-reuse, owner and importer/coalescer gates must remain
green before a bounded current mechanism run. No default switch follows.

[H214](present-pacing-inline-const-delta.201.md) confirmed fewer append/flush
events on the July recorder but kept the flag opt-in after a noise-sized
FPS result. Its historical uncalibrated timing estimate is not a current
CPU saving. The later semantic-owner and compact-acceptance migration changes
the transaction work being removed, justifying one fresh mechanism comparison
rather than inheriting either the old ceiling or an assumed win. Pin index
reorder OFF in both arms and keep phase-observer runs separate from FPS.

README's 30.1/18.1 values entered at `a5671697` on August 29 before
`8981494f` changed the shared perf-profile index-reorder default from ON to
OFF. That is a configuration mismatch to preserve in any comparison, not a
measured explanation for the remaining FPS gap. Re-enabling a backend option
would not by itself establish a PE optimization gain.
