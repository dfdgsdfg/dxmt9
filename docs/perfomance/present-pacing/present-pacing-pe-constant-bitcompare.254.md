---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Current PE attribution and branch-reduced constant equality"
type: evidence
status: spot-checked-not-promoted
updated: 2026-09-07
source: experiments/output/pe-current-attribution-20260907/analysis; experiments/output/app-d3d9-3dmark05-pe-current-attribution-249718d0-gt2-sampler-r1-20260907; experiments/output/app-d3d9-3dmark06-pe-current-attribution-249718d0-gt2-perf-r1-20260907; experiments/output/app-d3d9-3dmark06-pe-current-attribution-249718d0-gt2-sampler-r1-20260907; experiments/output/app-d3d9-3dmark{05,06}-pe-constant-bitcompare-6c3ca092-gt2-perf-r1-20260907; experiments/output/app-d3d9-3dmark{05,06}-pe-constant-bitcompare-6c3ca092-gt2-sampler-r1-20260907
related: present-pacing-pe-canonical-authority.253.md; ../overview-3dmark06-gt2.md; ../../../specs/d3d9/recorder/requirements.md
---

# Current PE attribution and constant equality

## Baseline and decision

Base `249718d0` has the H253 implementation (`f997e237`). The prior clean
05 GT2 run is 29.009 sampled FPS; README retains 30.1. A fresh 06 GT2 run at
1280x720 is 17.926 scene FPS, versus 18.082 re-derived from the README's
`app-d3d9-3dmark06-readme-head-20260829-gt2-final` artifact (rounded 18.1).
Both 06 populations use positive wall rows with more than 100 draws: this
excludes the loading animation, not slow scene frames. The same populations
result at cutoffs 10 and 500. Candidate baseline has 1,155 rows / 64,431.887ms;
the historical run has 1,159 / 64,097.547ms. The all-row result includes loading
and is not scene FPS. Historical opaque-depth index-cache selection is ON,
current OFF; no clean causal comparison to that build is claimed.

Both products use Sikarugir cx-24.0.7 and matched PE32/PE64/Unix staging.
The perf run has frame sampling ON and PE detailed timers/copy ledger OFF.
The 06 screenshot is the expected forest at 0:38.92; the timeout terminates
the post-scene GUI, with no harness failures or chunk/GPU/pipeline errors in
the retained counter snapshot. The counter snapshot is periodic (2,040
Presents), not the exact final frame row (2,057). A later external CPU sampler
started after rendering ended and is retained as **invalid active-scene CPU
evidence**, not interpreted as zero-cost rendering.

Separate 250-Hz PE sampler runs use module-map and Info logging. They are
invalid for FPS. Final cumulative emissions have zero suspend/context/resume
failures, no target-thread mismatch, zero self-PC overflow and passing module
probe containment. Whole-run module classification is:

| Product | Samples | Last sampled Present | App EXE | PE d3d9.dll | PE bridge |
|---|---:|---:|---:|---:|---:|
| 05 GT2 | 13,707 | 1,800 | 68.4% | 10.7% | 12.1% |
| 06 GT2 | 13,020 | 2,040 | 74.3% | 6.3% | 10.1% |

These are stopped-thread PC shares, not measured exclusive CPU milliseconds;
the bridge share may include time inside a Unix call. The 06 population
includes loading. Subtracting the last pre-scene emission (Present 900,
first heavy frame 951) gives 12,314 samples, with PE 6.60%, bridge 8.91%
and EXE 77.16%. That is an approximate phase boundary, not exact frame join.

The 32 reported self-PC buckets cover only 361/1,465 PE samples in 05 and
196/816 in 06. Missing top buckets are not zero-cost functions. Frozen DLLs,
full hashes, `join_selfpc.py` and symbol JSON retain the reproducible join;
buckets crossing symbol starts are labelled ambiguous. The clearest local
target is `applyConstShadowLiveFixedWidth<16>`: unequivocal buckets contain
114 samples in 05 and 50 in 06. Only its compare-loop bucket (`RVA 0x23040`)
has 55/33 samples; the others cover guards, loop setup and dirty settlement.
This does not support attributing all PE time to the owner or constants.

## Change and verification

R-CORE-REC-7.40's 16-byte equality uses four unsigned word XORs reduced with
OR instead of four short-circuit comparisons. Zero means every bit agrees.
Unaligned-safe loads remain bounded to 16 bytes; the 4-byte path, scalar copy,
zero-extension, dirty membership/range, Recording and transition-table
projection are unchanged. There is no new state, runtime knob, ABI, retention,
queue or concurrency policy. Existing model-derived compile-time projection
and bounded differential tests apply; no new TLA transition/model is claimed.

Native evidence extends every one-bit difference across 128 bits and all 16
source/destination byte offsets, `memcmp` comparison, exact self-alias and
mixed dirty prefixes. Existing generic/reference traces cover all six
constant families, NaN payloads, signed zero, growth, partial writes, A/B/A,
Recording and rollback. Four canonical builds pass; focused native tests
pass 9/9, x86_64 tests 6/6, and decimation-enabled native tests 2/2.

Both linked PE images now compare via two unaligned 128-bit loads,
`pcmpeqd`, a bit-mask extraction and one conditional branch. This is integer
equality, including where the mask extraction mnemonic is `movmskps`.
No floating-point equality or Darwin/ARM API crosses into PE. The portable
source permits target vectorization; it does not make this x86 Wine provider
a native ARM64 provider or prove Rosetta's instruction mapping.

PE32's complete helper has 248 -> 228 static instructions and its individual
stack reservation is 20 -> 12 bytes. PE64's padded symbol has 215 -> 222
instructions (including trailing `int3` padding), with the 56-byte reservation
unchanged. Static code is not executed work. Both DLL byte sizes, exports,
vtable/RTTI owners, build options and the 12 manifest hot-entry metrics match.
The provider and bridge DLLs are byte-identical. The ABI comparison uses the
preserved H253 linked receipt; earlier CLI attempts lacking a baseline
directory/tool PATH are retained as failed invocations, not failed ABI checks.

Candidate PE32 SHA-256:
`eb4631cd151dc824f0106d53aed594f7ea305c7b9396a40c850464532aac3454`.
PE64 SHA-256:
`094f4c423bf4da5688d3ed4ee2c0c1e01d34b6bca2759ef8dd550021dd239ec8`.

## Post-commit wild spot checks

Implementation commit is `6c3ca092`. Each product has one sequential perf run,
with frame sampling ON, PE sampler/detailed timers/copy ledger OFF, and no
GPU trace. Staged hashes match the linked receipts above; the Unix provider
and bridge DLLs match the baseline. Resolution is 1024x768 for 05 and
1280x720 for 06, with Sikarugir cx-24.0.7. A read-only external `ps` observer
samples process CPU/RSS every two seconds; these are not observer-free runs.

| Product | Baseline FPS | Candidate FPS | Delta | README FPS | Candidate rows / wall ms | CB / frame | Pass / frame |
|---|---:|---:|---:|---:|---:|---:|---:|
| 05 GT2 | 29.009 | 29.500 | +1.69% | 30.1 | 1,905 / 64,577.248 | 4.001 | 15.783 |
| 06 GT2 | 17.926 | 17.958 | +0.18% | 18.1 | 1,151 / 64,093.086 | 4.002 | 19.852 |

05 includes every positive-wall frame; 06 additionally requires more than
100 draws, as for both 06 references above. Initial slow scene frames stay
included. GPU command-buffer time from those frame rows is 1.993/11.831ms
per frame. This completion-attributed counter is not an exclusive GPU wall
interval. The catalogue process-elapsed FPS and the 06 all-row/loading FPS
are not used.

Both runs pass with zero chunk rejections, GPU errors, pipeline-build failures
and no-pipeline skips in their retained counter snapshots. 05 exits normally;
06's accepted timeout closes the GUI after scene completion. Captured forest
images at 0:59.45 / 0:30.70 are coherent at those instants, not whole-scene
pixel or flicker proof. 05 emits a captured `.3dr`; 06 does not. Neither is
substituted for the consistently defined frame metrics above.

The 05 baseline had an observed `duetexpertd` CPU spike, whereas the candidate
preflight did not. These single, ordered runs therefore establish neither an
attributed gain nor noise-qualified neutrality. Both values remain below the
README targets; the README is unchanged.

### CPU-consumption observation

`analysis/monitor_cpu.py` binds one discovered benchmark PID and records
Darwin process CPU time, RSS and the latest renderer frame. The retained
`candidate-measurements.json` is reproduced by `summarize_candidate.py`.
Only advancing, draw-heavy endpoint pairs before the final frame are used;
the final-frame pair is excluded because teardown/GUI time could precede
the process sample. Log frame and process time are not an atomic observation.

| Product | Observed frame increments (endpoints) | CPU seconds / elapsed seconds | Process CPU ms / frame | Average CPU cores | Sampled process RSS peak, KiB |
|---|---:|---:|---:|---:|---:|
| 05 GT2 | 1,180 (649–1,829) | 85.17 / 40.171 | 72.178 | 2.120 | 3,477,004 |
| 06 GT2 | 1,136 (986–2,122) | 108.31 / 62.294 | 95.343 | 1.739 | 3,612,152 |

These are partial-scene process totals across application, PE, Unix and all
threads, not PE-exclusive costs or comparable intervals across products.
There is no matched baseline CPU interval; no CPU or RSS reduction is claimed.
The sampled RSS maximum is not an exact lifetime high-water mark.

### Actual PE sampling after the change

Separate candidate runs use the same 250-Hz sampler and Info logging, never
FPS promotion. Both have zero suspend/context/resume failures, zero self-PC
overflow and passing probe containment. The frozen candidate PE32 and
`05-candidate-pe-symbols.json` / `06-candidate-pe-symbols.json` qualify
the symbol joins with the staged image SHA-256.

| Product | Candidate samples / last emitted Present | PE samples / thread share | EXE share | Bridge share | Reported self-PC samples / all PE samples |
|---|---:|---:|---:|---:|---:|
| 05 GT2 | 13,268 / 1,800 | 1,459 / 11.0% | 69.2% | 11.2% | 344 / 1,459 |
| 06 GT2 | 14,140 / 2,100 | 922 / 6.5% | 74.8% | 10.1% | 221 / 922 |

The 06 population includes loading and has a different endpoint from the
baseline's 2,040; it is not a matched scene-only sample. 05 has the same last
emitted Present, but different total populations and an unobserved final tail.
Neither whole-module share nor partial top-32 bucket counts prove CPU savings.

The candidate compare loop moved to RVA `0x22fc0`; it has 56/38 samples for
05/06, versus 55/33 at the baseline compare loop's `0x23040`. Candidate
`0x23000` now covers scalar copy/dirty work and `0x23040` covers post-loop
range settlement. Comparing `0x23040` directly across builds would falsely
report a large compare improvement. Even the symbol-qualified loop buckets
cover different instruction mixtures; only linked codegen, not a runtime
compare-cost win, is demonstrated. The unchanged extent guard is at `0x22e40`.

### Remaining gate

Fewer branches can lose to the old early exit when the first word differs.
Current actual PE sampling must be read against each image's own instruction
layout; moving a hot bucket is not removing its cost. A matched repeated
control, with active-interval CPU measurements, is still required to attribute
gain and close CPU-consumption/FPS recovery. This is not whole-scene pixel
proof or a completed PE optimization.
