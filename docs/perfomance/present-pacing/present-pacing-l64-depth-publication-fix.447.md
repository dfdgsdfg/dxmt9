---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "L64 depth/stencil live-out preservation across publication cuts"
type: experiment
status: reproduced-corruption-repaired
updated: 2026-10-07
source: traces/stkcop-l64-h447/{summary.json,native-before.txt,focused-final-tests.log,x86-final-tests.log,tla.log,runtime.diff}; traces/stkcop-l64-h447/{a,b,b2}/{completion.json,loading.png,scene.png}; experiments/output/app-d3d9-stkcop-bench-l64-h447-{a,b,b2}/result.json; tests/native/backend/publication_depth_store_metal_spec.mm; tests/native/backend/render_pass_actions_spec.cpp
related: docs/perfomance/present-pacing/present-pacing-l64-observer-off-repro.446.md; specs/backend/render-pass-actions/requirements.md
---

# L64 depth/stencil publication fix

## Cause and narrow repair

`depthStoreProofForLookahead` treated exhaustion of a non-deferred chunk
without Present as `AllowDeadNoPresent`. A DrawLimit publication can end that
chunk between a depth/stencil writer and its later consumer. The encoder then
discarded contents which the next chunk loaded. Chunk-local absence is not
attachment death, including when the EncodeSession has finalized.

The repair returns `BlockNoLookahead` / `SuffixExhausted` at that boundary.
An actual full next-clear still permits DontCare; subsequent reads, partial
or other-aspect clears, and Present remain conservative. The former
`DXMT9_AGGRESSIVE_DEPTH_DONTCARE` override is retired. L64 publication, queue
policy, resource marking, PE and Tape exact mode are unchanged. No blanket
exact-attachment mode is enabled in the wild runs.

The earlier marking asymmetry is not changed: `skipDrawResourceMarking_`
defaults false and its setter currently has no production caller beyond the
unused device forwarding facade. That source audit does not prove the dormant
skip mode safe; it separates it from the reproduced ordinary path.

## Deterministic evidence

- `testPublicationCutPreservesUnobservedDepthUse` enumerates 6,250 four-op
  trace/prefix/aspect cases over two surfaces. The production predicate failed
  before the repair (`native-before.txt`) and passes afterward. A negative
  control explicitly counts the retired rule's hidden-future-read cases.
- The Metal oracle calls production `prepareRenderPassWithStoreProofLookahead`
  with normal actions, query preservation OFF and Tape exact OFF. Serial and
  separate-command-buffer depth/stencil consumers have identical BGRA8
  readback, also checked against an independently specified coverage image.
  Both submitted buffers complete without errors; no CPU wait is inserted
  between the split writer and reader. Metal API validation is enabled.
- Eight focused native/Metal tests pass, including the retired override set
  to `1`, lifecycle, resource lifetime, query-preserved observer OFF/ON and
  WMT Store setters. x86_64 action/GPU tests and install-name audit pass.
- Existing TLA regression: 66 safety, six progress and 158 expected-failure
  configurations pass. The new cut/action refinement is the exhaustive native
  test, not a claim that these older TLA models prove attachment pixels.

The reduced oracle isolates production action selection and real attachment
contents. It does not exercise the entire queue scheduler, resource retirement,
MSAA, every shader or a complete STALKER frame.

## Wild verification

Same Day/high 1280x720, `-max-threads 2`, `snd_targets=16`, observer-OFF setup as
H446. Catalogue supervision, staged-image receipts and early-loading foreground
screenshots are retained. Runtime change is only the depth proof repair.

All three runs completed normally without timeout. The first repaired L64
capture shows the previously missing courtyard ground and buildings; the
second shows intact street geometry, building and characters. The limit-0
control shows normal street and later building views. All five staged binary
receipts and all requested/final/control config bytes match across the three
runs; only the Unix provider differs from H446. `analyze.py` asserts these
identities and writes `summary.json`.

These are qualitative different-camera checks, not pixel-exact full-frame
equality or continuous video coverage. Later capture attempts for both L64
runs occurred after normal process exit and are retained as failed UI receipts,
not additional visual evidence. The reduced GPU oracle provides exact
depth/stencil coverage independently of this timing limitation.

No FPS benefit is claimed: correctness runs overlap compilation/formal tests,
and H444–H446's corrupted-image FPS values are not equivalent-rendering
performance evidence. Default limit remains zero. New performance/policy
qualification must use this repaired image, not H443's older image receipts.

## Toolchain provenance

The host build needed an explicit current Xcode SDK root after the toolchain
update; otherwise `<array>` was absent from the default search path. Its cached
archive flags also require LLVM `ar` rather than Apple `ar` (`csrD`). A temporary
PATH shim to the installed LLVM archiver fixed the local invocation, without
editing build outputs or repository toolchain configuration. Canonical Meson
provider builds retain the install-name fixup. Raw evidence is preserved.

`git diff --check` and provenance audit of H444–H447 pass. The repository-wide
provenance audit still finds missing historical build logs in three unrelated
accepted-verdict leaves; this change neither fabricates those artifacts nor
rewrites their history.
