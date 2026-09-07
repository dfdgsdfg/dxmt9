---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Bounded changed-constant copy"
type: evidence
status: correctness-verified-no-demonstrated-fps-gain
updated: 2026-09-07
source: experiments/output/pe-constant-copy-20260907/analysis; experiments/output/app-d3d9-3dmark05-pe-constant-copy-5d764f37-gt2-perf-r1-20260907; experiments/output/app-d3d9-3dmark06-pe-constant-copy-5d764f37-gt2-perf-{r1,r2}-20260907
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
The committed change's separate full PE self-PC profiles and exact copy-cost
attribution remain open; H256's older histogram is selection evidence only.
README remains unchanged.
