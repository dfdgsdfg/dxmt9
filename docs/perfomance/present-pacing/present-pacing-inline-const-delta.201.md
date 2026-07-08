---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: pe-recorder
order: 201
title: Inline Const Delta Proves Mechanism But Lands Inside The Noise Band
date: 2026-07-09
type: no-gputrace
status: accepted-mechanism-confirm; fps-below-gate; kept-opt-in
source: experiments/output/app-d3d9-3dmark05-icd-control-cool-r4-20260709/result.json; experiments/output/app-d3d9-3dmark05-icd-candidate-cool-r4-20260709/result.json; experiments/output/app-d3d9-3dmark05-icd-control-r3-20260709/result.json; experiments/output/app-d3d9-3dmark05-icd-candidate-r3-20260709/result.json; docs/perfomance/present-pacing/present-pacing-decimated-pe-stats.200.md
related: docs/perfomance/present-pacing/index.md; docs/perfomance/present-pacing/present-pacing-pe-const-overhead-cut.193.md
---

# Present-Pacing H214 - Inline const delta (R-BACK-2.52) runtime judgment

## Implementation

`DXMT9_PE_INLINE_CONST_DELTA` landed as spec'd (T1 `ea515e89` wire sections,
T2 `d6740bd0` PE fold with per-shadow standalone-flush fallback, T3 `8538c9ea`
importer validation/apply + equivalence spec, T4 `1ce884ef` ABI-hash bump).
T3's spec-driven review caught a real correctness bug before runtime: the run
coalescer did not inspect const sections, so folded constant writes would have
been silently dropped by coalesced runs (fixed with a no-delta check plus an
unconditional run-compat rejection, since sections carry no content hash).

## Mechanism confirm (decimated stats, N=64)

Exactly as designed: append events `1,377 -> 743/present` (the `~634`
standalone const records eliminated), `flushConstShadow` events
`4,456 -> 56` (non-draw consumers only), setter shadowing unchanged, recorder
core `-2.8ms/present` in the measured pair. Off path verified: control on the
new code returns to the healthy population (`2,260` presents, encode/replay
per-present identical to pre-change), so the schema/validation tax is nil.

## FPS verdict

Cooled, symmetric pair: control `2,260` -> candidate **`2,297` = `+1.6%`** —
inside the `±5%` noise band, below the promotion gate. `gpu_err=0`, visuals
in class. This is the H193 pattern at 3x the scale: removing a *measured*
`~2.4-2.8ms/present` of producer recorder-core CPU bought only `~1/3` of the
arithmetically expected FPS. The working model must be revised: PE-side
CPU-ms under Rosetta over-credits wall-clock value (translation/memory-stall
overlap), so remaining GT1 producer levers should be assumed to under-deliver
by a similar factor unless a paired scout proves otherwise. Kept opt-in,
default off; promotion would need `2-3` more cooled pairs resolving a real
`+2-4%`, which is judged not worth the spend now.

## Method lessons (both cost a full pair each)

1. **Environmental contamination**: after hours of back-to-back runs the whole
   system ran `15-75%` slower (all unix scopes and PE per-event costs inflated
   uniformly; no pmset thermal warning recorded) — an r3 pair read `1,680/1,620`
   and was invalid. A 4-minute cool-down restored `2,260`. Insert cool-downs
   between FPS-evidence runs and sanity-check the control against the
   population before reading a pair.
2. **Startup-flake root cause found**: the shader-archive full prewarm had
   grown to `125MB` (loaded twice per launch), and both icd r1 runs failed
   deterministically with the known one-draw voluntary-exit signature;
   rotating the archive away cleared it. The archive is saved on clean device
   destruction (voluntary-exit runs save it; timeout-killed runs do not),
   which is how it bloats across long probe campaigns while probes stay cold.
   Follow-up candidates: prewarm size cap or async prewarm, and a probe-side
   archive-size guard.
