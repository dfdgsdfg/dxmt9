---
domain: present-pacing
workload: STALKER CoP H481 original Normal input, native base vertex and VS immediate shadow
title: "Removing 3359 vertex immediate pushes does not clear the predeclared CPU saving gate"
type: evidence
status: mechanism-proven-local-saving-gate-failed-runtime-restored
updated: 2026-10-11
source: traces/stkcop-vertex-shadow-h499/{contract.md,candidate.patch,vertex_immediate_shadow.hpp,vertex-immediate-reuse.md,build.log,tests.log,tla.log,build_runner.py,candidate-build.json,run.py,diagnostic-C.txt,diagnostic-C.stderr,compare.py,mechanism.json,matrix-images.json,rows.jsonl,summary.json,matrix.log,build-restored.log,tests-restored.log,verify.py,restoration.json}
related: present-pacing-volatile-multiplicity.498.md; present-pacing-command-raster-reuse.497.md; present-pacing-root-game.477.md
---

# Work removal is real; the required net CPU saving is not established

H499 implements H498's native-base-vertex plus exact-byte VS immediate shadow
under the existing native opt-in, without a new runtime selector or default
promotion. It removes 3,359 of 3,497 actual pushes while preserving original
pixels. The incremental effect is positive in all eight pairs, but its median
does not clear the predeclared A/A envelope; the combined effect against the
default also fails. The runtime, candidate tests and proposed contract are
restored. Patch, header, contract, frozen images and every result are retained.

## Refinement and actual-call evidence

The physical encoder's binding owner carries an exact 80-byte witness. Only
admitted native indexed draws may reuse it. Negative-base/expanded fallback
and non-indexed pushes invalidate it; a fresh encoder resets it. Matching
skips only the push, never the draw. The actual-push observer/counter is emitted
only for a real push. No borrowed pointer or GPU lifetime is added.

The exhaustive checker invokes the production shadow over 3,110,400 transitions:
all six-action length-five histories at every one of 80 mutated byte positions,
including separate owners, reset and intervening non-admitted writes. Recorder
tests invoke `encodeDraw`, checking base 5 -> 6, negative fallback, non-indexed
interposition, encoder reset, instance-divisor change and missing owner.
Focused tests pass 3/3, including the existing full draw-recorder suite and
install-name audit. The full x86 build passes; existing TLA regression passes
in 209.37 seconds. TLA is regression evidence, not a model of the new byte
shadow; the bounded native checker is its model/code binding.

The binding diagnostic authenticates frozen images and the original input.
Its 3,497 actual issued draws remain unchanged. There are 138 actual VS pushes,
down from 3,497. Carrying bytes only within the same physical encoder matches
the native-only reference at every draw, including the first binding. All
3,496 indexed geometry records are equal. Source/output readbacks exactly
match the original SHA-256; no tolerance, replacement oracle or alpha masking.
This confirms the mechanism, not performance or complete GPU-read authority.

## Finite three-arm CPU comparison

Arms: A is frozen H495 default; B is the same images with existing native base
vertex enabled; C is frozen candidate with native base vertex and shadow.
The H486 executable recipe and original H481 input/source cuts are unchanged.
Each run uses a fresh device and cold PSO cache. The primary metric is native
Encode thread CPU in source index 1's command body, excluding cold prefetch.
Supply-trace boundaries remain enabled; per-draw binding/geometry diagnostics
and profiler are off. Neither this input nor its timings represent the whole
default-game tail automatically.

The predeclared matrix has six OFF image/oracle checks, two warmups per arm,
four A/A pairs and eight counterbalanced triples. No build or TLA execution
overlaps the matrix. All 44 executions pass the original source/output oracle;
including the separate mechanism run gives 45. All 38 coarse-trace runs retain
92 raws, 208 accepted validators and 84,587 command-header visits. No unfavorable
pair is removed and no extra favorable run extends the matrix.

| Paired command-body comparison | Median saving | Positive pairs | Required A/A envelope | Gate |
|---|---:|---:|---:|---|
| Default -> native only | -0.0805 ms | 4/8 | 0.523 ms | Fail |
| Native only -> native + shadow | 0.370 ms | 8/8 | 0.523 ms | Fail |
| Default -> native + shadow | 0.301 ms | 5/8 | 0.523 ms | Fail |

The gate requires both median saving above maximum absolute A/A difference
and at least seven positive pairs. Arm medians are 18.408 / 18.202 / 17.8085 ms;
these unpaired medians must not replace paired differences. The unchanged
planned-replay control has combined paired saving -0.0595 ms against 3.436 ms
A/A noise. Concurrent thread scopes are not added.

Eight positive incremental pairs are a promising small directional signal;
failure of this conservative gate does **not** prove zero benefit or a general
regression. It does rule out claiming the predeclared robust local benefit,
default FPS improvement, or the largest default-tail removable cost from this
experiment. The observed delta is not a statistical upper bound.

## Decision and restoration

The predeclared new-cache retention gate fails, so the candidate is not kept
behind another switch or promoted. Source paths have no runtime/test diff
against HEAD. Both rebuilt x86 native/unix image hashes match H495 byte for
byte; restored focused tests pass. ARM build and Wine installation are untouched.
Only investigation documentation remains changed.

This narrows the investigation: a driver call-stack observation plus thousands
of avoidable calls did not establish a large removable CPU owner. Do not repeat
this experiment with favorable-run selection, nor generalize the result to all
Metal driver work. The next ownership decision must cover a larger active-body
scope or strengthen default-tail attribution, not assume another isolated
binding cache explains the gap. Mixed Primary game/Wine/PE/synchronous Unix
ownership and capture-to-tail transfer remain open; the global goal is not met.
