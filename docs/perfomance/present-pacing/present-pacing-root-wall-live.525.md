---
domain: present-pacing
workload: STALKER Day high, fixed late-camera diagnostic window
title: "Primary root CPU envelope lies below both dedicated workers in one diagnostic episode"
type: evidence
status: diagnostic-primary-exclusion-default-transfer-and-removal-open
updated: 2026-10-14
source: traces/stkcop-root-wall-game-h525/{live-contract.md,generation.json,inputs.json,analysis-generation.json,wall_bounds.py,parse_map.py,test_bounds.py,bounds-tests.json,analyze_live.py,analysis.json,verify.py,verification.json}; traces/stkcop-root-wall-game-h525/run1/{receipt.json,gap.bin.196,self.bin,post-tail.png,recovered.json,restore/inventory.json}; experiments/output/app-d3d9-stkcop-bench-root-wall-game-h525-run1/result.json
related: present-pacing-root-wall-abi.524.md; present-pacing-root-wall-preflight.522.md; present-pacing-primary-gap-live.521.md; present-pacing-source-self-thin.516.md
---

# A useful diagnostic exclusion, not a default-owner verdict

The H523 root-wall observer and H524 activation receipt complete one live
episode with H516's sparse worker reporter. In the same fixed late-camera
window, the Present caller's inclusive D3D9-root CPU upper envelope is
**19.6901 ms per observed completed frame**, below both Replay and Encode
self-CPU lower bounds. All five component intervals also satisfy this ordering.

This result is conditional on H522/H523's finite current-host clock
qualification and the retained endpoint allowance. It is **diagnostic**, not
yet default-transferred. It does not establish a global/default largest owner,
rank Replay above Encode, identify the FPS critical path, or price removable
work. Do not replace those missing gates with this successful exclusion.

## Same-window observations

Sikarugir CX24.0.7; Day/high 1280×720; max-threads 2; snd_targets 16; draw command
limit 0; production-counter occlusion. Product perf/frame/supply observers OFF,
isolated H523 PE and H516 worker diagnostics ON. Both reports arm at spatial
knot 0 with attachment epoch 2. All seven frozen knots remain active/qualified.
Knots 1–6 encompass 105 observed completed frames.

| Scope | CPU ms / observed completed frame |
|---|---:|
| Present-calling Primary, whole mixed thread | 55.5806–56.6805 |
| Inclusive Primary D3D9 roots, conservative upper envelope | ≤19.6901 |
| Replay, whole dedicated thread | 31.4634–31.8374 |
| Encode, whole dedicated thread | 31.7663–32.0000 |

Whole-thread CPU and root envelope are different quantities. Root wall
includes synchronous descendants, application callbacks, waits and inside-
bracket diagnostic work. It is not exclusive PE execution. Whole Primary also
includes game/Wine work and outside-bracket instrumentation. Neither scope
is directly removable. Concurrent worker times must not be added into latency.

The envelope uses the last whole-root checkpoint strictly before the initial
scene window and the first checkpoint strictly after the final window. It
includes **1,671,213 complete outer roots**, measured elapsed sum
1,733,217,171 ns and allowance 334,242,600 ns (200 ns per root), totaling
2,067,459,771 ns. No interpolation, wait subtraction or synthetic hook-cost
subtraction is performed. Boundary roots are included in full.

The five component upper envelopes are 19.9153, 20.6251, 21.3577, 20.4660 and
19.8290 ms/frame. Each is below both corresponding worker lower bounds.
Their enclosing roots overlap at boundaries; do not sum the component budgets.
Replay/Encode CPU bounds overlap in the full window and all components, so
their fine ordering remains unresolved rather than an optimization target.

## Integrity and limitations

The map contains 245 complete Present checkpoints. Activation freezes the same
native/guest Present caller and two attachment slots; the retired epoch remains
empty. Worker reporting has 468 sampled Replay pairs and 1,555 Encode pairs.
Compatibility Encode metadata still has no raw/source join. Sampled scope sums
are not whole-thread totals and are never multiplied by the sampling stride.

Sixteen receipt/clipping tests pass before live use. After the run, `verify.py`
independently decodes original tuples, recomputes six enclosing root sums and
18 thread bounds, verifies both-worker comparisons, and checks image/artifact
hashes and restoration. H523's 44 parser negatives and H524's native/actual-
object activation evidence remain prerequisites, not substituted live proof.

The game exits naturally with code 0. Catalogue failure is solely
`missing_capture`, administratively qualified by the frozen diagnostic
contract, not relabeled as a catalogue pass. Computer-use restores the game
window before measurement and takes one post-window screenshot; its rendered
building scene is visual sanity only, not exact-pixel or whole-run proof.

All 14 pre-existing shared files are hash-restored; four temporary service files
are recovered. No owned game/helper remains live. Canonical provider/source
hashes remain unchanged. No product default, README FPS, commit or push changes.

## Next decision

The root-wall route is now useful enough to justify a bounded **default-
interference/transfer comparison**, not more clock microbenchmarks. Freeze
scene, renderer artifacts and policies, expose A/A variation, and retain a
comparison whose control does not carry this all-root instrumentation. The
sparse worker observer is itself diagnostic and also needs qualification.
Historical H521/H516 episodes are not substitutes for that matched control.

If transfer holds, treat Replay and Encode as overlapping leading dedicated
costs and test a distinct correctness-preserving removal mechanism against
original late input plus live transfer. Do not revive H508/H509's closed
validation-reuse campaign or imply a 32 ms removal budget from a 32 ms thread
total. Global ownership and measured removable CPU remain the active goal.
