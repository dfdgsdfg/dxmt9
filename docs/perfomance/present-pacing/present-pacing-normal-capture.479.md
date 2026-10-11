---
domain: present-pacing
workload: STALKER CoP Day Normal-action capture diagnostic
title: "Normal capture publishes an authenticated late courtyard interval"
type: evidence
status: capture-authenticated-normal-replay-unqualified
updated: 2026-10-09
source: traces/stkcop-normal-capture-h479/{run.py,executed-r1.py,analyze.py,summary.json,receipt.json,restore/restored.json}; traces/stkcop-normal-capture-h479/r2/{receipt.json,loaded-images.txt,inspection.json,surface-inventory.json,normal-rejection.json,published.png,output.png,progress.jsonl,restore/restored.json}; traces/stkcop-normal-capture-h479/r2/tape/frame-1681915295368900-1/manifest.json; experiments/output/app-d3d9-stkcop-bench-normal-capture-h479/result.json; experiments/output/app-d3d9-stkcop-bench-normal-capture-h479-r2/{result.json,xrEngine_dxmt9.log}
related: present-pacing-default-capture-scope.478.md; present-pacing-root-game.477.md; ../../../specs/experiments/gap.md
---

# Capture works; Normal replay is a separate next step

The R-HARN-REPLAY-9.19 minimum implementation now has actual-game evidence.
One STALKER courtyard interval publishes with **Normal at construction, arm
and freeze, revision zero**. It was not captured under Exact and relabeled.
Native inspection validates the events, V4 identity pair and execution topology;
the authenticated action witness correctly reports `stable_exact=false`.

## Run and evidence boundary

Both attempts use the same five current built images on Sikarugir CX 24.0.7,
Day/high/1280x720, `-max-threads 2`, 16 audio voices, Metal occlusion OFF,
draw-command limit 0, product counters/frame sampling/validation OFF. Capture
uses frame-tape, a 1 GiB blob cap, 65,536 events and 16,384 unique blob entries.
These are diagnostic runs, not FPS baselines. Capture registry work, snapshots,
drains, getter-cache exclusion, mirrors and the external 250 ms observer remain
perturbations. R2 additionally enables info logging for failure attribution.

R1 exits successfully but produces no bundle: skip 3,200 is above its observed
engine frame range (maximum 1,826). R2 changes the selector to 1,400 and publishes
one bundle. Successful process exit alone did not qualify R1 as a capture.
The external helper observes frame 1,401 during arming and frame 1,404 when it
notices the published manifest; the nearby screenshot shows the building
courtyard. This is **not** an authenticated same-game-frame/source join or the
entire last-ten-second workload. Present skip and engine frame are distinct
coordinates; publication discovery is also delayed relative to execution.

The runner verifies all five staged SHA-256 values, the catalogue receipt
records the same images, the ABI handshake succeeds, and `vmmap` records the
expected Unix provider mapping. The original shared Wine images are restored
and hash-checked after each engine exits. No `.gputrace` or replay execution is
needed to answer the publication question.

| Authenticated artifact | R2 result |
|---|---|
| Bundle | `frame-1681915295368900-1` |
| Action history | Normal / Normal / Normal; arm/freeze revisions 0 / 0 |
| Binding | token 1, epoch 1, queue 2 |
| Events / object definitions / mutations | 5,742 / 749 / 4,905 |
| PE chunks / wire records | 85 / 3,640 |
| Published queue source sequences | 3549, 3550, 3551 |
| Unique blobs / total bytes | 4,665 / 428,355,082 |
| Present completions | 1 |
| Source and output readbacks | 3,686,400 bytes each, identical SHA-256 |

The common source/output digest is
`79db8464262b564381dc98771726a58e092d7ef29dccdd9347bac9e4ddf2801f`.
The preview decodes the authenticated backbuffer descriptor's format 22
(`D3DFMT_X8R8G8B8`) as little-endian BGRX. The `.rgba` filename is not a promise
of RGBA byte order. The raw captures remain unchanged.

Source/output equality checks this original Present mirror path; it is **not**
replay-versus-original equality or proof that previously discarded attachment
bytes are defined. The actual bundle is rejected by the unchanged controlled
CPU authenticator for lacking a stable-Exact action witness, before any child
execution. No existing qualification gate was relaxed.

## Minimum missing checkpoint, grounded in this input

The interval contains 16 surface definitions: swapchain generations,
texture-subresource aliases (including FP color, DF24 and NULL descriptors),
and standalone D24S8. This is not merely a single backbuffer seed. Definitions
and aliases provide storage topology, but do not encode the queue's original
attachment-action history.

The next bounded implementation needs an authenticated arm-boundary attachment
checkpoint, rather than generic Metal command capture:

1. Bind each relevant surface's original touched state to its generation and
   storage/subresource alias identity. `restoreSeededColorAttachmentTouches`
   currently marks every seeded attachment touched; seed presence alone does
   not reproduce Normal first-use decisions.
2. Preserve the current backbuffer identity and post-Present discard state.
   `CommandQueue::completeRenderTapeBackbufferSeed` currently requires Exact
   and clears discard after seed initialization. Normal restoration must not
   silently replace this state with the initializer's history.
3. Distinguish defined initial contents from discarded/unknown contents and
   content established by a captured full write, including depth/stencil
   aspects. Successful readback supplies bytes, not a validity proof. Fail
   closed when the first required read cannot be justified.
4. Consume that checkpoint in a separately admitted Normal replay route,
   retaining original source cuts, and compare its source/output and query
   observations with this original input before measuring any policy change.

`device_c_render_tape_provider.cpp` still explicitly enables Exact for prepared
and preflight replay. This evidence does not authorize changing only that flag,
loosening the launcher gate, or inferring a CPU saving from this capture.
Whole-tail prevalence and H477's game/Wine/PE CPU split remain unresolved.
