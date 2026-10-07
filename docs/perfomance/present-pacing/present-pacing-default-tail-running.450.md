---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "Default-path tail Running samples: compatibility snapshot staging candidate"
type: evidence
status: sampled-owner-located-removable-cost-open
updated: 2026-10-07
source: traces/app-d3d9-stkcop-cpu-running-h450/{run.py,receipt.json,map.sample.txt,scene-before.png,late.trace,export-results.json,time-sample.xml,metal-gpu-intervals.xml,metal-application-command-buffer-submissions.xml,ca-client-present-request.xml,analyze.py,symbols.json,running-samples.json,running-summary.json}; experiments/output/app-d3d9-stkcop-bench-cpu-running-h450/{result.json,stkcop-requested.ltx,stkcop-after-run.ltx,stkcop-controls.txt}; src/d3d9/core_draw.cpp; include/dxmt9/core_snapshots.hpp
related: present-pacing-default-tail-cpu.449.md; present-pacing-l64-repaired-wild-abba.448.md
---

# Running samples locate a staging candidate, not measured savings

At `bafada9e`, the unchanged repaired H448 images run Day/high 1280x720,
Sikarugir CX24.0.7, two engine threads, sixteen sound targets and command
limit 0. Product performance observers remain OFF; external profiling is ON.
The runner passes without timeout. All five staged artifact receipts and the
requested/final/control-file SHA-256 values match H448 A1. A late-scene screenshot
before profiling shows buildings/street; this is neither continuous foreground
evidence nor an exact pixel oracle. No runtime or default changes.

## Attribution boundary

An eight-second Metal System Trace starts approximately 185 seconds after engine
discovery. Analysis excludes attachment edges and selects trace-relative
`[1,7)` seconds, PID 92491, `Timer Fired` samples with thread state `Running`.
The template's timer period is 1000 microseconds. XML id/ref references are
resolved before filtering. A same-process native stack capture identifies app,
Replay and Encode TIDs and the provider load address `0x21bf5a000`.

The symbolized x86_64 provider has UUID
`6C6BEBCE-2712-350C-8E27-EC0D01D6DD45` and SHA-256
`7f45561ddf0fd22f477e3d3447000913aac83c5b61950726fbe17cffc88aece5`.
The UUID and staged-image hash are checked against the local image before
`atos` resolves provider backtrace addresses. Opaque guest/Rosetta PCs are not
assigned to PE. A nearest provider ancestor can include callees; even the top
x86 backtrace entry is not necessarily the interrupted translated instruction.

The full `time-profile` export crashes with exit -5. Raw `time-sample`, GPU
interval and submission exports succeed and supply these results; no conclusion
depends on the failed export. Trace saving outlives the game, so there is no
post-trace scene screenshot.

## Sampled owners

| Thread / scope | Running samples | Interpretation |
|---|---:|---|
| App Primary | 5,299 | Game/Wine/PE/synchronous Unix combined; only 498 have provider ancestry |
| Replay | 3,106 | Identified by `ReplayOffloadWorker::run` ancestry |
| Encode | 3,266 | Identified by native thread name and stack |
| Replay: `submitCompatibilityReplayDrawBatched` inclusive | 1,198 (38.57%) | Includes cache preparation and other nested work |
| Replay: same function nearest provider / top x86 backtrace | 576 / 576 | Callsite envelope, not exclusive instruction cost |
| Encode: `encodeDraw` inclusive | 1,267 (38.79%) | Includes backend calls; not all avoidable |

Replay cache preparation has 642 inclusive samples; batch flush has 943 and
`submitDrawRunBatch` has 915. These overlap the table and each other: do not
sum them. Encode's broader DrawRun loop, render-encoder creation and command
encoding also consume CPU. This does not establish Replay as the largest
whole-pipeline owner or isolate the Primary thread's PE share.

## Concrete duplicate-materialization lead

`src/d3d9/core_draw.cpp`, `Device::submitCompatibilityReplayDrawBatched`, creates
an owned `CompatibilityBatchRun` for each new stateful run, then moves it into
`compatBatchRuns_`. The local run copies cached canonical state and the full
uniform payload. In the measured binary, large inline arrays are copied even
on the vector's capacity-available path; a C++ move does not eliminate these
bytes. The vector element stride is `0x5458` (21,592 bytes).

Bounded `llvm-objdump` disassembly and Mach-O indirect symbols identify the
callee at `0x4170b8` as `_memcpy`. Five frequently sampled top-backtrace return
addresses immediately follow these calls (addresses relative to provider load):

| Return address | Copy length | Source / destination in generated code | Samples |
|---|---:|---|---:|
| `0x439b9` | 8,112 B | Cached state to stack temporary | 77 |
| `0x43b4e` | 8,112 B | Stack temporary to local run | 79 |
| `0x43f05` | 10,836 B | Cached uniform region to local run | 82 |
| `0x43f84` | 8,112 B | Local run to vector element | 95 |
| `0x440dc` | 11,152 B | Local run trailing region to vector element | 122 |

These 455 samples are 14.65% of Replay's Running sample population. They are
copy-callsite evidence, **not 455 ms proven removable**: sampling/unwinding can
attribute a library callee to its return address, and some ownership copies
remain necessary. The generated path nevertheless confirms repeated large
materialization rather than merely inferring it from a function name.

`include/dxmt9/core_snapshots.hpp`, `CompatibilityBatchRun`, explains why an
owned snapshot is needed: the mutable cache can rebuild while older runs are
pending. Binding spans also point into reserved run storage until publication.
Replacing these snapshots with naked cache references is unsafe. The bounded
candidate is direct construction in already reserved run storage, preserving
ownership, span stability, failure behavior and publication boundaries. It is
not restoration of the old carrier or a new batch-capacity policy.

## Submission timing

The union of target-process active GPU intervals occupies 1.4011 of six seconds
(23.35%). There are 92 Present requests; their rate is not engine benchmark FPS.
The three largest internal GPU idle gaps are 35.064, 34.723 and 31.035 ms.
The following command-buffer commit occurs 34.630, 34.292 and 30.615 ms after
the respective gap starts; commit-to-GPU is only 0.435, 0.430 and 0.421 ms.
Thus these observed gaps are mostly before submission, not sustained GPU work.
This is not a total GPU-utilization claim or proof that Replay alone causes
the gaps. The largest gap contains 33 app, 25 Replay and four Encode Running
samples, suggesting upstream service deserves attention before Encode-only
parallelization.

## Decision and missing evidence

Stop retuning L64/headroom from these data. The next bounded comparison is
identical-input compatibility-run construction with and without intermediate
owned temporaries, followed by whole-Replay CPU measurement. Preserve source
cuts and state/resource identity; validate CPU semantics and GPU output before
using observer-OFF reversed-order wild runs to assess end-to-end benefit.

If generated copies disappear but whole-Replay CPU does not decrease beyond
measurement variation, reject this candidate as a meaningful CPU lever. If
Replay improves but wild FPS does not, record the local saving without claiming
critical-path ownership. No savings percentage, largest removable owner, FPS
benefit, README update or default promotion is established here.
