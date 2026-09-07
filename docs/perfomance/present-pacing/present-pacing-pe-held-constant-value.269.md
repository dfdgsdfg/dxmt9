---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Held constant input: smaller code is not a cheaper equal path"
type: evidence
status: rejected
updated: 2026-09-08
source: experiments/output/pe-constant-held-value-20260908/analysis; experiments/output/pe-gt2-clean-rerun-20260908/analysis; experiments/output/app-d3d9-3dmark05-pe-constant-held-value-33e8e174-gt2-off-perf-{r1,r2}-20260908; experiments/output/app-d3d9-3dmark06-pe-constant-held-value-33e8e174-gt2-off-perf-r1-20260908; experiments/output/app-d3d9-3dmark05-pe-constant-held-value-33e8e174-gt2-off-threadcpu-r1-20260908
related: present-pacing-pe-constant-loop.267.md; present-pacing-pe-native-thread-cpu.268.md; present-pacing-inline-constant-economy.266.md
---

# Held constant input: rejected, original runtime retained

## Decision

The changed 16-byte constant reload identified in
[H267](present-pacing-pe-constant-loop.267.md) is real, but the tested fusion
is not retained. PE32 trades that changed-only work for a source-argument
reload on **every** element. Two 05 spots and one 06 spot show no economy
benefit. Two follow-up source shapes do not remove the tradeoff. Production
`d3d9_pe_const_shadow.hpp` is restored exactly; no renderer default, ABI,
wire layout, retained ownership or README FPS value changes.

The retained changes strengthen native constant-boundary tests and the
R-CORE-REC-7.40 acceptance rule: inspect equal and changed paths separately,
not only whole-symbol byte counts. This is a negative experiment, not a
claimed CPU/FPS improvement or proof that PE optimization is exhausted.

## Linked mechanism and rejected alternatives

The first candidate captures exactly 4/16 input bytes into call-local unsigned
words, compares those bits and stores the held value only when changed.
PE32/PE64 keep the 16-byte value in XMM registers: the changed source reload
disappears without a per-element call. PE remains x86/x64 under Wine/Rosetta;
this does not introduce native ARM64 PE code or Darwin calls across the boundary.
Raw NaN/signed-zero bits, equal no-store,
exact self-alias, growth order and dirty effects pass bounded checks.

| Variant | PE32 16-byte body bytes / instructions | PE32 4-byte body bytes / instructions | PE64 16-byte body bytes / instructions |
|---|---:|---:|---:|
| Original H267 | 594 / 208 | 504 / 186 | 561 / 176 |
| Held value, relative-index loop | 556 / 198 | 504 / 186 | 554 / 174 |
| Absolute-register cursor loop | 527 / 187 | 758 / 257 | 510 / 160 |
| Held value, dirty mark moved last | 556 / 198 | 504 / 186 | 554 / 174 |

Those totals include cold growth. The original PE32 loop keeps its source
pointer in `%esi`; only changed elements reload a spilled dirty pointer and
the source value. The first candidate instead executes
`movl 0x24(%esp), %esi` inside the common loop before the source vector load.
It also removes changed-path address calculations. Which trade wins depends
on changed/equal frequencies, which these measurements do not independently
count. The extra common-path load is therefore a mechanism to investigate,
not a proved cause of the small FPS differences.

The cursor variant still reloads the source argument and reads its loop bound
from the stack. It expands the 4-byte kernel to 758/689 bytes in PE32/PE64;
PE32 prologue stack subtraction grows from 8 to 24 bytes. Moving the dirty
mark does not remove the source reload. Neither follow-up reaches Wine.
No hand-written register constraint, persistent cache or new hot-path probe
is introduced to force a favorable result.

`linked-constants.json` preserves all frozen image comparisons. `final-pe*.dll`
is the first, actually measured candidate, **not** the retained runtime.
`cursor-pe*.dll` and `reordered-pe*.dll` are rejected static alternatives.
`public-linked.json` pins unchanged twelve public hot-body metrics per width,
exports and vtable/RTTI owners for the measured candidate. That public-entry
audit does not price the outlined constant loops; their separate audit is
essential. The restored PE32/PE64 `.text` sections are byte-identical to the
original images, independently checked in `experiment-qualified.json`.

## Actual GT2 spots

Sikarugir `sikarugir-cx-24.0.7`; 05 1024x768, 06 1280x720. Warn-level `perf`,
frame sampling and two-second process CPU observation; worker replay ON,
inline constants/segmented transport/CPU-ready Tape/index reordering OFF.
No overlapping build or test. All five staged runtime images are checked;
the controls use frozen H267 images and candidates use the frozen held-index
images. Unix and both bridge DLLs are unchanged.

| Product / image | Frames | Scene wall ms | Sampled FPS | Partial process CPU ms/frame |
|---|---:|---:|---:|---:|
| 05 control | 1,930 | 64,591.458 | 29.880 | 69.447 |
| 05 held-index r1 | 1,921 | 64,650.450 | 29.714 | 69.892 |
| 05 held-index r2 | 1,911 | 64,613.359 | 29.576 | 69.946 |
| 06 control | 1,175 | 64,121.415 | 18.325 | 94.934 |
| 06 held-index r1 | 1,167 | 64,086.814 | 18.210 | 95.288 |

The 05 execution order was candidate r1, an intervening candidate CPU
diagnostic, control, candidate r2; the 06 control preceded the candidates.
This is not randomized ABBA or an attributed regression proof. It is enough
to decline promotion of a candidate with no demonstrated economy and an
unfavorable common-path codegen trade. No average mixes excluded runs.

Frame filtering retains all positive-wall 05 samples and positive-wall 06
samples with more than 100 draws, including slow frames. The 06 control covers
frames 1028–2202, candidate 1031–2197. Both have their final error-counter
snapshot at present 2160, so their tails are not error-counter-covered.
All eight reported rejection/pipeline/GPU-error counters are zero. CB/frame
stays about 4.002; passes/frame stay about 15.79/19.87. Configured 06 timeout
is accepted after the completed scene and static result GUI, not as evidence
of scene progress by itself.

The earlier desktop-use-contaminated 06 run remains excluded. The first new
clean-rerun attempt is also excluded: the custom Unix staging directory was
one level too deep, causing installer fallback to the native `build/src`
provider and a `0xc0000003` attach failure. Corrected explicit staging restores
all five expected hashes. Neither attempt contributes FPS or CPU evidence.

One separate Info diagnostic of the held-index 05 candidate qualifies 1,280
of 1,926 frames (66.5%), with 30.428 ms inclusive commit-thread CPU/frame and
0.954 occupied cores. The retained interval population differs from H268;
comparing 30.428 directly with H268's 33.826 would invent a CPU reduction.
This includes game/Wine/synchronous Unix, not PE-exclusive time. Further 06
CPU and stopped-PC candidate runs were not executed after rejecting the
candidate. No new PE-symbol distribution is claimed.

## Retained verification and next gate

The native test composes the existing production equality/copy primitives for
direct byte checks; independent Live/factory differentials separately test
the actual reducer. Both 4/16-byte widths cover all 16-by-16 byte alignments,
four raw patterns, every differing source bit, return values, immutable source
bytes, destination canaries and exact self-alias: 165,888 matrix rows plus
self-alias checks. The existing 1,568-row dirty/change-mask differential stays.

Each Unix guard-page fixture reserves three contiguous `PROT_NONE` pages,
enables only the middle one and tests both edges. Equal destinations and
exact self-alias run read-only; changed source remains read-only. A first
revision lacked guaranteed protected neighbors; a subsequent revision forgot
to restore write access before the second edge. Both test-fixture defects are
corrected, with failure logs preserved. Guard pages cover native hosts, not
PE COM pointer-fault behavior or unsupported partial overlap.

The held-index candidate passed native 4/4, x86_64 5/5 and real PE32/PE64
six-family VB/indexed/UP pixel fixtures: 144 exact sampled pixels, expected
append receipts and device Release zero. Correctness did not establish its
economy. Restored native 4/4 and x86_64 5/5 pass again. This is local value
refinement; no concurrency/lifetime transition changes or new TLA proof is
claimed.

The restored PE32/PE64 images also pass the same 144 sampled pixels, exact
append receipts and zero device Release result. All five installed Wine-root
runtime images match the restored canonical build outputs by full SHA-256;
the rejected candidate is no longer staged.

A fresh bounded retention audit also found no justified local deletion:
record-local identity witness, current source equality and mutable physical
pin validity are distinct authorities. Keep them. The next stronger economy
candidate is reducing record/flush work through the already verified portions
of inline constant consumption, subject to its remaining boundary/pixel and
repeatable CPU-economy gates in [H266](present-pacing-inline-constant-economy.266.md).
Do not reopen arbitrary pin-check removal from PC counts alone.
