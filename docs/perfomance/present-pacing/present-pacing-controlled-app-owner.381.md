---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "H381: Controlled Primary samples expose game work and burst backpressure"
type: evidence
status: partial-app-owner-attribution
updated: 2026-09-26
source: traces/app-d3d9-stkcop-app-owner-h381/{run.py,snapshot.c,snapshot.txt,analyze.py,analysis.json,summarize.py,summary.json,sample.txt,receipt.jsonl}; experiments/output/app-d3d9-stkcop-bench-app-owner-h381/{result.json,dxmt9.log}
related: present-pacing-audio-controlled-supply.380.md
---

# H381: App CPU is not PE CPU; average starvation coexists with backpressure

Commit `070dcf59` records H372–H380 and the explicit audio-control harness.
Then run the same controlled Day/high 1280x720 lane, worker limit two and voice
cap 16. After observed frame 2701, capture 320 bounded guest Primary contexts
and a separate eight-second native sample. The guest selector resolves engine
base plus known start RVA `0x6cdc1`, not a reused TID. All guest snapshots have
prior suspend count zero, resume result one and valid context. The app passes,
reports zero GPU errors and stages the same binaries as H380.

These simultaneous diagnostic samplers perturb the run, do not have identical
windows, and are not joined per sample. They provide owner leads, not calibrated
CPU-time fractions, H380's 43.49ms decomposition, or FPS improvement evidence.

## Guest top-PC classification

| Module / class | Samples of 320 |
|---|---:|
| `xrCDB.dll` | 66 |
| `xrRender_R2.dll` | 54 |
| `xrParticles.dll` | 34 |
| Other game/runtime modules (xrGame, Lua/JIT, ODE, etc.) | 25 |
| `d3d9.dll` | 29 |
| `winemetal_dxmt9.dll` | 52 |
| `ntdll.dll` | 54 |
| CRT modules | 2 |
| Unresolved addresses | 4 |

The 179 game-module PCs show substantial game-side work. Names suggest
collision/render/particle domains, but no private function semantics are
assigned merely from DLL names. PE samples distribute among recorder/semantic
admission, retention, StateBlock and constant-state paths rather than one
dominant function; the symbol receipt preserves nearest text-symbol mapping.

Of 54 ntdll PCs, 49 are the known alert-wait PC: 41 unwind through
`xrCore::xrCriticalSection::Enter` and eight through the OpenAL global Enter
wrapper. Residual engine/audio waiting therefore survives cap 16. Other ntdll
samples include clocks/dispatch and are not all waits.

Of 52 bridge-module PCs, **49 are RVA `0x1990`**, immediately after the
indirect Unix-call instruction in `dxmt9_winemetal_unix_call`. A parked guest
return PC can represent ongoing Unix work; it must not be labelled exclusive
bridge marshalling overhead. The independent native stack supplies the next
level of attribution, without pretending to pair its samples to guest PCs.

## Native synchronous Unix owners

The Primary has 336 native sample observations. Inclusive thunk-path counts
across its call-tree branches include:

| Native call path | Observations | Important descendant |
|---|---:|---|
| `device_commit_chunk` | 23 | 11 in resource-mark/capture mutex acquisition |
| `stateblock_apply` | 11 | 10 in replay queue space condition-variable wait; one in mutex acquisition |
| `buffer_unlock` | 5 | Too few observations for independent cost ranking |
| `buffer_lock` | 3 | Same limitation |
| Texture addref/release, shader addref | 1 each | Same limitation |

The 11 resource-mark observations reach
`markChunkResourcesAndCaptureBufferBindings` → `std::mutex::lock` → pthread
mutex slow wait. They are **acquisition waits, not 11 samples of resource-loop
CPU**. Current `src/dxmt9/dxmt9_command_queue.cpp`,
`CommandQueue::markChunkResourcesAndCaptureBufferBindings`, already places the
ordinary stamp and binding-capture loops outside the queue lock; its remaining
frozen-ticket acquisition/rare re-stamp still can encounter another holder.

The ten StateBlock observations reach `enqueueStateBlockApply` →
`ReplayOffloadQueue::pushStateBlockApply` → `spaceCv_` wait. In
`src/d3d9/device_c_replay_offload.hpp`, `pushStateBlockApply` waits while the
queue is at `maxChunks_`, unless stopping. This identifies queue-space
backpressure, not a wait for that StateBlock's GPU completion.

## Decision

H380's high average empty-queue fraction does not rule out short full-queue
bursts. Do not collapse the pipeline to a single permanent producer bottleneck,
or infer that increasing queue capacity is safe/useful. Both upstream game work
and synchronous queue acquisition/backpressure are now concrete owners.

Next instrument **StateBlock queue-space entry/exit with replay identity and
depth**, and **the holder responsible for the mark-ticket mutex acquisition**.
Connect those bursts to replay service/publication and the app's supply gaps
before changing capacity, lifetime marking, or StateBlock ordering. The current
selected wait trace's empty result did not cover these owners. Keep CPU work,
blocked wall time and guest return-PC attribution separate.

## Follow-up instrumentation (not new runtime evidence)

The bounded supply transcript now appends StateBlock queue-space capacity,
entry/exit depth and adoption events. A process-local diagnostic attempt ID is
used because the ledger assigns `replaySeq` only after queue-space admission.
Adoption links the attempt to that sequence; zero/no adoption remains
unqualified, not proof of rejection. CV elapsed time includes reacquiring the
queue mutex. No FIFO, capacity or lifetime policy changes.

The mark-ticket capture site also records acquisition entry/exit and the end
of its locked body, keyed by queue, thread and diagnostic attempt, carrying
the original marking ticket (not a raw-source identity). The body endpoint
precedes the existing probe destructor and actual unlock; it is not an exact
mutex-release timestamp. All timings include observer overhead.

`scripts/tools/analyze_app_queue_waits.py` reads the existing qualified v2
transcript, pairs attempts, excludes clipped endpoints and reports unresolved
adoption. Follow-up work adds same-mutex/different-thread held-segment
intersection at existing lifecycle unlock/CV boundaries. This is partial
coverage; uninstrumented CommandQueue sites and the gaps between a segment
endpoint and actual unlock remain unattributed. Time overlap with publication
alone does not establish lock ownership.
