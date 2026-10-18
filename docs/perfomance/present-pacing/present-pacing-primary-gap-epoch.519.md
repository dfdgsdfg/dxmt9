---
domain: present-pacing
workload: STALKER Day high, combined root-gap and sparse-worker diagnostic attempt
title: "Live Primary arming rejects a second D3D9 image epoch"
type: evidence
status: rejected-before-primary-arming-no-cpu-result
updated: 2026-10-14
source: traces/stkcop-primary-gap-game-h519/{live-contract.md,control.cpp,generation.json,run.py,inputs.json,clocks.json,failure_analysis.py,failure-analysis.json}; traces/stkcop-primary-gap-game-h519/run1/{receipt.json,progress.jsonl,startup-focus.json,gap.bin.196,self.bin,recovered.json,restore/inventory.json,restore/restored.json}; experiments/output/app-d3d9-stkcop-bench-primary-gap-game-h519-run1/result.json
related: present-pacing-primary-gap-abi.518.md; present-pacing-source-self-thin.516.md; present-pacing-primary-gap-preflight.517.md
---

# Retain the rejected episode; no Primary price

The first combined H518 PE / H516 Unix diagnostic episode reaches the frozen
late camera knot 0, then rejects Primary activation. It has **zero Primary
gap rows**, no knots 1–6 and no valid CPU interval. Do not quote its partial
worker trace, total duration or catalogue result as a performance observation.
The largest default dxmt9 single-thread owner and removable budget remain open.

## Concrete failure

H519's predeclared controller requires `map->incarnation == 1` and rejects any
image reload in the process. The retained map instead has two attachment slots:

| Field | First attachment | Second attachment |
|---|---:|---:|
| Image epoch | 1 | 2 |
| Wine thread ID | 200 | 200 |
| Native thread ID | 5018598 | 5018598 |
| Native PID | 18968 | 18968 |
| Native birth (Unix microseconds) | 1791912696413377 | 1791912696413377 |
| Cold binding queries | 1 | 1 |
| Gap clock queries / completed rows | 0 / 0 | 0 / 0 |

The map reports no coverage fault, overflow or dropped row. Its second epoch
alone is sufficient to fail the controller's fixed-epoch prerequisite. It
does **not** show a cross-process/TID alias or renderer correctness failure.
The exact reload time is not traced; do not invent a particular adapter-probe
or initialization call as its cause.

The supervisor had enabled worker reporting immediately before attempting
Primary activation. It stops the owned game after controller exit 6 and
preserves the failed episode. The catalogue reports `process_exit` and
`missing_capture`; unlike H516, this is **not** an administrative-only failure
with normal game completion. The final map's `liveImages=1` survives forced
termination and is not evidence that the process remains live. OS process
inspection confirms all Wine/game/helper processes are gone.

## Scope and recovery

The run retains the same controlled Day/high/1280x720 policy, max-threads 2,
snd_targets 16, draw limit 0 and production-counter occlusion. Product counters,
frame sampling and supply trace remain OFF. A computer-use startup restore
requests focus; active/stable camera observations, not the UI return code,
qualify knot 0. No post-tail screenshot exists because the tail was not reached.

All 14 existing shared renderer/support/prior-result files are hash-restored.
The four new clock-service/discovery files are moved into `run1/recovered/`.
Frozen H518 images and the H516 reporter image are preserved. No production
source, default, canonical image, README FPS, commit or push is changed.
`failure_analysis.py` independently verifies the raw epoch/slot evidence,
missing measurement endpoints, restoration hashes and absence of processes.

## Next mechanism, not a retrospective gate change

H519 remains rejected under its frozen contract. Before another game run,
replace the **first-image assumption** with an explicit activation receipt:

1. Bind the current image epoch together with process birth and native/guest
   thread identity at activation; retired epochs must contain no admitted gap.
2. Check that this exact attachment remains current across the request write,
   and return an immutable epoch-qualified activation receipt.
3. Require every accepted row and the final map to retain that receipt's epoch;
   reload during the measurement, stale receipt, active retired interval or
   changed process identity rejects the episode.
4. Test successful activation after cold reload and negative epoch/identity
   races with the actual H518 load/unload mechanism before live use.

This changes the authority from “first DLL ever loaded in the process” to
“the exact DLL attachment owning this measurement”; it must not simply allow
arbitrary epochs or reinterpret H519 as a pass. The root-gap subset's utility,
observer interference, exclusive Primary ownership and actual removable work
still require separate evidence. No synthetic timer subtraction or repetition
of H477's all-call-clock campaign is justified by this failure.
