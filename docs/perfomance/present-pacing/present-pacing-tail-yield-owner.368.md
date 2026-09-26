---
domain: present-pacing
workload: STALKER CoP Day
title: "H368: Seven CPU-consuming threads resolve to Wine yield calls"
type: evidence
status: partial-attribution
updated: 2026-09-26
source: traces/app-d3d9-stkcop-tail-h368/{analysis.json,thread-cpu.jsonl,startup-sample.txt,supervisor.jsonl,pe-final.md}; traces/app-d3d9-stkcop-tail-h368b/{analysis.json,cpu.jsonl,sample-2000.txt,supervisor.jsonl}; experiments/output/app-d3d9-stkcop-bench-tail-h368/{result.json,dxmt9.log}; experiments/output/app-d3d9-stkcop-bench-tail-h368b/{result.json,dxmt9.log}
related: present-pacing-tail-supply-audio.367.md; present-pacing-coalesce-query-reuse.366.md
---

# H368: Resolve the repeated PC before changing dxmt9 scheduling

Two Sikarugir catalogue runs pass with zero reported GPU errors. Their staged
Unix SHA prefix is `829f3d304b314d38`, PE32 `4d30258d6e7d0c3e`, PE64
`cae268981dc6cc85`, unchanged from H366/H367. HEAD at measurement is
`f29323a6`; no runtime code or scheduling policy changes are made.

## Repeated-PC ownership

H368b disables the invasive PE sampler and takes a three-second, 10ms-interval
macOS `sample` of the unique game PID 99078, triggered at observed frame 2002.
Seven unnamed threads, TIDs `12920225` through `12920231`, each have 117
recorded stack observations. Between 105 and 111 observations per thread
(89.7–94.9%) contain this chain:

```text
NtYieldExecution (ntdll.so)
  → cthread_yield (libsystem_pthread.dylib)
    → 0x7ff89da9d9f4
```

The final address is the repeated PC from H367. Disassembly of the installed
Sikarugir `ntdll.so` confirms `_NtYieldExecution` calls the `_sched_yield`
import (stub `0x677f6`); the sampled return offset is `+9`. This closes native
API attribution of the repeated PC, not the guest caller's identity.

Stacks repeat `__wine_syscall_dispatcher` frames and contain opaque Rosetta
PCs. Do not interpret the repeated unwind frames as real recursion or claim
that the guest caller is `xrCPU_Pipe.dll` without a guest stack/start-address
binding. The threads are distinct from the named dxmt9 encode and audio threads.

## Independent thread CPU evidence

After the H368b sample, `dxmt9-thread-cpu` reads two cumulative OS CPU snapshots
about 8.18 seconds apart. Each delta retains PID, native TID, process start
identity and POSIX monotonic endpoints; different thread endpoints vary slightly.

| Selected threads | Core-equivalent CPU rate |
|---|---:|
| Seven yield-heavy threads individually | 0.434–0.458 |
| Seven threads combined | 3.130 |
| X-RAY Primary | 0.553 |
| dxmt9-encode | 0.227 |

These are measured CPU deltas, not stack-sample percentages. The intervals
follow the stack sample rather than exactly coinciding with it. They establish
substantial execution by the same threads, but do not price every CPU cycle as
yield overhead or prove that removing those cycles would recover a specific FPS.

In the separate invasive H368 run, seven analogous threads consume 2.474 core
equivalents over about 25.16 seconds, while the primary consumes 0.839. Its
startup sample sees those threads in `NtDelayExecution` rather than the later
yield chain. Thread IDs differ across runs and are not interchangeable.
Neither CPU snapshot window has an exact scene/Present join.

## Primary-thread module evidence

H368 enables the existing 100Hz Win32 stopped-PC sampler. Module-map probe
passes, target mismatch is absent, and cumulative suspend/context/resume
failures remain zero through Present 2460. This is not an FPS lane or an
on-CPU profiler: a stopped PC can describe a blocked caller as well as execution.

Difference cumulative module counters only for modules present at both
endpoints; the top-20 report omits small modules, so keep an unassigned residual.

| Stopped-PC share | Presents 1800–1980 | 2220–2400 | 2400–2460 |
|---|---:|---:|---:|
| Samples | 917 | 1875 | 762 |
| Wine `ntdll.dll` | 13.96% | 24.48% | 47.11% |
| PE `d3d9.dll` | 10.25% | 8.80% | 6.43% |
| `winemetal_dxmt9.dll` | 14.72% | 12.69% | 9.19% |
| `xrCDB.dll` | 9.27% | 18.29% | 12.20% |
| `xrRender_R2.dll` | 17.45% | 14.35% | 10.24% |
| Unassigned sample count | 6 | 4 | 2 |

The late rise is in ntdll stopped PCs, not PE recorder share. However, ntdll
has not been split into specific wait/yield/syscall sites for the primary
thread. Bridge samples likewise do not separate synchronous Unix work from
waiting. Do not multiply these shares by H367 CPU budgets from another run.
The report's self-PC top-32 histogram is partial and cannot support complete
PE function attribution. Whole-run totals include expensive startup; the
interior differences above avoid conflating startup with the scene.

## Qualification and next gate

- H368 ends at frame 2511 before its planned frame-2600 native sample; that
  sample was not collected. H368b collects frame-2002's sample but ends at
  frame 2413 before the second planned frame-2500 sample. Missing samples are
  not silently substituted. Frame counts vary with the time-based benchmark
  and observer cost; they do not align scenes across runs.
- No system-wide trace is used, avoiding H367's large invalid/gapped export.
  New supervisors use POSIX monotonic timestamps. These are observation-time
  receipts, not exact source identity joins.
- Audio interruption has no user-qualified timestamp in these runs. Low audio
  CPU consumption would not disprove deadline misses. No audible-event cause
  or FPS improvement is claimed.
- Diagnostic artifacts and failure evidence are retained. No build or test
  workload ran concurrently with the game.

The next target is **the guest owner and contract of the seven yield loops**,
together with the primary thread's late ntdll wait site. Obtain bounded guest
thread start/caller evidence and determine whether these are idle worker
polls, barrier waits, or useful work interleaved with yielding. Only then test
a supported worker-count/wait-policy change in an isolated comparison. Do not
globally replace Wine yield with sleep, change affinity, or introduce encode
parallelism based on this observation alone: progress, latency and audio
effects remain unproven.
