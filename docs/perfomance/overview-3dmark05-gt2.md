---
domain: root
workload: 3DMark05 GT2
title: "3DMark05 GT2 Performance — Current Baseline"
type: root-overview
status: current
updated: 2026-07-19
source: experiments/output/app-d3d9-3dmark05-managed-versioned-gt2-r{1,2,4}-20260719; experiments/output/app-d3d9-3dmark05-managed-generation-hash-gt2-r1-20260719; experiments/output/app-d3d9-3dmark05-managed-incremental-hash-gt2-r{1,2,3}-20260719; experiments/output/app-d3d9-3dmark05-managed-incremental-hash-default-gt2-r1-20260719; experiments/output/app-d3d9-3dmark05-managed-{direct-cbuf,preacquire}-gt2-r1-20260719; experiments/output/app-d3d9-3dmark05-gt{1,2}-phase-latency{1,-control}-r1-20260719; experiments/output/app-d3d9-3dmark05-gt2-phase-latency2-r1-20260719; experiments/output/app-d3d9-3dmark05-gt2-immediate-default-latency-r1-20260719; traces/app-d3d9-3dmark05-{managed-versioned-gt2,gt2-phase-latency1}-systemtrace-20260719
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

### Phase-Aligned Drawable Lifecycle

The same trace resolves the presenter holder by joining
`ca-client-buffer-wait-interval`, `ca-client-present-request`,
`metal-command-buffer-completed`, `ca-client-presented-handler`,
`display-vsyncs-interval`, `display-surface-swap`, and
`displayed-surfaces-interval` on the shared trace clock:

| Relation | Phase-aligned evidence |
|---|---|
| Drawable pool | `146` CoreAnimation client-buffer waits occur on `dxmt9-encode`; wait p50/p95 is `77.234/103.596ms`. The `147` present requests rotate exactly three surface ids (`0x68`, `0x1e`, `0x6c`), `49` requests each, matching the runtime's three-drawable layer configuration. |
| Pool release | After excluding the initial trace-edge wait, the nearest present-bearing Metal command-buffer completion for all `145/145` waits belongs to request `N-2`. That completion precedes the `nextDrawable` return by p50 `1.690ms`; the return precedes present request `N` by p50 `2.008ms`. |
| Presented handler | The same-request CoreAnimation presented handler arrives only `10.537ms` after the request at p50. It is not the pool-release signal: the drawable remains unavailable until the later `N-2` present-bearing command buffer completes. |
| Display cadence | The full run requests immediate presentation for `523/523` presents with zero minimum duration. After a drawable wait ends, the next display VSync is p50 `7.733ms` away and spans the full `0.014-16.654ms` phase range; the next physical display swap is p50 `30.058ms` later. The wait therefore does not end on a fixed VSync boundary. |
| Downstream backlog | Present request to present-bearing command-buffer completion is p50 `274.788ms`; the trace's app CPU-to-display latency is p50 `307.194ms`. The drawable pool is applying backpressure from the older present/GPU/driver chain, not creating independent work. |

The proximate stall is therefore **three-deep drawable-pool exhaustion**. Its
observed release gate is retirement/completion of the present-bearing command
buffer from two requests earlier, not the next compositor VSync and not the
early CoreAnimation presented handler. The compositor and driver may still
lengthen that older command buffer's retirement, but scanout cadence is
downstream of the measured `nextDrawable` unblock.

The present-bearing command-buffer interval identifies which part of that
older chain is actually long. For `144` phase-complete baseline requests,
request-to-first-GPU-work is p50 `235.871ms`, the present command buffer's GPU
envelope is `35.103ms`, and last-GPU-work-to-completion is only `0.205ms`.
About `86%` of request-to-completion therefore precedes GPU execution of the
present-bearing command buffer. The driver/completion retirement tail is not
the source of the `274.788ms` interval; deep queued GPU work is.

#### Frame-Latency Discriminator and Default Policy

A phase-aligned trace with `DXMT9_MAX_FRAME_LATENCY=1` provides the causal
discriminator. It keeps the renderer, command-buffer splitting, drawable
count, and Immediate presentation mode unchanged while moving the existing
present-completion boundary from four incomplete presents to one:

| Phase metric (p50) | Default maximum `4` | Maximum `1` | Change |
|---|---:|---:|---:|
| CoreAnimation drawable waits | `146` | `0` | eliminated |
| present-CB creation → present request | `96.615ms` | `9.756ms` | `-89.9%` |
| present request → first GPU work | `235.871ms` | `94.256ms` | `-60.0%` |
| present-CB GPU envelope | `35.103ms` | `26.192ms` | phase-dependent; not used as the policy claim |
| last GPU work → command-buffer completion | `0.205ms` | `0.197ms` | unchanged |
| present request → command-buffer completion | `274.788ms` | `125.048ms` | `-54.5%` |
| app CPU present → displayed surface | `307.194ms` | `155.734ms` | `-49.3%` |

The unchanged `~0.2ms` retirement tail and halved CPU-to-display backlog rule
out compositor cadence and command-buffer completion publication as the
primary owner. Latency one prevents frames from being published far ahead of
GPU execution. The remaining request-to-GPU interval is principally the
current frame's own earlier GPU work, which must precede its tail present.

The full-scene, frame-sampled control also shows that this is a latency and
backpressure improvement rather than a throughput trade:

| GT2 configuration | Sampled FPS | Acquire p50 | Acquire total | GPU CB p50 |
|---|---:|---:|---:|---:|
| engine default (`4`) | `7.540` | `65.851ms` | `38.143s` | `24.409ms` |
| `DXMT9_MAX_FRAME_LATENCY=2` | `7.570` | `22.067ms` | `15.542s` | `25.344ms` |
| `DXMT9_MAX_FRAME_LATENCY=1` | `7.584` | `0.108ms` | `0.060s` | `24.915ms` |
| promoted Immediate default, no latency env | `7.843` | `0.108ms` | `0.061s` | `24.412ms` |

The three pre-policy discriminator runs completed the same `~67.3s` scene with
zero GPU errors. The latency-one FPS delta is `+0.58%`, inside run noise. The
blocked time moves from encode-thread `nextDrawable` acquisition to the
explicit app-side present-ordinal completion boundary; it is not hidden or
converted into more GPU work. A GT1 guard is likewise neutral within noise
(`20.476 -> 20.294` sampled FPS, `-0.89%`) while acquire p50 falls
`22.641 -> 0.092ms`. The
existing quiet-desktop SFIV discriminator also reports `1,500 -> 1,500`
presents and the same GPU-time cluster at latency one.

The post-change no-override production run confirms that the promoted default
reaches the same mechanism: `528` Immediate presents, `527` positive frame
samples over `67.198s`, zero GPU command-buffer errors, acquire p50/p95
`0.108/0.132ms`, and `31.192s` in the explicit ordinal boundary. Its
`7.843` sampled FPS is inside the current promotion-set range.

The production policy therefore uses an effective one-frame boundary for an
Immediate present only when the device still carries the engine-default
maximum of four. Synchronized presents retain four, and a non-default
application or `DXMT9_MAX_FRAME_LATENCY` value remains authoritative. Both
the inline seqId boundary and commit-replay present-ordinal boundary use the
same pure resolver, so offload does not change the policy. The public
maximum-frame-latency value remains four; the scheduler is allowed to keep
fewer than that maximum incomplete.

This also explains both negative scouts. Direct constant-buffer binding reaches
the same saturated pool sooner, converting most of the draw-encode reduction
into a longer acquire wait. Pre-acquire moves the wait to another thread but
cannot make the `N-2` completion release the pool earlier. The GT1 residual is
different: its current acquire wait was about `0.1ms/present`, while its
remaining wall was attributed to the app's Rosetta guest CPU and Wine thunking
([GT1 frame sampling](present-pacing/present-pacing-frame-sampling-current.39.md),
[GT1 producer attribution](present-pacing/present-pacing-postcache-resample.199.md)).

GPU work is still a secondary optimization axis. The normal-run median is
about `28.3ms` of GPU command-buffer time, `17.76` passes, and `232.6MiB` of
tile preservation per present. In the aligned trace, Metal stage attribution
is `93.53%` vertex and the largest groups are opaque-depth indexed (`65.15%`)
and alpha-blend indexed (`22.64%`). These stage sums are attribution totals,
not additive frame-wall time, but they prioritize vertex volume and
RT/clear-driven pass traffic over fragment shading.

The practical order after the Immediate-default policy is:

1. Reduce the remaining current-frame request-to-GPU interval by reducing GPU
   vertex volume and RT/clear pass preservation without adding
   command-buffer or tile-flush churn.
2. Revisit the residual snapshot (`10.2ms/present`) and argument-buffer CPU only with an
   A/B that also reduces the encode-lane total or frame wall time.
3. Treat acquire relocation and compositor-cadence tuning as diagnostics, not
   throughput candidates, unless a new trace shows the `~0.2ms` retirement
   tail or physical display cadence has become the binding stage.

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
