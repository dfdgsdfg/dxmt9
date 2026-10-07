---
domain: present-pacing
workload: Sikarugir CX24.0.7 x86 clock calibration for STALKER Primary attribution
title: "Primary attribution clock gate: Wine thread-time APIs rejected"
type: evidence
status: clock-rejected-native-allocation-bound-prepared
updated: 2026-10-07
source: traces/stkcop-primary-clock-h452/{clock_probe.c,clock_probe.exe,clock-results.jsonl,sha256.txt,check_clock.py,clock-verdict.json,check_residency_bounds.py,bounds-check.json,inventory.py,inventory.json}
related: present-pacing-default-tail-cpu.449.md; present-pacing-default-tail-running.450.md; present-pacing-run-construction-removal.451.md; present-pacing-app-thread-attribution.313.md
---

# Wine thread-time calls cannot price Primary PE CPU

H449/H450 leave Primary's game/Wine/PE/synchronous-provider CPU split open.
Before inserting per-call CPU brackets, a standalone x86 PE probe tests the
actual Sikarugir CX24.0.7 runtime used by STALKER. It uses the existing prefix,
does not load D3D9, and changes no renderer source, runtime default, or staged
renderer image. This is clock qualification, not a STALKER performance run.

## Observed clock failure

The helper compares `GetThreadTimes(GetCurrentThread())` with an actual
`OpenThread(THREAD_QUERY_INFORMATION)` handle to the same thread. A controlled
second thread distinguishes thread CPU from process CPU. All thread-time calls
report success.

| One-second phase | Elapsed | Pseudo-handle reported CPU | Real-handle reported CPU |
|---|---:|---:|---:|
| Caller busy | 1000.006 ms | 1000 ms | 0 ms |
| Caller sleeping, no busy helper | 1003.896 ms | 0 ms | 0 ms |
| Caller sleeping, other thread busy | 1007.192 ms | 1010 ms | 0 ms |

The pseudo-handle clock charges another thread's execution to a sleeping
caller. It is therefore rejected as thread CPU. The real handle is also
rejected because it reports zero for the busy caller. `QueryThreadCycleTime`
fails with `ERROR_CALL_NOT_IMPLEMENTED` (120). Three batches of 1,000 successful
thread-time queries take 7.329–7.383 microseconds per call elapsed; this is
observer wall overhead, not a CPU measurement.

A prior console-only calibration had the same qualitative result. The retained
JSONL above is the independently repeated, archived run. The reducer checks
all phase outcomes and query counts. `sha256.txt` binds the helper source,
executable, actual Wine executable, and native ntdll image.

Do not use these APIs to subtract synchronous Unix CPU from PE entry scopes,
or multiply guest-PC populations by their counters. H449's calibrated native
libproc counter remains the CPU authority; this result does not invalidate it.

## A bounded alternative, not yet a workload measurement

Let `W` be a matched window's elapsed time, `C` its native Primary CPU delta,
and `D` the non-overlapping wall-time union of qualified outer API scopes.
Then CPU executed inside those scopes is bounded by:

```text
max(0, C - (W - D)) <= scoped CPU <= min(C, D)
```

This is an allocation bound, not the claim that API residency is CPU time.
The discrete checker enumerates all 65,536 pairs of eight-slot CPU/residency
timelines and verifies both containment and attainability of both bounds.
It does not prove that a runtime observer measures `D` correctly.

The independent source inventory and read-only scanner agree on 350 current
normal API/forwarding bodies: 320 COM methods, six validator C-vtable methods,
12 public exports and 12 frontend shims. Declarations, `DllMain`, callback
typedefs and the diagnostic sampler worker are excluded. Nested forwarding
must share one thread-local depth and count only outermost residency.

Existing decimated timers cover only fourteen device entries (six constant
setters, four draw entries, four state setters). They neither cover the whole
API nor measure CPU. Instrumenting only that set cannot label everything
outside it as game/Wine.

A runtime bound still requires complete intended body coverage, native/guest
Primary identity matching, non-overlapping root intervals, closed nesting,
matched CPU endpoints, calibrated QPC duration, and explicit uncertainty for
function prologues/epilogues and observer perturbation. Synchronous Wine/native
work is inside an API-service scope; asynchronous Replay/Encode is not.
If the conservative interval overlaps worker CPU budgets, no largest-owner
ranking follows. This gate is prepared, not executed or accepted.

## Verdict

The convenient PE thread-time APIs are empirically unusable in the deployed
runtime. No replacement CPU attribution or removable-cost result is claimed.
The current default late scene's largest dxmt9 single-thread owner remains
unresolved. H451's small captured-body saving is unchanged; no FPS/default
promotion or README update is justified by this calibration.
