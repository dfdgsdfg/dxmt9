---
domain: present-pacing
workload: STALKER H502/r5 original V4 input and ordinary-route negative control
title: "The 6ms append residual belongs to Tape verification"
type: evidence
status: harness-observer-cost-attributed-production-saving-rejected
updated: 2026-10-16
source: traces/stkcop-append-tail-h591/{contract.md,prepare.py,observer.hpp,native.cpp,native.json,parser-tests.json,build.json,run.py,report.json,verify.py,verification.json,normal-control-contract.md,normal_control.py,normal-control.json,normal-control.bin,normal-control.log}; traces/stkcop-append-tail-h591/{on0,on1}-batch.bin; traces/stkcop-append-tail-h591/{off0,on0,on1,off1}/{census.json,source.bgrx,output.bgrx}
related: present-pacing-append-nesting.590.md; present-pacing-batch-residual.589.md; present-pacing-reserve-recurrence.588.md; present-pacing-sparse-cpu.584.md
---

# Do not count capture verification as removable default rendering work

H591 separates the work combined in H589's named Append phase. Actual append
costs **3.087–3.850 / 3.353–4.123ms caller CPU**; Tape runtime observation costs
**5.909–6.653 / 5.972–6.718ms** in the same executions. The approximately6ms
residual belongs predominantly to verification, not state copies or capacity
settlement. All values are per captured input, not per live frame or savings.

## Boundaries and controls

Two private objects change: command queue adds Settlement at mutation callback
exit; queue brackets `observeTapeRuntimeNewCommands` only for the same thread's
active batch in Settlement. No callback, settlement, command or lifetime work
is skipped. No product signatures, layouts, policies or source files change.
Formal rendering refinement is inapplicable to this observer-only experiment.

ARM64/Rosetta 12-process controls pass: inactive/wrong-phase admission, overflow,
false return, exception unwind, unavailable storage and `_exit` durability.
Parser positive/14 negative fixtures pass. Build warnings are absent and the
install-name audit passes. Inherited build stdout incorrectly says one changed
object; the authoritative archive comparison records **two**.

Fixed OFF0/ON0/ON1/OFF1 completes without replacement. Product perf/append clocks
are OFF. All four preserve original source/output BGRX SHA-256
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`, seven source
boundaries, query transcript, 670 wrappers and normal-state restoration.
Both ONs have 84 batches, 2,828 draws, 2,343 entries and 1,176 phase rows:

`Setup → Prepare → Acquire → Binding → Slot → Plan → Reserve → Mark → Append → Settlement → Observe → AfterObserve → Commit → Epilogue`.

Independent verification imports neither parser nor estimator. It authenticates
hashes, checks phase/input shapes, recomputes CPU/wall bounds and verifies
telescoping batch CPU. Caller-self read extents and 4us endpoint allowances
remain included. This is not an observer-overhead-corrected estimate.

| Disjoint scope, ms per captured interval | ON0 CPU bounds | ON1 CPU bounds | ON0 / ON1 body wall |
|---|---:|---:|---:|
| Actual append callback body | 3.087–3.850 | 3.353–4.123 | 3.464 / 3.734 |
| Callback exit to observer entry, including settlement | 0–0.394 | 0–0.395 | 0.011 / 0.019 |
| Tape runtime observation | **5.909–6.653** | **5.972–6.718** | **6.281 / 6.337** |
| Observer exit to mutation-helper return | 0–0.391 | 0–0.389 | 0.004 / 0.005 |

Small CPU uppers are allowance-limited, not proof of large settlement costs.
Enclosing batch CPU is 17.031–17.904ms / 17.771–18.636ms and includes cold
reserve and verification. Do not add enclosing and child costs.

## Default versus explicitly armed Tape observer

`src/dxmt9/dxmt9_render_tape_runtime_observer.hpp`, `TapeRuntimeObserverEndpoint`,
initializes `current_` to null; `enabled()` requires explicit attachment.
Attach sites are capture/replay adapters. The V4 executor explicitly arms
`beginRenderTapeIdentityCaptureV4ForReplay` in
`src/d3d9/device_c_render_tape_execution_v4.cpp`. `mutateWritingPayload` calls
the measured observation only when enabled.

A separately declared logical negative control runs the existing ordinary
fixture against exact H591 images, authenticating loaded paths. It executes
two observed batches with **zero Observe/AfterObserve calls**, successful
ordinary-route markers, completion sequence2 and two API-validated readbacks.
It intentionally skips draws: this proves route gating, not whole-game pixels
or performance. It is not an extension of the four timing runs.

H588 live receipts have no Tape capture/arm settings or capture log markers.
Together with the explicit attach contract and ordinary negative control,
the measured6ms cannot be charged to those default runs. Capture-enabled game
runs can activate the callback; the claim is not that it never runs under Wine.

## Decision

Close the approximately6ms residual as a **harness cost**, not a production
optimization budget. Preserve the verifier. H588 already rejected cold reserve
growth as the major warmed live owner. Actual append and preparation remain
real but smaller local costs; their full removal is neither possible nor proved.

The global goal stays open. Live Replay/Encode remain near30ms/frame with
overlapping bounds, and no operation-level production removal is established
by H591. Next use bounded default-live operation coverage or revisit the
retained whole-draw-loop owner; exclude proof callbacks and cold-growth
transients before selecting another optimization.
