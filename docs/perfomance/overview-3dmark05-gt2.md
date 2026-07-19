---
domain: root
workload: 3DMark05 GT2
title: "3DMark05 GT2 Performance — Current Baseline"
type: root-overview
status: current
updated: 2026-07-19
source: experiments/output/app-d3d9-3dmark05-managed-versioned-gt2-r{1,2,4}-20260719; experiments/output/app-d3d9-3dmark05-managed-generation-hash-gt2-r1-20260719; experiments/output/app-d3d9-3dmark05-managed-incremental-hash-gt2-r{1,2,3}-20260719; experiments/output/app-d3d9-3dmark05-managed-incremental-hash-default-gt2-r1-20260719; experiments/output/app-d3d9-3dmark05-managed-{direct-cbuf,preacquire}-gt2-r1-20260719; traces/app-d3d9-3dmark05-managed-versioned-gt2-systemtrace-20260719
related: docs/perfomance/overview.md; docs/perfomance/overview-3dmark05-gt1.md; docs/perfomance/overview-3dmark05-gt3.md
---

# 3DMark05 GT2 Performance — Current Baseline

## Current Result

Three completed current-runtime runs used the V2-only command wire with
MANAGED buffer backing versioning and the default incremental shader-constant
content index,
`-gt2 -nosplash -nosysteminfo -noscreens`, no Metal frame capture, frame
sampling, frontmost supervision, and the engine-default offload plus
opaque-depth index-cache policy. All three captures show the expected forest
scene, every run completed the GT2 sequence, and all GPU error counters are
zero. The promotion set is `managed-incremental-hash-gt2-r1..r3`; a separate
no-environment-variable default confirmation also completed with the expected
scene, zero errors, `7.847` sampled FPS, and `9.913ms/present` snapshot CPU.

| Metric | Run median | Run range |
|---|---:|---:|
| sampled average FPS | `7.868` | `7.841-7.889` |
| sampled frames | `527` | `526-529` |
| sampled wall time | `67.058s` | `66.984-67.083s` |
| wall p50 | `113.041ms` | `111.075-113.221ms` |
| wall p95 | `166.424ms` | `160.753-169.251ms` |
| GPU CB p50 | `24.514ms` | `23.943-24.562ms` |
| GPU CB p95 | `40.755ms` | `40.212-41.011ms` |
| encoded presents | `528` | `527-530` |

The sampled average varies by `0.61%` from the slowest to fastest promotion
run. Relative to the pre-index MANAGED median, sampled FPS improves `0.72%`
(`7.812 -> 7.868`) while wall p95 is unchanged within run noise
(`166.408 -> 166.424ms`). Relative to the earlier `current-v2` median, the
combined MANAGED and snapshot work improves sampled FPS `5.92%`
(`7.428 -> 7.868`). `map_buffer_wait_ms`, queue-sequence wait, and map-wait
chunk publication remain zero.

The versioned MANAGED path performs about `1,222` uploads and `8.9 GB` of
full-shadow publication per run. Its median backing choices are `163`
idle-active, `904` completed-backing reuse, and `156` fresh allocations. The
traffic is a follow-up bandwidth watchpoint, but it no longer blocks writable
locks on GPU completion.

The snapshot cache now updates a per-register content index from effective
constant setter ranges and hashes only the shader-visible prefix. Broad state
mutation invalidates and lazily rebuilds the index. Across the promotion runs,
the median effects versus the pre-index MANAGED run are:

| Metric | Pre-index | Incremental index | Change |
|---|---:|---:|---:|
| snapshot CPU / present | `12.164ms` | `10.248ms` | `-15.8%` |
| snapshot lookup CPU / present | `10.550ms` | `8.600ms` | `-18.5%` |
| batch-miss CPU / present | `8.558ms` | `7.000ms` | `-18.2%` |
| VS constant hash CPU / run | `1,573.5ms` | `438.1ms` | `-72.2%` |
| VS constant bytes visited / run | `1.095GB` | `445.8MB` | `-59.3%` |
| payload lookup bucket hits / run | `55,284` | `55,587` | stable |
| payload append bytes / run | `947.3MB` | `952.2MB` | `+0.5%` |

A generation-plus-usage scout reached `9.421ms/present` snapshot CPU and
visited zero constant bytes, but reduced payload bucket hits from `55,284` to
zero and increased payload append bytes `23.6%` (`947.3MB -> 1.171GB`). It was
rejected because A→B→A values could not recover content identity. The promoted
index retains content-stable identity and the exact payload comparison gate;
both scout environment variables were removed after promotion.

## Remaining Bottleneck Attribution

The next owner is not queue publication latency. At the valid-run median,
publish-to-encode-dequeue is `0.003ms/present`, dequeue-to-command-buffer
commit is `0.363ms/present`, and the no-enqueue interval is
`7.46ms/present`. More bulk publication or another present-sized streaming
layer therefore has little remaining direct leverage.

The encode lane instead spends about `115.5ms/present` end to end. Its largest
serialized components are drawable acquisition (`72.7ms/present`) and draw
encoding (`36.9ms/present`), including argument-buffer setup
(`15.6ms/present`). Two opt-in scouts separate local CPU cost from the current
wall-time owner:

| Scout | Local effect | Sampled FPS | Verdict |
|---|---|---:|---|
| `DXMT9_ARGBUF_DIRECT_CBUF=1` | draw encode `37.6 -> 25.6ms/present`; argbuf setup becomes zero; drawable acquire rises `72.7 -> 87.5ms/present` | `7.699` | Large local encode win, but `-1.45%` FPS; do not promote. |
| `DXMT9_PRESENT_PREACQUIRE=1` | `522/523` pre-acquire hits; encode lane remains `116.7ms/present` | `7.792` | `-0.26%` versus the default median; moving acquire to a worker does not shorten the critical path. |

A 20-second Metal System Trace covers seq `197..341` and joins all
`2,609/2,609` render encoders. The `dxmt9-encode` thread carries `7,758ms` of
sampled CPU weight and contains `presentDrawable=56`, `CAMetalLayer=26`, and
`nextDrawable=17` stack hits. The automatic producer verdict remains
inconclusive because the PE/native producer thread id was not logged, while
the main thread itself has only `20ms` of sampled profile weight; the trace
therefore supports an encode/presenter holder, not a proved app-main-thread
stall.

GPU work is still a secondary optimization axis. The normal-run median is
about `28.3ms` of GPU command-buffer time, `17.76` passes, and `232.6MiB` of
tile preservation per present. In the aligned trace, Metal stage attribution
is `93.53%` vertex and the largest groups are opaque-depth indexed (`65.15%`)
and alpha-blend indexed (`22.64%`). These stage sums are attribution totals,
not additive frame-wall time, but they prioritize vertex volume and
RT/clear-driven pass traffic over fragment shading.

The practical order is now:

1. Determine why the drawable pool/compositor cadence keeps the encode lane
   near `116ms/present` even when draw CPU is removed or acquire is prefetched.
2. Reduce GPU vertex volume and RT/clear pass preservation without adding
   command-buffer or tile-flush churn.
3. Revisit the residual snapshot (`10.2ms/present`) and argument-buffer CPU only with an
   A/B that also reduces the encode-lane total or frame wall time.

## V1 Comparison Status

There is no completed, frame-sampled, same-build V1 GT2 reference. The
2026-07-14 V1-era `at-immediate-gt2` diagnostics timed out after only 480
presents and used a different observation window, so they are not a valid
performance denominator. Treat this three-run V2-only result as the first
defensible GT2 baseline and require a preserved-binary bisect if a V1 delta is
ever needed.

## Measurement Rule

Use per-frame `wall_ms` for GT2. Presents-at-process-exit is invalid when a run
times out or stops in a different phase. Future candidates should repeat three
times, keep the same result/scene selection, and report sampled FPS, wall
p50/p95, GPU CB p50/p95, completion status, capture sanity, and GPU errors.
