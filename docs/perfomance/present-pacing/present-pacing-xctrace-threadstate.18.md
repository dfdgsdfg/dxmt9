---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: xctrace-threadstate
order: 18
title: Xcode System Trace CPU Thread State
date: 2026-06-14
type: attribution
status: accepted
source: traces/app-d3d9-3dmark05-phase43-xctrace-system-r1-20260613/metal-system.trace, traces/app-d3d9-3dmark05-phase43-xctrace-system-r1-20260613/analysis/toc.xml, traces/app-d3d9-3dmark05-phase43-xctrace-system-r1-20260613/analysis/time-profile.xml, traces/app-d3d9-3dmark05-phase43-xctrace-system-r1-20260613/analysis/time-sample.xml, traces/app-d3d9-3dmark05-phase43-xctrace-system-r1-20260613/analysis/runloop-events.xml, traces/app-d3d9-3dmark05-phase43-xctrace-system-r1-20260613/analysis/ca-client-present-request.xml, traces/app-d3d9-3dmark05-phase43-xctrace-system-r1-20260613/analysis/ca-client-presented-handler.xml, traces/app-d3d9-3dmark05-phase43-xctrace-system-r1-20260613/analysis/metal-application-command-buffer-submissions.xml, traces/app-d3d9-3dmark05-phase43-xctrace-system-r1-20260613/analysis/metal-command-buffer-completed.xml, traces/app-d3d9-3dmark05-phase43-xctrace-system-r1-20260613/analysis/process-info.xml, traces/app-d3d9-3dmark05-phase43-xctrace-system-r1-20260613/analysis/dyld-library-load.xml, traces/app-d3d9-3dmark05-phase43-xctrace-system-r1-20260613/analysis/os-log.xml
related: docs/perfomance/present-pacing/present-pacing-pe-wide-call-coverage.17.md, docs/perfomance/present-pacing/present-pacing-pipeline-overlap.05.md
---

# Present-Pacing 18 - Xcode System Trace CPU Thread State

## Question

After [[present-pacing-pe-wide-call-coverage.17]], the exposed front gap is no
longer inside a meaningful PE D3D9 entry point: the final logged child getter
returns at p50 `0.674ms`, then `Clear` enters at p50 `18.421ms`.

One remaining explanation is a broad app/Wine sleep: the producer could be
waiting in a runloop, macdrv event wait, or display/present callback until Metal
completion makes the next frame legal. The available `Metal System Trace`
sidecar was re-exported to inspect CPU/thread tables before adding more PE
D3D9 entry coverage.

## Exported Tables

The trace table of contents includes CPU and scheduling-adjacent schemas in
addition to GPU rows:

| Schema | Rows | Role |
|---|---:|---|
| `time-profile` | `39,283` | sampled running CPU stacks |
| `time-sample` | `42,604` | kperf samples with thread state |
| `runloop-events` | `120,975` | CFRunLoop intervals |
| `thread-info` | `3,083` | PID/TID/thread metadata |
| `os-signpost` | `4,937` | CAMetalLayer/CoreAnimation signposts |
| `ca-client-present-request` | `200` | present request timeline |
| `ca-client-presented-handler` | `200` | presented-handler timeline |
| `metal-application-command-buffer-submissions` | `800` | Metal submission timeline |
| `metal-command-buffer-completed` | `1,200` | Metal completion timeline |

The export required local cleanup first because the volume had only `116MiB`
free. Only ignored, unrelated `traces/sfiv-*` trace bundles were removed.

`process-info` was also exported to confirm PID `97983` is `3DMark05.exe`.
`dyld-library-load` and `os-log` were exportable but contain `0` rows in this
sidecar, so they cannot map the raw PE/app PCs.

## Result

The trace covers `15.563s` of sampled CPU time (`00:00.355` to `00:15.918`).
Within that interval, the main D3D/Wine unix-call producer thread is not
globally asleep:

| Thread | Running sample weight | Dominant evidence |
|---|---:|---|
| `3DMark05.exe (0x3b1b5c)` | `15,354ms` | raw PE/app frames, `mach_continuous_time`, `dxmt9p_buffer_lock`, and unix `commit_chunk`/pipeline-cache stacks |
| `dxmt9-encode (0x3b1c00)` | `5,099ms` | `encodeChunk`, `encodeDraw`, `pushDebugGroup`, timer/counter calls |
| CAMetalLayer submit callback threads `0x3b1b5f`, `0x3b1bf3`, `0x3b1b5e` | `476ms` combined | `IOGPUCommandQueueSubmitCommandBuffers`, CAMetalLayer/IOGPU callback work |
| `dxmt9-completion` | `11ms` running samples | mostly condition-variable wait / `waitUntilCompleted` samples |

CoreAnimation and Metal timing in the same trace has the expected present-paced
shape:

| Metric | p50 | p95 | Max |
|---|---:|---:|---:|
| CA present-request gap | `74.398ms` | `98.436ms` | `118.158ms` |
| CA request -> next presented handler | `33.637ms` | `42.348ms` | `48.881ms` |
| Metal command-buffer submit gap | `5.715ms` | `71.117ms` | `91.119ms` |
| Metal submission duration | `5.721ms` | `12.707ms` | `32.382ms` |
| submission -> next command-buffer completion | `14.397ms` | `24.164ms` | `30.747ms` |

`runloop-events` does not support a runloop-sleep owner for the producer. It
contains only two `3DMark05.exe` rows, both on a different `Main Thread
(0x3b1b5a)` around `14.674s`; it does not show the D3D/Wine producer thread
spending the trace in a CFRunLoop wait.

The dominant producer-thread top frames are still not fully symbolicated:

| Top frame | Samples | Binary |
|---|---:|---|
| `0x20072a19d` | `863` | `?` |
| `0x20072a1bd` | `507` | `?` |
| `0x7ff8a5d37d5d` | `303` | `?` |
| `0x7ff8a5d37c51` | `279` | `?` |
| `0x7ff8a5715359` | `264` | `?` |

Across the first five stack frames of producer samples, `?` accounts for
`23,739` frame occurrences and `winemetal.so` accounts for `12,678`. That
looks like a mix of PE/Wine/unwind-unresolved frames plus unix `winemetal.so`
work, not a clean macOS wait stack.

```mermaid
flowchart TD
  A["PE call coverage<br/>last getter returns p50 0.674ms"] --> B["Clear enters p50 18.421ms"]
  B --> C["Question: sleep/wait or busy producer?"]

  C --> D["System Trace time-profile"]
  D --> E["D3D/Wine producer 0x3b1b5c<br/>15.354s Running / 15.563s trace"]
  D --> F["dxmt9-encode<br/>5.099s Running"]
  D --> G["runloop-events<br/>only 2 rows on other main thread"]

  E --> H["Broad sleep/runloop owner weakened"]
  F --> I["CPU encode remains live P2/P3 owner"]
  G --> H
```

## Interpretation

This does **not** prove the exact PC responsible for the current
`SetRenderTarget` return -> `Clear` entry gap, because this older System Trace
does not contain the PE `Clear`/getter milestone markers. It does reject the
broadest version of the sleep hypothesis: during a representative GT1 System
Trace, the D3D/Wine producer thread is sampled as running for nearly the entire
capture, not sitting in an obvious runloop/macdrv sleep.

The better current model is:

```mermaid
sequenceDiagram
  participant A as App / Wine D3D thread
  participant P as PE D3D9 wrapper
  participant U as unix commit/replay
  participant E as dxmt9 encode thread
  participant M as Metal / CA present

  M-->>A: Present-bearing completion eventually returns
  A->>P: BeginScene and early RT setup
  P-->>A: final logged getter returns quickly
  Note over A: producer remains CPU-active in System Trace<br/>exact raw PE/app PC still unmapped
  A->>P: Clear
  P->>U: first useful record/chunk later crosses
  U->>E: replay/snapshot/publish then encode
  E->>M: Metal command buffer commit
```

Next proof should map the raw PE/app frames on thread `0x3b1b5c` or align a new
System Trace with PE milestone logs. That can be done by adding a lightweight
PE/Wine PC-map diagnostic, or by running a new System Trace sidecar after adding
signpost/log markers that share a time base with `pe_present_call_return` and
`pe_present_call_milestone`. More D3D9 descriptor/getter coverage is not the
right next step.
