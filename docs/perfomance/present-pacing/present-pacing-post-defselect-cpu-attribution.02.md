---
domain: present-pacing
workload: 3DMark05 GT2
subcategory: post-defselect-cpu-attribution
order: 02
title: The Producer Thread Never Blocks — GT2's Wall Is Producer CPU, Not Serialization
date: 2026-07-29
type: experiment-run
status: accepted-attribution-scout
source: traces/app-d3d9-3dmark05-gt2-native-tid-r1/analysis/xctrace-cpu-thread-verdict.json; traces/app-d3d9-3dmark05-gt2-native-tid-r1/analysis/xctrace-cpu-thread-summary.md; src/d3d9/device_c_chunk_replay.cpp
related: docs/perfomance/present-pacing/present-pacing-post-defselect-cpu-attribution.01.md; docs/perfomance/shader-codegen/shader-codegen-defselect.02.md
---

# The Producer Thread Never Blocks — GT2's Wall Is Producer CPU, Not Serialization

**Question / hypothesis.**
[attribution.01](present-pacing-post-defselect-cpu-attribution.01.md) showed
application threads own `83.7%` of burned CPU after the GPU ceiling fell, with
one thread pinned at `94.9%` of a core. But `time-profile` samples *running*
stacks, so it attributes CPU burned and cannot see time *lost* to
serialization — and at `1.34` cores busy on a multi-core machine, waiting was
the obvious alternative explanation. Does the producer block?

**Prerequisite: the tool was missing its input.**
`summarize_xctrace_cpu_threads.py` resolves the producer thread by scanning the
run log, preferring a unix-side `unix_commit_chunk_entry native_tid=0x...` line
(`:61`, `:296`) over a PE `pe_present_*` `thread_id=0x...` fallback (`:305-313`).
**Nothing in `src/` had ever emitted the preferred line**, so every run fell
through to the PE fallback, which reports a Win32 thread id that cannot match
xctrace's native Mach namespace — hence `producer-thread-not-found` in
attribution.01. `150e21d2` emits it from `dxmt9c_device_commit_chunk`, once per
thread behind a `shouldLog(Info)` guard, using `pthread_threadid_np`.

**Method.** Same sidecar as attribution.01 —
`run_3dmark05_system_trace_sidecar.sh --export-cpu-summary
--cpu-producer-from-pe-log`, GT2, `25 s` window — against a rebuilt unix
provider.

**Result.** The selector resolved through the new path.

| Field | Value |
|---|---|
| Status | `producer-state-inconclusive` |
| Selection source | `native-log-commit-chunk-entry` |
| Producer thread | `3DMark05.exe (0x17fcfc2)`, non-main |
| Profile weight | `24,928 ms` over a `25,000 ms` window |
| Sample rows | `24,936` |
| **Running** | **`24,935`** |
| **Blocked** | **`1`** |
| P4 wait keyword hits | `0` |

The emitted line appears exactly once in the run, confirming the per-thread
latch does not perturb the hot path.

**Verdict.** The producer thread is Running in `24,935` of `24,936` samples —
one blocked sample and zero wait-keyword hits. The tool reports
`producer-state-inconclusive` rather than a clean negative precisely because
that one blocked sample carries no matching wait stack, which is the honest
label: the evidence is overwhelmingly against a producer wait, but it is not a
proof of zero. GT2's residual frame time is
**producer CPU, not a serialization stall** — there is no producer-side wait for
dxmt9 to shorten. Combined with attribution.01, the picture after the codegen
fix is: GPU `18%` of frame time, a saturated producer thread that is Running
in all but one sample, and dxmt9's own threads at `16.3%` of burned CPU.

**One nuance that matters for what to do next.** The producer thread is where
the D3D9 application calls in, so its CPU is *not* purely the game's: dxmt9's
PE-side chunk recording runs on that same caller thread. This measurement
attributes the thread, not the work inside it. Sizing the dxmt9 share of that
`23.5 s` is the natural follow-up, and `DXMT9_PE_STATS_DECIMATION` exists for
exactly that — `agents/rules/environment_variables_bridge.rules.md` documents it
as the low-perturbation way to size the PE recorder core, verified at `N=64`/`16`
to stay inside the healthy presents population.

**Scope.** The tool labels this a *scout*, correctly: one `25 s` window of one
GT2 run. It is strong evidence against a producer-wait explanation, not a
measurement of how the producer's CPU divides between game and translator.

**Related.**
[attribution.01](present-pacing-post-defselect-cpu-attribution.01.md) ·
[present-pacing](index.md) ·
[shader-codegen-defselect.02](../shader-codegen/shader-codegen-defselect.02.md) ·
[overview](../overview.md)
