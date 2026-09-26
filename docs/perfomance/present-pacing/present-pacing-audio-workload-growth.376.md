---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2
title: "H376: Late audio mixing sees many more playing sources"
type: evidence
status: partial-workload-growth-attribution
updated: 2026-09-26
source: traces/app-d3d9-stkcop-audio-workload-h376/{snapshot.c,run.py,receipt.jsonl,snapshot-1.txt,snapshot-2.txt,analyze.py,analysis.json}; traces/app-d3d9-stkcop-audio-owner-h375/static.json; experiments/output/app-d3d9-stkcop-bench-audio-workload-h376/result.json
related: present-pacing-audio-owner-execution.375.md
---

# H376: Source workload grows in the late mixing window

Two bounded 80-snapshot windows in one Day/high 1280x720 `-max-threads 2` run
start after observed frames 603 and 2700. The helper selects the WinMM timer
thread as in H375. It only decodes workload when its PC is inside the known
push-free inner loop RVA `0x1bf84..0x1c0d4` and the global lock header shows
that thread owns the lock. The selected thread remains suspended during reads.

Installed-binary disassembly establishes these offsets for this build only:

- stack + `0x58`: context pointer; + `0x2c`: output-frame loop bound;
  + `0x34`: fixed-point source increment (scale `0x4000`).
- context + `0x74` / `0x78`: source-list head / declared count.
- source + `0x124`: next node; + `0x58`: state compared to `0x1012`
  (`AL_PLAYING`) before the mixing loop.

Traversal is bounded to 256 nodes, rejects cycles/read failures and requires
the traversed count to equal the declared count with a null list terminator.
No audio API is invoked and no game memory is written. The decoder is specific
to the saved binary and PC interval, not a public OpenAL structure contract.

| Qualified inner-loop observations | Early window | Late window |
|---|---:|---:|
| Accepted / total snapshots | 2 / 80 | 62 / 80 |
| Declared source list size | 32 | 32 |
| Playing sources, min / median / max | 2 / 2 / 2 | 22 / 32 / 32 |
| Output-frame bound, min / median / max | 941 / 1,176 / 1,411 | 470 / 1,411 / 1,412 |
| Current source increment | 16,384 | 16,384 |

The current-source increment is unity in these observations; it does not prove
that every source has the same ratio or that interpolation is skipped. Frame
bounds describe sampled mixer invocations, not unique callback counts: the
callback may split its output and invoke the mixer more than once.

## Interpretation and qualification

The early qualified observations have only two playing sources; the late
observations have 22–32. The output-frame bound does not show a comparable
order-of-magnitude increase. This supports increased concurrent source work
under the global lock as a concrete reason the tail is more expensive, rather
than assuming a fixed workload suddenly executes more slowly in dxmt9.

Early evidence is only **two qualified snapshots**. Sampling inside the loop
is duration-biased, not callback-uniform. Do not report a 16x total CPU cost,
derive an FPS ceiling, or claim identical per-sample speed from this table.
Qualification failures include samples outside the selected PC range; they do
not mean the audio list was empty. More early observations and source-lifetime
or sound-event attribution are needed before tuning a source cap.

Both sets contain 80 balanced suspend/resume/context records. The diagnostic
helper prints a literal `\\n` separator before thread records; the parser
explicitly normalizes that separator without modifying raw evidence. The app
passes with exit zero, zero reported GPU errors, and the same five staged
binaries as H375. This intrusive run is not FPS qualification. No runtime,
sound setting, DLL or benchmark baseline changes.

Next verify whether the 32 late playing sources represent intended scene audio
or avoidable source lifetime/looping work. If testing a source limit or another
audio implementation, preserve an explicit original baseline and qualify sound
semantics; fewer voices may be a quality tradeoff, not a correctness-preserving
optimization.
