---
domain: present-pacing
workload: 3DMark05 GT2
subcategory: post-defselect-cpu-attribution
order: 05
title: The Producer Thread Cannot Be Split By Image — dxmt9's PE Code And The Game Are The Same Blob To xctrace
date: 2026-08-01
type: experiment-run
status: accepted-negative-result
source: traces/app-d3d9-3dmark05-gt2-producer-modules/analysis/time-profile.xml; traces/app-d3d9-3dmark05-gt2-producer-modules/analysis/xctrace-cpu-thread-summary.md
related: docs/perfomance/frame-lifecycle.md; docs/perfomance/present-pacing/present-pacing-post-defselect-cpu-attribution.01.md
---

# The Producer Thread Cannot Be Split By Image — dxmt9's PE Code And The Game Are The Same Blob To xctrace

**Question / hypothesis.** [frame-lifecycle](../frame-lifecycle.md) says dxmt9's
PE recording is `15.1%` of GT2's frame and labels it a **floor**: it is the sum
of four instrumented scopes, and the residual `~80%` is a residual, not a
measurement. The attribution that residual leans on (H212) is GT1, three weeks
stale, and its own text says the guest blob is "game x86 code **plus our 32-bit
PE d3d9.dll recording path**". So how much of the producer thread is actually
ours? Profile it and bucket self-time by image.

**Method.** Extended `summarize_xctrace_cpu_threads.py` with weighted
self-time-by-image (`top_binary_weight_ms`): it already carried a per-row weight
and a top-frame binary but bucketed the binary by *sample count*, which answers
"how often" rather than "how much". Bucketing on the **top** frame is deliberate
— that is self time, the right attribution for whose code is burning CPU rather
than who called into it. Then a GT2 Metal System Trace with
`--export-cpu-summary --cpu-producer-from-pe-log`, producer thread resolved
through the native-tid path.

**Result — the method cannot answer the question.**

The producer thread's self time is `100%` `<unknown-binary>`. Not a symbolication
gap to be tuned around: **the trace names 564 distinct images and neither the
game nor our PE `d3d9.dll` is among them.**

| present in the trace | absent |
|---|---|
| `winemetal.so`, `ntdll.so`, `winemac.so`, `wine.real`, `winebus.so` | `d3d9.dll` |
| `libRosetta.dylib`, `Translation` | `3DMark05.exe` |
| 550-odd macOS frameworks (`CoreFoundation`, `libdispatch`, …) | |

Only `53,295` of `1,065,037` frames carry an image at all — `5%` — and every
named one is a macOS framework or a Wine **unix** object. The unix side
symbolicates; the PE side does not, because both the game and dxmt9's
`d3d9.dll` execute as translated x86 PE code and appear as bare addresses.

Address-range bucketing does not rescue it either: separating the two would need
each PE image's guest load address, which the trace does not carry.

**Verdict.** ACCEPTED as a negative result. **dxmt9's PE-side share of the
producer thread is not measurable from outside the process**, and H212's "guest
blob = game + our PE d3d9.dll" was not a shortcut — it is the instrument's
ceiling. Any future attempt to tighten "dxmt9 is ≥15% of the frame" by profiling
will hit the same wall.

**What this redirects to.** The floor can only be raised **from the inside**,
by instrumenting more PE-side scopes — which is exactly what
`DXMT9_PE_STATS_DECIMATION` does, and exactly how the floor moved last time:
[append-decomposition.07](../state-churn-encode/state-churn-encode-append-decomposition.07.md)
found a `~1.9%`-of-frame item in the hot path with half of it structurally
outside the four existing scopes. The uninstrumented PE layers named in
frame-lifecycle §4.2 — `DeviceState` validate/mutate, COM dispatch, the buffer
lock/shadow path — are the next candidates, and each one converts residual into
measurement rather than estimating it.

**Scope, and a second weakness in this run.** Only `717` of `66,046`
`time-profile` rows matched the target process (`458 ms` on the producer
thread), against attribution.01's `33,588 ms` — the capture window was far
thinner, so even the thread-level numbers here are weak. That does not change
the verdict: more samples cannot create image names the trace does not record.
GT2 only, though nothing about the limitation is workload-specific.

**Related.**
[attribution.01](present-pacing-post-defselect-cpu-attribution.01.md) ·
[attribution.04](present-pacing-post-defselect-cpu-attribution.04.md) ·
[frame-lifecycle](../frame-lifecycle.md) ·
[present-pacing](index.md)
