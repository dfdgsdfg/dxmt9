---
domain: present-pacing
workload: STALKER Day high, fixed late-camera interval, combined diagnostic images
title: "Same-window app and worker CPU; post-Present gap is too small for attribution"
type: evidence
status: diagnostic-subset-closed-no-default-owner-or-removal
updated: 2026-10-14
source: traces/stkcop-primary-gap-game-h521/{live-contract.md,inputs.json,analysis-generation.json,analyze_live.original.py,analysis-correction.md,analyze_live.py,gaps.py,analysis.json,verify_analysis.py,verification.json}; traces/stkcop-primary-gap-game-h521/run1/{receipt.json,gap.bin.196,self.bin,post-tail.png,recovered.json,restore/inventory.json}; experiments/output/app-d3d9-stkcop-bench-primary-gap-game-h521-run1/result.json
related: present-pacing-primary-activation.520.md; present-pacing-source-self-thin.516.md; present-pacing-primary-gap-abi.518.md
---

# Close the small subset; do not infer renderer ownership

H521 completes one live diagnostic episode with valid, same-window cumulative
self-CPU bounds for the Present caller, Replay and Encode. The post-Present
non-root subset is only **0.2785–0.2865 ms per observed completed frame**.
It cannot explain the mixed app-thread total or exclude that thread as the
largest dxmt9 CPU owner. Stop repeating this subset experiment.

## Frozen lane and result

Sikarugir CX24.0.7, Day/high, 1280×720, `-max-threads 2`, `snd_targets 16`,
draw command limit 0 and production-counter occlusion. Product perf/frame/supply
observers are OFF; H518 PE and H516 sparse-worker diagnostic images are ON.
This is neither unrestricted game defaults nor an FPS A/B comparison.

H520 activation adopts epoch 2 at camera knot 0. All seven frozen H502 knots
are observed; knots 1–6 encompass 107 observed completed frames. The native
Present caller is named `X-RAY Primary thread`, distinct from both workers.

| Scope in the same late window | Self CPU ms / observed completed frame |
|---|---:|
| Present-calling app thread, all work | 56.7509–57.8540 |
| Replay thread, all work | 31.9956–32.4387 |
| Encode thread, all work | 32.2090–32.5025 |
| Fully enclosed post-Present gaps, subtotal only | 0.2785–0.2865 |

These are concurrent thread CPU totals, not additive frame latency or critical
path percentages. Replay and Encode bounds overlap in the full window. App
thread CPU includes game, Wine, PE and synchronous Unix work. Subtracting the
gap subtotal does **not** produce exclusive renderer CPU: it covers only the
time after successful outermost Present and before the next outer public D3D9
entry, and includes diagnostic hook/bridge work.

The terminal maps contain 250 completed gaps / 500 clock queries, no pending
gap, and two epoch slots with the retired one empty. Worker reporting contains
477 sampled Replay pairs and 1,544 Encode pairs. No raw/source join exists for
these compatibility Encode records. Do not multiply Replay scope sums by 256.

## Validation and recovery

The game exits naturally with code 0. Catalogue status remains failure solely
for `missing_capture`, administratively qualified by the frozen diagnostic
contract, **not** a catalogue pass. A post-window screenshot shows a rendered
building scene without an obvious full-screen failure; it is not an original
image exact-pixel oracle or whole-run correctness proof.

The parser validates every row against the activation receipt and enforces
clock/order/conservation checks. Independent verification rejects 21 stale
identity, reload, loss and clock mutations and recomputes six intervals / 18
thread bounds directly from raw tuples. The first analyzer invocation had a
redundant `identity` dictionary lookup; its original script and generation hash
are retained. Only that lookup is corrected; no gate or measurement changes.

All 14 existing shared files are hash-restored; four temporary service files
are recovered. Canonical source/provider hashes match the pre-run state, and
no game/helper process remains. No product code/default, README FPS, commit or
push changes in H520–H521.

## Remaining decision

This closes a measurement feasibility question, not the performance goal.
Default transfer, exclusive mixed-thread ownership and correctness-qualified
removable CPU remain open. More post-Present samples or denser worker clocks
will not answer those questions. Any next attribution mechanism must cover
the **interleaved app-thread work during rendering**, preserve callback
ancestry, and qualify its own perturbation before ranking default owners.
Do not revive H477's all-call clock observer, H512's narrow external-counter
bounds, or H509's closed validation-reuse campaign as substitutes.
