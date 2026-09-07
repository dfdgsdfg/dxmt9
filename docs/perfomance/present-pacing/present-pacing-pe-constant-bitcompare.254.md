---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Current PE attribution and branch-reduced constant equality"
type: evidence
status: native-linked-verified-runtime-pending
updated: 2026-09-07
source: experiments/output/pe-current-attribution-20260907/analysis; experiments/output/app-d3d9-3dmark05-pe-current-attribution-249718d0-gt2-sampler-r1-20260907; experiments/output/app-d3d9-3dmark06-pe-current-attribution-249718d0-gt2-perf-r1-20260907; experiments/output/app-d3d9-3dmark06-pe-current-attribution-249718d0-gt2-sampler-r1-20260907
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

Runtime gates remain open: fewer branches can lose to the old early exit
when the first word differs. Fresh actual PE sampling and no-sampler 05/06
GT2 runs must price that tradeoff. This is not README FPS recovery, reduced
process CPU consumption, whole-scene pixel proof or a completed PE optimization.
