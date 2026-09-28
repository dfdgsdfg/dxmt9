---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "H405: Current controlled-lane guest wait owners"
type: evidence
status: engine-audio-waits-reconfirmed-cross-renderer-causality-open
updated: 2026-09-27
source: traces/app-d3d9-stkcop-bench-thread-owner-h405/{run.py,analyze.py,receipt.json,analysis.json,middle.sample.txt,late.sample.txt,snapshot.txt,guest-processes.txt}; traces/app-d3d9-stkcop-guest-wait-h372c/{snapshot.c,ownership-receipt.json}; experiments/output/app-d3d9-stkcop-bench-thread-owner-h405/result.json
related: present-pacing-stalker-gpu-supply-gaps.404.md; present-pacing-guest-wait-ownership.372.md; present-pacing-audio-cap-abba.379.md
---

# H405: Input waits persist; guest ownership is mixed engine/audio work

## Method

One current-binary Day/high run, Sikarugir CX24.0.7, 1280x720 window,
`-max-threads 2`, `snd_targets 16`. Same five staged images and requested,
after-run and control-file hashes as H403/H404; HEAD `dbc216d5`. Keep actual
Metal occlusion and covered-Pending OFF. Runtime counters/frame observers
and PE sampler OFF; no System Trace, concurrent benchmark or build.

Request native `/usr/bin/sample` windows at PID-discovery +100 and +165 s
(3 s, requested 5 ms interval). Native PID is 71051. After the late native
sample completes, reuse H372c's original bounded external PE snapshot helper:
eight rounds over eight guest threads, with per-thread suspend/context/read/
resume and output only after resume. WineDbg is used only for process listing,
not debugger attachment. Guest PID is `0xc4`.

All 64 guest snapshots have prior suspend count 0, resume result 1 and valid
context. All tools and the game exit 0; runner PASS. Engine 28.553917 FPS is
diagnostic only: sampling pauses threads and cannot qualify performance.
Native sampling and guest snapshots are sequential, not the exact same
instant, and neither is H404's exact 176 ms gap.

## Native disjoint stack populations

The reducer subtracts child counts from each call-tree node and checks
conservation per thread. These are sampled stacks, not CPU percentages or
exact wall-clock residence. Repeated Rosetta dispatcher frames remain opaque.

| Stack category | Middle, 131 samples/thread | Late, 153 samples/thread |
|---|---:|---:|
| Encode Ready-prefix condition wait | 62 | 113 |
| Encode backend work | 63 | 35 |
| Replay input condition wait | 63 | 111 |
| Primary address wait | 0 identified | 79 |
| Secondary address wait | 119 | 117 |
| Unnamed helper yield | 101 | 122 |

Middle Primary is entirely unresolved by the classifier; zero identified
address waits does not prove it never waits. Other categories account for
the rest of each thread's population. Encode native TID is 17059646, replay
17059651, Primary 17058999, Secondary 17059665, yielding helper 17059347.

This reinforces H404's supply-gap observation rather than continuous encode
saturation. It does not establish removable milliseconds or a parallel-encode
speedup bound.

## Guest identities and current wait objects

Guest TIDs are distinct from Darwin TIDs. No direct native↔guest TID join
was collected, and none of these IDs may be reused across runs.

| Role | Guest TID | Start / observed evidence |
|---|---|---|
| Primary | `0xc8` | `xrEngine.exe+0x6cdc1` |
| Secondary | `0x144` | `MSVCR80.dll+0x5c980`, engine/sound return sites |
| Yield helper | `0x108` | `xrCPU_Pipe.dll+0x3730`; all eight PCs `ntdll+0x56dec` |
| WinMM callback | `0x13c` | `winmm.dll+0x13b00` |

The guest helper's start/PC evidence is consistent with the native yielding
thread, but is not an exact identity binding to H404's unnamed TID.

Concrete critical-section observations:

| Rounds | Waiter | Object | Header owner | Return evidence |
|---|---|---|---|---|
| 0, 4, 5, 6, 7 | Secondary `0x144` | `0x01775c00` | Primary `0xc8`, recursion 1 | `xrCore+0x26ce9` |
| 1 | Secondary `0x144` | `0x038a6520` | WinMM `0x13c`, recursion 2 | `wrap_oal+0x7c9b` |
| 3 | Primary `0xc8` | `0x01775bc0` | Secondary `0x144`, recursion 1 | `xrCore+0x26ce9` |

The `EDI-16` reads recover complete x86 critical-section headers. Secondary's
round-3 PC is `xrSound+0x1dd3f`; round-2 PC is `xrSound+0x1dd5a`.
Primary's other seven PCs span d3d9 (2), winemetal bridge (2), particles (1),
collision (1), and the game renderer (1). Eight observations are far too few
and too intrusive for cost shares.

WinMM has two PCs in the previously identified scalar mixing routine,
`wrap_oal+0x1c0b0` and `+0x1bff2`, and six timer-wait stacks returning through
`winmm+0x13d36`. Timer idle must not be charged as held OpenAL-lock work.
The installed wrapper SHA256 remains
`d12e434c49009d9309cdcbde838a2d29aa70a641d97d265c3a9f158652c973be`, identical
to H372's static Enter/Leave and lock-RVA evidence. Its load base is
`0x037a0000`; `+0x106520` is the observed audio lock.

These are non-atomic sequential snapshots. Do not combine different rounds
into a simultaneous deadlock, continuous hold, or observed unlock/wake event.
The actual current-run wake edge was not traced. H374 supplies historical
wake evidence, not a replacement for current timing.

## What changed in the conclusion

- The 16-voice cap does not eliminate all OpenAL contention. This is current
  evidence under the controlled lane, not reuse of the old 32-voice result.
- The observed waits are not all audio: engine coordination around Primary
  work is the more frequent guest snapshot pattern here. Primary alternates
  game work and graphics/bridge calls.
- A yielding game helper remains. H370 already tested helper-count reduction;
  do not silently alter Wine yield behavior or repeat it as a new discovery.
- These facts explain plausible supply stalls, **not the 1.825x D9VK gap**.
  D9VK runs the same game/audio settings. Common overhead alone cannot be
  assigned as the differential cause without comparing timing/workload.

Next obtain the equivalent D9VK late-window thread/wait populations with a
scene-qualified trigger, or a current wait/waker timeline bound to renderer
publication. Compare occurrence and duration, not just common function names.
Keep actual-visibility workload differences as a separate axis. Do not add
another audio cap, replace a DLL, parallelize encode or promote a runtime
policy on this snapshot alone.

No runtime code, defaults, README, commit or push changes. Raw snapshots stay
in ignored traces. Documentation-source audit and whitespace checks pass;
no native/TLA or GPU exact tests were rerun because no renderer was changed.
