---
domain: present-pacing
workload: STALKER CoP H502/r5 original Normal input
title: "Source 5 CPU is distributed over DrawRuns, not nondraw dispatch"
type: evidence
status: bounded-command-owner-identified-removal-open
updated: 2026-10-15
source: traces/stkcop-command-owner-h542/{contract.md,run.py,analyze.py,summary.json,structure.py,structure.log,structure.json,structure-receipt.json,verify.py,verification.json}; traces/stkcop-command-owner-h542/{A0,B0,B1,A1}-receipt.json; traces/stkcop-command-owner-h542/{A0,B0,B1,A1}/analysis.json
related: present-pacing-encode-source-selection.541.md; present-pacing-prefix-live-result.540.md; present-pacing-normal-source-cost.486.md
---

# Source 5 is a distributed DrawRun cost

Four fixed fresh-device executions, A0/B0/B1/A1, preserve the original source
and output bytes, seven source mappings, 670 balanced wrappers and production-
counter query obligations. A keeps coarse source tracing; B additionally enables
the existing command split observer. Per-draw clocks remain OFF. The frozen
H537 A executable/runtime images are unchanged, with before/after hashes and
A0 dyld paths checked. No build, Wine staging, profiler or GUI operation occurs.

The private run uses the fresh-device contract, unlike H541's rejected repeat
probe. All traces are closed without overflow or incomplete writers. Analysis
joins captured source 5 to local sequence 8 and checks same-thread monotone
self CPU. A separate reducer independently pairs all command endpoints and
reproduces DrawRun totals. These are captured-source costs, not ms/frame.

## Source 5 command partition

| Exclusive body component | Calls | B0 CPU | B1 CPU |
|---|---:|---:|---:|
| DrawRun, including setup/pass/loop/Metal work | 526 | 19.876 ms | 19.550 ms |
| Nondraw commands | 65 | 0.157 ms | 0.153 ms |
| Per-record split policy | 591 | 0.298 ms | 0.283 ms |
| Dispatch/observer residual | — | 0.921 ms | 0.900 ms |
| Total command body | — | 21.252 ms | 20.886 ms |

DrawRun accounts for approximately 94% of the **observed** body. Removing
nondraw dispatch or the per-record split classifier is not a mechanism for
removing most of this measured body. This does not exclude query synchronization
as a whole-game bottleneck: queue blocking and publication/completion outside
these scopes are separate from encoding the nondraw commands.

The ten most expensive DrawRuns account for only 11.47%/11.07% of DrawRun CPU.
The largest costs 0.266/0.260 ms; the median costs 0.014/0.013 ms. There is no
single millisecond-scale DrawRun accounting for the source's roughly 20 ms.
Command ordinals here are reconstructed from complete ordered endpoints,
not guessed from sample timestamps.

Source 1 contains 228 DrawRuns and no nondraw commands: its DrawRun scopes
cost 6.190/6.406 ms, within 6.733/6.936 ms command bodies.

## Observer limits

Source 5 coarse-only bodies are 21.565 ms (A0) and 19.986 ms (A1). B0−A0 is
−0.313 ms, whereas B1−A1 is +0.900 ms. Source 1 differences are +0.450/+0.822 ms.
Those four observations do not establish negligible instrumentation overhead,
a statistical bound or a product performance effect. A0 additionally prints
dyld images. Do not subtract these differences from B's child scopes to invent
an observer-free partition. No run is excluded or repeated to improve the
result; the old H541 18-run baseline remains separate.

## Separate unpriced draw/state multiplicity

One additional fresh-device execution enables sequence-filtered encode-progress
logging, with supply trace OFF. It is not an extension of the timing set.
Both original readbacks and all query obligations pass. Every original source
map is retained; source 5 remains local sequence 8.

The complete `drawrun.enter` transcript contains 526 unique command ordinals,
matching the timed DrawRun classification. Each command's declared draw count
equals its actual logged draw-loop entry count:

- 3,125 draw iterations, 526 DrawRuns, 526 distinct source-local state indices.
- 351 single-draw runs contain 351 draws.
- 175 multi-draw runs contain 2,774 draws; the maximum is 80 draws per run.

Joining those ordinals to B0/B1 gives 5.640/5.465 ms in single-draw runs and
14.236/14.085 ms in multi-draw runs. This is a descriptive same-input join,
not a causal estimate of per-run versus per-draw overhead. State indices are
row identities, not proof of distinct shaders or semantic inequality. The
unpriced logging execution supplies no timing authority.

## Decision

Focus next on the multi-draw loop and its command-level preparation, preserving
the actual batching already present. A return to draw-by-draw submission is not
indicated: most draws are already in multi-draw runs. Before choosing a new
cache, separate once-per-run preparation/pass work from the complete loop using
run-level milestones, not another clock around every draw. Existing H487 scratch,
H497 raster-cache and H499 vertex-shadow negative gates stay in force.

The current source trace does not distinguish driver encoding from required
translation or prove removable CPU. It also cannot establish canonical live
owner ranking or isolate Primary's game/Wine/PE mixture. No runtime/default
change, FPS claim, qualified removal amount or goal completion follows.
