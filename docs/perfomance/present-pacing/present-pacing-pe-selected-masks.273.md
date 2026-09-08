---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Compact selected-mask reuse: reject added spills"
type: evidence
status: rejected
updated: 2026-09-08
source: experiments/output/pe-sparse-admission-economy-20260908/analysis; experiments/output/app-d3d9-3dmark{05,06}-pe-sparse-admission-9263d1dd-baseline-gt2-seg0-inline0-threadcpu-r1-20260908
related: present-pacing-pe-constant-loop.267.md; present-pacing-pe-held-constant-value.269.md; present-pacing-segmented-preflight.272.md
---

# Selected-mask reuse: original runtime retained

## Decision

At `9263d1dd`, `buildSparseStatePlan` in
`src/d3d9/d3d9_pe_producer.cpp` reads PendingDelta texture/stream masks into
the compact source, then rereads them after `prepareConstantRanges` to derive
the full-snapshot flag. Linked PE32/PE64 bodies retain this duplicate. Constant
preparation cannot mutate PendingDelta or call an invalidator, so reuse is a
valid local refinement, provided failure publication order is preserved.

Three tested representations remove the shadow reread but add register spills
or address materialization. None is retained. Production source is restored
exactly; no runtime default, ABI, wire, lifetime or README value changes.
The retained work is stronger native selection/failure coverage, the
R-CORE-REC-7.61 acceptance boundary, and fresh baseline CPU evidence.

## Linked evidence

| Representation | PE32 bytes / instructions / stack | PE64 bytes / instructions / stack |
|---|---:|---:|
| Original | 1,787 / 423 / 12 | 1,798 / 391 / 56 |
| Early local masks and predicate | 1,819 / 426 / 20 | 1,766 / 387 / 56 |
| Locals at original mask-selection point | 1,798 / 427 / 16 | 1,815 / 395 / 72 |
| Existing source fields after constant preparation | 1,795 / 426 / 16 | 1,765 / 388 / 72 |

Stack means prologue subtraction, excluding saved registers and return
addresses. Whole-symbol counts include cold paths, not dynamic cost per draw.
All bodies retain six direct calls. The last form saves field addresses on
the early path and reloads them from the stack after the constant helper.
Its smaller PE64 body therefore does not establish economy; PE32 gains stack
traffic and instructions. Independent linked review agrees with rejection.
No candidate reaches Wine, and no candidate CPU/FPS result is claimed.

Frozen DLLs and complete selected-symbol reports are in the analysis root:
`{baseline,early,local,candidate,restored}-selected-masks-linked.json`,
`{early-masks,local-masks,selected-fields}-pe{32,64}.dll`, and
`rejected-selected-fields.patch`. Restored PE `.text` sections are byte-identical
to baseline; full DLL hashes differ after relinking and are not called equal.
The Unix provider remains byte-identical. Public-entry metrics, export pairs
and declared vtable/RTTI ownership pass separately; those public metrics alone
would have missed the outlined producer's unfavorable codegen tradeoff.

Two other audit hypotheses are not optimization candidates here:

- `sectionRule(kind)` already lowers to a bounded dense-table index in both
  PE widths. Its source-level linear loop is not executed as a search.
- Generic direct-pin witness lookup remains real, but H248's low population
  does not justify adding ten saved ordinals. Record uniqueness, current-source
  identity and mutable physical-pin validity remain distinct checks.

## Native boundary

`sparsePlanMaskSnapshotFlagParity` adds 108 real producer cases: three record
kinds, texture and stream selections each in `{0, one, all}`, forced-full
OFF/ON and inline constants OFF/ON. One plan is reused across the matrix;
an independent expected selection/flag formula is checked each time.
The failed inline-range fixture now selects both full masks before failing,
asserts inaccessible source and zero flags, and retries with a clean source.
Retry bytes/counts still match fresh compact and generic builders.

The last candidate and restored source both pass native 4/4 and x86_64 5/5
focused producer/views/owner/validation tests, plus the provider install-name
audit in the latter set. All four canonical builds pass. This is bounded local
refinement evidence, not a new concurrency proof. No TLA model or protocol
changes, no new TLC run, and no candidate Metal-pixel claim are made.
Test receipts are `native-{candidate,restored}.log` and
`unix-{candidate,restored}.log` under the analysis root.

## Fresh default-path baselines

Both runs use the five frozen `9263d1dd` runtime images and canonical catalogue
runner with `--skip-stage --stage-mingw-runtime`. They precede candidate builds;
no native test, build or second Wine benchmark overlaps either scene.
Sikarugir cx-24.0.7; 05 1024x768, 06 1280x720; named GT2; perf with explicit
Info logging and frame sampling. Replay offload is ON; segmented transport,
inline constants, CPU-ready Tape, index reorder and PE hot probes are OFF.
The earlier desktop-use-contaminated 06 sample remains excluded.

| Product | Scene frames / wall ms | Sampled FPS | Inclusive native thread CPU ms/frame | Same-iteration process CPU ms/frame |
|---|---:|---:|---:|---:|
| 05 GT2 | 1,910 / 64,637.494 | 29.549 | 31.284 | 70.303 |
| 06 GT2 | 1,158 / 64,085.226 | 18.070 | 52.825 | 96.054 |

`cpu-baselines-qualified.json` independently recomputes raw frames, CPU deltas,
image/selector identity and health coverage. CPU envelopes cover 1,848/1,120
frame increments, about 96.8% of each scene span, with 31 paired observer
iterations and zero endpoint-frame offsets. Native thread occupancy is
0.942/0.966 cores. This includes game/Wine/synchronous Unix, not PE-exclusive
CPU. Endpoints are approximate stable log observations, not exact Present times.

All eight reported renderer error counters are zero. 05 counters cover the
logged scene; 06's final counter is 2,160 versus logged frame 2,204, leaving
44 unreported tail frames. Its timeout is the catalogue-allowed completed
result GUI, following a complete approximately 64-second heavy scene.
These single Info diagnostics neither update README nor establish recovery,
regression, thermal neutrality, whole-scene pixels or an exhausted PE ceiling.

The next evidence target is eliminating whole record/flush work, using the
existing bounded inline-constant consumer coverage. Repeated same-image
CPU-economy and broader consumer/failure gates remain open; another local
mask or pin cache is not justified by the present samples.
