---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "H406: Engine and audio waits also occur on D9VK"
type: evidence
status: common-wait-owners-confirmed-performance-comparison-unqualified
updated: 2026-09-28
source: traces/app-d3d9-stkcop-bench-renderer-owners-h406/{run.py,compare.py,summary.json}; traces/app-d3d9-stkcop-bench-renderer-owners-h406/{dxmt9,d9vk}/{receipt.json,analysis.json,late.sample.txt,snapshot.txt}; experiments/output/app-d3d9-stkcop-bench-renderer-owners-h406-{dxmt9,d9vk}/result.json; traces/app-d3d9-stkcop-bench-thread-owner-h405/analyze.py
related: present-pacing-stalker-controlled-thread-owners.405.md; present-pacing-stalker-renderer-comparison.399.md
---

# H406: Common engine waits are not a renderer differential explanation

## Method and qualification limit

Run dxmt9 then D9VK sequentially through the catalogue harness with the same
Sikarugir Wine root/prefix, Day/high, 1280x720 window, two game threads and
16 sound targets. Requested, final and control-file SHA256 hashes match.
Runtime perf/frame/query observers are OFF. Both exit 0 with runner PASS.

D9VK uses H399's original app-local release DLL (SHA256
`22511d1fbb15cdbc5365dfcb231f028af427533bfb6957505720dac0aca98e3e`), the same
cache and MoltenVK 1.4.1. The startup banner confirms the D9VK route. Its
`--skip-stage` receipt describes installed dxmt9 images, not the active renderer.
The supervisor removes the hash-validated app-local DLL after D9VK exits;
the vendor source and installed Wine-root DLLs remain untouched.

Unlike H405's process-relative trigger, wait for the first batch of
`ERROR: OA not initialized.` messages and request native sampling 80 seconds
later. These messages occur during native benchmark execution, but are
buffered and are **not a precise scene clock**. Both first batches contain
151 lines. The first batch arrives 109.72 s after launch for dxmt9 and
242.49 s for D9VK, demonstrating why launch-relative timing is unsuitable.

Native sampling requests 3 s at 5 ms, followed by the bounded eight-round
guest helper. Helpers balance all 64 dxmt9 / 128 D9VK thread snapshots.
D9VK has additional guest workers, so the total snapshot burden differs.
Do not treat the policies as equally perturbed or the windows as exact
camera-aligned. There is no continuous focus/host-load recording.

**Performance qualification fails.** Diagnostic engine averages are 15.600501
and 24.507856 FPS, both far below H399's reference, with reconstructed scene
durations 134.80 / 131.43 s rather than about 119 s. Do not compute a new
renderer ratio or attribute this drift to sampling, focus, thermal state or
code without evidence. Code/config identity does not establish equivalent
runtime conditions. Buffered OA counts also differ from result frame counts.
Mapping the observed prefix counts to result frames estimates about 79.49 /
74.70 s, but this is explicitly an approximate, unverified alignment.

## Native observed stacks

| Category | dxmt9 samples | D9VK samples |
|---|---:|---:|
| Primary address wait | 155 / 198 | 146 / 166 |
| Secondary address wait | 182 / 198 | 164 / 166 |
| Unnamed helper yield | 143 / 198 | 123 / 166 |
| dxmt9 encode Ready-input wait | 174 / 198 | not applicable |
| dxmt9 replay input wait | 180 / 198 | not applicable |

Counts are disjoint sampled call-stack categories, not CPU time, wall-time
fractions or renderer cost shares. D9VK submit/cs/queue stacks largely unwind
into repeated Wine syscall-dispatcher frames, so their cost is unresolved.
Do not label these unresolved populations either busy or idle.

## Guest wait objects on both routes

dxmt9 guest process `0xc4`: Primary `0xc8`, Secondary `0x144`, WinMM `0x13c`.
Primary has four engine-wait snapshots; Secondary three. Primary's object
`0x01975bf0` is observed owned by Secondary; Secondary also waits on
`0x01975c30` owned by Primary. No OpenAL wait is caught in these eight rounds,
but five WinMM PCs lie in the known scalar mixing loop. Absence of an audio
wait here does not negate H405's captured audio wait.

D9VK guest process `0x174`: Primary `0x178`, Secondary `0x1ec`, WinMM `0x1c4`.

| D9VK rounds | Waiter | Object | Observed owner | Return evidence |
|---|---|---|---|---|
| 1, 3, 7 | Primary | `0x01975c80` | Secondary `0x1ec` | `xrCore+0x26ce9` |
| 0, 5, 7 | Secondary | `0x03aa6520` | WinMM `0x1c4`, recursion 2 | `wrap_oal+0x7c9b` |
| 1 | Secondary | `0x01975c80` | Primary `0x178` | `xrCore+0x26ce9` |
| 6 | Secondary | `0x01975cc0` | Primary `0x178` | `xrCore+0x26ce9` |

Sequential per-thread snapshots are not atomic: round 1 observes ownership
handoff on the same engine object, not a simultaneous cyclic deadlock.
No current unlock/wake/return timeline or continuous hold durations were
collected. The yielding guest thread starts at `xrCPU_Pipe+0x3730` in both
routes. D9VK WinMM has six sampled PCs in the same scalar mixing interval,
one other wrapper PC and one timer-wait PC.

## Decision

The engine/Secondary/OpenAL chain is not dxmt9-specific. Observing it in
dxmt9 alone cannot explain why D9VK's previously qualified run was faster.
This comparison establishes common mechanisms, not equal waiting time,
equal work, or causality for H399's gap. It also cannot infer that more GPU
or encode parallelism will resolve the engine's dependency waits.

Before another comparative cost attribution, restore a short observer-OFF
control under demonstrably stable conditions and obtain a stronger scene
anchor. A subsequent matched-window comparison should measure actual game
work and query visibility outcomes, or time the dependency chain rather than
count its presence. Runtime shader/queue changes, sound reduction and Wine
yield changes are not justified by this result.

No runtime/default/README changes, commit or push. Raw diagnostic results and
the qualification failure are retained. Documentation artifact audit and
`git diff --check` pass.
