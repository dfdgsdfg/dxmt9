---
domain: root
workload: 3DMark05 GT2
title: "3DMark05 GT2 Performance — Current Baseline"
type: root-overview
status: current
updated: 2026-07-25
source: experiments/output/app-d3d9-3dmark05-managed-versioned-gt2-r{1,2,4}-20260719; experiments/output/app-d3d9-3dmark05-managed-generation-hash-gt2-r1-20260719; experiments/output/app-d3d9-3dmark05-managed-incremental-hash-gt2-r{1,2,3}-20260719; experiments/output/app-d3d9-3dmark05-managed-incremental-hash-default-gt2-r1-20260719; experiments/output/app-d3d9-3dmark05-managed-{direct-cbuf,preacquire}-gt2-r1-20260719; experiments/output/app-d3d9-3dmark05-gt{1,2}-phase-latency{1,-control}-r1-20260719; experiments/output/app-d3d9-3dmark05-gt2-phase-latency2-r1-20260719; experiments/output/app-d3d9-3dmark05-gt2-immediate-default-{latency-r1,direct-cbuf-r1,direct-cbuf-r2}-20260719; experiments/output/app-d3d9-3dmark05-managed-versioned-indexcache-{restore,direct-cbuf}-gt2-r1-20260719; experiments/output/app-d3d9-3dmark05-managed-versioned-indexcache-direct-cbuf-passaware-store-gt2-r{1,2}-20260719; experiments/output/app-d3d9-3dmark05-release-default-gt2-r1-20260725; traces/app-d3d9-3dmark05-{managed-versioned-gt2,gt2-phase-latency1}-systemtrace-20260719; docs/perfomance/index-cache-locality/index-cache-locality-scope-merge-gt2.22.md; docs/perfomance/index-cache-locality/index-cache-locality-merge-rejection.23.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.36.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.39.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.40.md; traces/app-d3d9-3dmark05-gt2-order-store-control-phasealigned-frame255-xcode-r1-20260724/analysis; traces/app-d3d9-3dmark05-gt2-passcoalesce-order-store-frame279-xcode-r1-20260724/analysis; experiments/output/app-d3d9-3dmark05-gt2-all-production-opts-r1-20260724; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.42.md
related: docs/perfomance/overview.md; docs/perfomance/overview-3dmark05-gt1.md; docs/perfomance/overview-3dmark05-gt3.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.41.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.42.md
---

# 3DMark05 GT2 Performance — Current Baseline

## Current Result

The latest post-policy stack combines V2 MANAGED backing versioning, the
restored version-aware opaque-depth index cache, direct constant-buffer
binding, and the pass-aware Store proof. Two full runs complete normally with
zero GPU errors at `8.122-8.178` sampled FPS (median `8.150`), wall p50
`105.276-106.883ms`, wall p95 `160.758-163.473ms`, and GPU-CB p50
`23.248-23.319ms`. The older three-run table below remains the promotion
baseline for MANAGED versioning plus the incremental snapshot index; the later
sections record each post-promotion addition and its isolated evidence.

The 2026-07-25 release-default spot check rebuilt commit `5dc7ca01` as
release/O3 and explicitly ran `-gt2`. Its `560` positive samples span
`67.318s`, giving `8.319` sampled FPS, wall p50/p95
`103.046/155.840ms`, and GPU-CB p50/p95 `2.971/3.405ms`. This is `2.07%`
above the closest `8.150` post-policy median and `5.73%` above the original
`7.868` promotion median. One run is insufficient to replace either repeated
baseline. The forest capture is coherent, and chunk/V2 rejects, GPU errors,
pipeline failures, missing-pipeline draws, and DCE activity are all zero.

The 2026-07-21 [extended-scope/merge gate](index-cache-locality/index-cache-locality-scope-merge-gt2.22.md)
adds a verified four-lane GT2 A/B. Extended index-cache scope leaves the
candidate/miss/create population exactly `61/61/37`, and strict adjacent
compatible indexed-draw merging eliminates zero draws. Sampled FPS is
`8.185-8.211` versus baseline `8.201` (`-0.20%` to `+0.12%`); wall and GPU-CB
percentiles are stable, all error gates pass, and closely phase-aligned
captures at frames `500-502` are visually coherent. Both experimental flags
remain default OFF, with no Xcode trace spend justified for GT2.

The follow-up [merge-rejection distribution](index-cache-locality/index-cache-locality-merge-rejection.23.md)
classifies every adjacent GT2 draw boundary with the same predicate used by
the strict merge. All `575,523` pairs are indexed, single-instance triangle
lists without UP data, but none has a single relaxable cause. The dominant
exact class simultaneously changes serialized binding payload and uses a
non-contiguous index range (`361,143`, `62.75%`); another `128,617` pairs also
change uniforms. Joined-index-only volume is zero. This rejects a standalone
joined-index-buffer merge and redirects any future work toward a mechanism
that preserves per-subdraw binding/uniform state.

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
(`15.6ms/present`). The acquisition counter identifies the CPU wait site, not
its upstream owner: the phase-aligned partition below attributes that wait to
the already-queued predecessor GPU chain rather than compositor cadence or
drawable-pool bookkeeping. Two opt-in scouts separate local CPU cost from the
current wall-time owner:

| Scout | Local effect | Sampled FPS | Verdict |
|---|---|---:|---|
| `DXMT9_ARGBUF_DIRECT_CBUF=1` | draw encode `37.6 -> 25.6ms/present`; argbuf setup becomes zero; drawable acquire rises `72.7 -> 87.5ms/present` | `7.699` | Historical pre-latency-policy result; the current-policy remeasurement below supersedes its FPS verdict. |
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

#### Latency-One GPU Partition and Current-Stack Remeasurement

The latency-one System Trace was reprocessed with
`scripts/tools/summarize_gt2_present_gpu_latency.py`. It joins `148/149`
CoreAnimation present requests to the target present command buffer, uses
unions rather than adding overlapping Vertex/Fragment channels, and identifies
exactly three current-frame predecessor command buffers for every phase-complete
row.

| Request → present-CB GPU-start component | mean | p50 | p95 |
|---|---:|---:|---:|
| total interval | `92.976ms` | `94.899ms` | `121.572ms` |
| current-frame predecessor sub-CB busy | `83.763ms` | `84.938ms` | `98.473ms` |
| prior-present CB tail | `11.925ms` | `13.663ms` | `22.952ms` |
| all dxmt9 application GPU active | `92.610ms` | `94.765ms` | `120.291ms` |
| external-only GPU active | `0.108ms` | `0.012ms` | `0.711ms` |
| global GPU idle | `0.258ms` | `0.113ms` | `1.502ms` |
| Metal driver command-buffer CPU interval | `0.296ms` | `0.266ms` | `0.486ms` |

The prior-present tail and current-frame predecessor views overlap and are not
additive. The additive result is application GPU active + external-only active
+ global idle: dxmt9 GPU work occupies `99.61%` of the request interval. The
CA request occurs only `0.389ms` after command-buffer submission ends at p50,
while GPU submission mapping precedes target GPU start by about `95ms` at p50.
This is resident queued GPU work, not a missing enqueue, driver CPU interval,
compositor cadence, or drawable-acquire bubble. Advancing publication cannot
remove more than the roughly `0.26ms` mean global-idle remainder in this phase.

Removing the old four-frame drawable saturation also changes the direct-cbuf
result. The earlier two-run set measured `+1.98%`. The 2026-07-20
[cross-workload gate](state-churn-encode/state-churn-encode-encode-phase.202.md)
supersedes that isolated estimate with a fresh same-build ABBA pair: sampled
FPS `8.087 -> 8.181` (`+1.15%`), draw/chunk CPU `-29.56%/-24.93%`, argbuf
setup/binds zero, and zero errors. GT2 is the strongest throughput result in
the four-workload set, but it is still modest and does not by itself promote
the default.

The V2 MANAGED backing work exposed a separate regression in the already
default opaque-depth index-cache lane: every draw now carries a concrete
`DrawBufferBindingSnapshot`, but the old stability gate accepted only draws
without that snapshot. Consequently the new-policy controls showed zero cache
activity. Keying the reordered result by the snapshot's explicit
`contentRevision`, while keeping the backing pinned by its draw sequence,
restores the version-safe path. The GT2 index-only run reaches `8.014` FPS with
`192,403` hits; the direct-cbuf stack reaches `8.163` FPS with `195,468` hits
and only `37` reordered buffers created. The previously captured Xcode proof
establishes that this exact locality mechanism reduces target-row VS
invocations by `14.12%`; a fresh GT2 Xcode counter capture remains desirable,
but the current runtime evidence proves that V2 no longer disables the lane.

Finally, the render-pass Store proof was found to treat later DrawRuns inside
the same Metal pass as live-out reads. The corrected portable-path proof skips
only same-attachment, exact-hazard-free DrawRuns; texture aliases, attachment
changes, helper operations, and tile-FFP stay conservative. Two full GT2 runs
are visually normal with zero GPU errors and produce:

| Metric | Before correction | Corrected range | Change |
|---|---:|---:|---:|
| depth next-clear proofs / run | `14` | `3,720-3,748` | proof now reaches the logical pass boundary |
| color next-clear proofs / run | `0` | `13` | small; most color live-out remains sampled/presented |
| tile preservation / present | `229.65MiB` | `121.15MiB` | `-47.25%` |
| sampled FPS | `8.163` | `8.122-8.178` (median `8.150`) | neutral (`-0.16%` median) |

The stable traffic reduction without a throughput response rejects
attachment Store bandwidth as the current first-order average-FPS owner.
Residual color proofs are dominated by texture sampling (`~72%`), while the
remaining request interval is still almost entirely earlier application GPU
execution. The next GPU work should therefore target vertex/tiler volume beyond
the restored opaque-depth cache, not more producer streaming. Pre-acquire also
remains a relocation-only result: it moves the wait to another thread but
cannot shorten the predecessor GPU chain.

The GT1 residual is different: its current acquire wait was about
`0.1ms/present`, while its remaining wall was attributed to the app's Rosetta
guest CPU and Wine thunking
([GT1 frame sampling](present-pacing/present-pacing-frame-sampling-current.39.md) —
`outdated: evidence-missing`,
[GT1 producer attribution](present-pacing/present-pacing-postcache-resample.199.md)).

The phase-aligned result promotes GPU execution to the primary residual axis.
The earlier normal-run median was about `28.3ms` of GPU command-buffer time,
`17.76` passes, and `232.6MiB` of tile preservation per present. In the aligned
trace, Metal stage attribution is `93.53%` vertex and the largest groups are
opaque-depth indexed (`65.15%`) and alpha-blend indexed (`22.64%`). These stage
sums are attribution totals, not additive frame-wall time, but they prioritize
vertex volume and RT/clear-driven pass traffic over fragment shading.

### Full-Frame Native Metal Replay Discriminator

The 2026-07-22 [full-frame native replay](hidden-backend-storage/hidden-backend-storage-shape.36.md)
removes 3DMark05, Wine/Rosetta, the PE/unix bridge, and dxmt CPU encoding from
the timed path by replaying the captured frame279 Metal command stream directly
in Xcode. Summary replay is `126.77ms`; encoder-counter replay is `131.678ms`,
equivalent to replay rates of `7.89-7.59 FPS`. That is the same band as the
current `~8.15 FPS` runtime and makes the emitted Metal GPU workload, not
translation CPU time, the decisive owner for this representative frame. Xcode
labels the Summary replay performance state `Medium`, so this is a
CPU-removal discriminator at the captured replay state, not an absolute
maximum-frequency M1 ceiling.

The frame issues `2,529,660` VS invocations and writes `6,952.646MiB` through
Xcode's VS buffer device-write bucket, `15.66x` the `443.895MiB` implied by its
visible `184B` VSOut. Named tiled vertex and primitive-block buffers total only
`47.750MiB`, and every one of the `19` encoders reports **zero partial
renders**. The large hidden bucket therefore cannot be described as
parameter-buffer overflow-triggered partial-render store/reload in GT2. The
open owner is per-invocation hidden VS/backend write amplification; compiler
spill versus other firmware/backend storage still needs a controlled A/B.

This result also demotes draw-boundary-preserving multi-draw work. It can reduce
CPU encode calls but preserves the VS invocation and hidden-write numerator
that owns the native replay.

### Frame279 Production Pass-Coalescing Discriminator

> Historical pre-alias-normalization result. The measured mechanism remains
> useful, but the `18 -> 15` topology is not parity-safe and cannot support
> promotion. See the alias-hazard correction below.

Xcode identified frame279 encoder `0` as the first of four encoders that could
be coalesced. The matching source encoders are `0`, `2`, `4`, and `11`: all
write the same color/depth attachment pair, while the intervening passes clear
and draw to other offscreen targets. The frame-scoped DAG scout confirms that
the whole sequence is in one v2 chunk and changes `19 -> 16` total passes
(`18 -> 15` render passes) under `passcoalesce`.

The default-off production experiment keeps the existing v2 `encodeChunk`
implementation and changes only its complete source-command permutation. It
retains Clear/helper/Present records, rejects incomplete or duplicate plans,
rejects a merge when the second pass starts with a Clear/helper boundary, and
falls back to source order for an open encode session or injected command
buffer. The diagnostic run proves that frame279 consumed all `818` commands
through the reordered tape. Runtime encoder breakdown reports `18 -> 15`
encoders and combines the four Xcode-identified encoders into one
`1,323`-draw encoder. The captured GT2 image is coherent and
`gpu_command_buffer_errors=0`.

The clean performance gate used the same build in ABBA order, with DAG export
and command tracing disabled:

| Run order | Mode | Sampled FPS | wall p50 | passes / present | encode CPU / present | frame279 encoders |
|---|---|---:|---:|---:|---:|---:|
| A1 | traditional / strict | `7.955` | `110.600ms` | `17.744` | `24.614ms` | `18` |
| B1 | framegraph / progressive + passcoalesce | `8.218` | `104.572ms` | `14.789` | `22.628ms` | `15` |
| B2 | framegraph / progressive + passcoalesce | `8.269` | `104.749ms` | `14.790` | `22.523ms` | `15` |
| A2 | traditional / strict | `8.171` | `104.928ms` | `17.751` | `23.538ms` | `18` |

Both pairs favor the candidate (`+3.31%`, `+1.20%` sampled FPS). Pooled wall
time is effectively equal (`134.565s` control, `134.533s` candidate), so the
weighted result is `8.063 -> 8.243 FPS` (`+2.24%`). Passes per present fall
`16.67%` and encode CPU per present falls `6.23%` on the two-run mean. Both
candidate runs complete the intended GT2 scene with coherent captures and zero
GPU command-buffer errors.

The initial conservative safety fallback is visible in attachment traffic. Per
present, color load/store falls from about `9.27/57.17MiB` to
`0.35/48.36MiB`, and depth load falls from `17.87MiB` to `8.98MiB`, but depth
store rises from `36.84MiB` to `136.81MiB`. At this stage reordered chunks
disabled source-order Store proof, so every affected depth pass stored. The
candidate won despite that cost and established order-aware Store proof as the
next pass-coalescing optimization.

Historical verdict: pass coalescing exposed a real GT2 pass-boundary mechanism,
but this specific candidate reordered a texture consumer before its surface
producer. Its performance deltas must be re-measured after alias-aware hazard
normalization. It is not promotion evidence.

Evidence:

- mechanism/DAG:
  `experiments/output/app-d3d9-3dmark05-gt2-passcoalesce-{chunk-scope,clear-shape,v2-tape}-r1-20260723`
  and
  `traces/app-d3d9-3dmark05-gt2-passcoalesce-v2-tape-r1-20260723/analysis`
- clean controls:
  `experiments/output/app-d3d9-3dmark05-gt2-passcoalesce-clean-control-r{1,2}-20260723`
- clean candidates:
  `experiments/output/app-d3d9-3dmark05-gt2-passcoalesce-clean-candidate-r{1,2}-20260723`

### Order-aware Store proof follow-up

> Historical pre-alias-normalization performance result. Store proof itself
> remains order-aware, but the command permutation it followed was built from
> an incomplete surface/texture hazard graph.

The follow-up makes Store proof consume the same complete, duplicate-free
command permutation as v2 replay. `encodeChunk` builds an inverse
source-command-to-replay-ordinal map while validating the permutation. A pass
opening on source command `i` scans the replay suffix after `i`'s ordinal.
Compatible DrawRuns retained in the active Metal pass are skipped as before;
the first later clear, draw/read, sample, helper operation, or present is
classified in actual replay order. Out-of-range proof input returns
`BlockNoLookahead` and therefore keeps `Store`.

The device-free render-pass-actions fixture covers both color and depth:
source order sees a clear, the synthetic optimized order first sees a
same-target draw and blocks without active-pass context, active-pass context
skips that draw and reaches the clear at replay distance two, and an invalid
replay index is defensive. The arm64 fixture, renderer/framegraph regression
set, and Rosetta provider build pass.

The same GT2 recipe then ran in B-A-B-A order: Sikarugir Wine,
`-gt2 -nosplash -nosysteminfo -noscreens`, 120-second no-gputrace timeout,
frame sampling, frontmost supervision, encoder breakdown, and the default
offload/index-cache policy.

| Run order | Mode | Sampled FPS | wall p50 / p95 | passes / present | encode CPU / present | color / depth Store per present |
|---|---|---:|---:|---:|---:|---:|
| B1 | framegraph / progressive + passcoalesce | `8.301` | `104.852 / 158.682ms` | `14.791` | `23.482ms` | `48.290 / 27.950MiB` |
| A1 | traditional / strict | `7.944` | `110.905 / 162.327ms` | `17.744` | `24.892ms` | `57.162 / 36.836MiB` |
| B2 | framegraph / progressive + passcoalesce | `8.329` | `103.274 / 159.040ms` | `14.791` | `22.960ms` | `48.291 / 27.950MiB` |
| A2 | traditional / strict | `8.147` | `105.903 / 160.993ms` | `17.750` | `23.984ms` | `57.174 / 36.840MiB` |

Pooled wall time is phase-balanced (`134.454s` candidate, `134.478s`
control). Pooled sampled FPS is `8.046 -> 8.315` (`+3.35%`), while passes per
present fall `16.66%`. Relative to the earlier conservative passcoalesce run,
depth Store falls `136.81 -> 27.95MiB/present` (`-79.57%`). Relative to the
same-build traditional control it falls `36.84 -> 27.95MiB/present`
(`-24.13%`); total color-plus-depth Store falls about
`94.00 -> 76.24MiB/present` (`-18.89%`).

The two candidates record `3,804` and `3,818`
`render_pass_depth_proof_allow_next_clear` decisions, zero
`render_pass_depth_proof_block_no_lookahead`, and zero GPU command-buffer
errors. All four runs complete normally. Candidate/control captures at frames
`467-504` show the same coherent glowing-tree scene, including bloom.

Evidence:

- controls:
  `experiments/output/app-d3d9-3dmark05-passcoalesce-order-store-control-r{1,2}-20260723`
- candidates:
  `experiments/output/app-d3d9-3dmark05-passcoalesce-order-store-candidate-r{1,2}-20260723`

### Phase-aligned Xcode counter follow-up

> Historical pre-alias-normalization capture. Per-encoder workload counters
> remain useful shape evidence; the whole-frame replay delta is not a valid
> parity comparison because the consumer ran before its producer.

The 2026-07-24 Xcode follow-up compares the order-aware `passcoalesce`
frame279 capture with a same-build strict control selected from the same
dark-forest/glowing-tree phase. The historical frame279 capture was rejected
because it had `23.7%` fewer draws and `17.1%` fewer vertices than the new
candidate. A same-build strict frame231 capture was also rejected because its
bright-forest image was an earlier visual phase. Strict frame255 is the closest
usable control: its overlay time is `30.42s` versus `32.10s` for the candidate,
and its draw, vertex, and primitive counts differ by at most `3.76%`.

| Whole-frame metric | Strict frame255 | Passcoalesce frame279 | Delta |
|---|---:|---:|---:|
| Xcode encoders / renderer encoders | `19 / 18` | `16 / 15` | `-15.79% / -16.67%` |
| DXMT draws | `2,238` | `2,175` | `-2.82%` |
| Xcode vertices / primitives | `6,625,397 / 2,212,091` | `6,376,127 / 2,129,001` | `-3.76% / -3.76%` |
| Xcode GPU replay | `172.670ms` | `149.701ms` | `-13.30%` |
| GPU time / vertex | `26.062ns` | `23.478ns` | `-9.91%` |
| GPU time / primitive | `78.057ns` | `70.315ns` | `-9.92%` |
| VS invocations / vertex | `0.4705` | `0.4725` | `+0.43%` |
| VS buffer write / vertex | `1,453.3B` | `1,440.4B` | `-0.89%` |
| attachment Load | `27.293MiB` | `9.293MiB` | `-65.95%` |
| attachment Store | `94.629MiB` | `76.629MiB` | `-19.02%` |
| attachment Load + Store | `121.922MiB` | `85.922MiB` | `-29.53%` |
| partial renders | `0` | `0` | unchanged |

The raw replay gain is larger than the workload-normalized gain because the
candidate frame has `3.76%` fewer vertices and primitives. Even after that
correction, GPU cost per vertex and primitive falls about `9.9%`. VS
invocations per vertex and hidden VS write per vertex are effectively
unchanged, so this is not a vertex-work elimination result. The measured
mechanism is fewer pass boundaries and less attachment preservation, with no
partial-render spill introduced. Xcode's top-three comparison is not valid
across this transformation because coalescing changes encoder boundaries: the
candidate top three contain `94.02%` of frame GPU time versus `63.36%` for the
control. Whole-frame totals are the comparison denominator.

This single replay pair retains a `1.68s` visual-phase offset, so the normalized
`~9.9%` result is directional mechanism evidence rather than a standalone
promotion number. Alias-liveness later invalidated the underlying command
permutation, so neither this replay nor the phase-balanced `+3.35%` runtime
result remains a policy gate. Both must be repeated on the alias-aware build.

Evidence:

- strict control:
  `traces/app-d3d9-3dmark05-gt2-order-store-control-phasealigned-frame255-xcode-r1-20260724/analysis/frame255-{counters-xcode.csv,xcode-dxmt-bottleneck-report.md}`
- passcoalesce candidate:
  `traces/app-d3d9-3dmark05-gt2-passcoalesce-order-store-frame279-xcode-r1-20260724/analysis/frame279-{counters-xcode.csv,xcode-dxmt-bottleneck-report.md}`
- normalized whole-frame comparison:
  `traces/app-d3d9-3dmark05-gt2-passcoalesce-order-store-frame279-xcode-r1-20260724/analysis/frame279-vs-control-frame255-phasealigned-normalized.md`

### Black-draw / hidden-invocation discriminator

The many black draw previews visible near the start of frame279 encoder 0 are
not failed fragment shading. Xcode inspection of
`Draw[seq=279,prim=2376]` shows Color 0 remaining black while the depth
attachment receives the selected geometry; the bound depth state is
`LessEqual, Write Yes`. Same-run route telemetry identifies exactly `120`
such draws, all with `color_write=0`, depth write on, alpha blend/test off,
and `VSOut=0xfff`. They occur in early encoder-draw blocks `0..8`,
`75..180`, and `279..283`.

The class contains `94,980` primitives and `284,940` submitted vertices:
`4.46%` and `4.47%` of the full frame. It is therefore a real pre-Z route, but
not a credible explanation for the full `149.701ms` replay or `8,758.891MiB`
VS-write bucket. The existing indexed-state Xcode proxy must not be used as
class attribution: it distributes encoder 0's whole counters over only
`126/1,329` represented draws, of which `120` are this class, and consequently
overstates the black route.

The similarly sized GT1 `60/0` depth-only experiment already supplies the
controlled mechanism check. Removing its fragment function while preserving
`VSOut=0xfff` passed depth/color equality but left target VS invocations
unchanged at `152,895` and VS write flat at
`224.918 -> 224.944MiB`. Position-only `VSOut=0x0` changed depth and is not a
legal route. The GT2 black draws therefore do not justify another
fragmentless-only capture. The remaining material branch is invocation or
backend-write reduction in the dominant programmable textured/color work.
See [hidden-backend-storage-shape.37](hidden-backend-storage/hidden-backend-storage-shape.37.md).

### R32F alpha-test index-locality discriminator

The two dominant frame279 R32F passes each spend `23.789-26.359ms`, issue
`486,280` VS invocations, and write about `1,535MiB` through Xcode's VS buffer
bucket. Each pass has `367` indexed draws; `229` of them are alpha-tested and
therefore outside the production opaque-depth LRU32 selector.

A candidate-only GT2 scout measured the exact matching `367`-draw R32F shape
without submitting reordered indices. The `160` alpha-tested draws for which a
candidate was built move LRU32 misses only `238,571 -> 238,484`
(`-0.0365%`), and none pass the `10%` gate. The other `69` draws already have
`miss32 == unique`, so the upper-bound gate correctly avoids candidate
construction. The non-alpha control in the same pass moves
`174,601 -> 104,743` (`-40.01%`) with `125` gate passes, reproducing the
existing production optimization.

The R32F result is a sampled shadow/depth input, not disposable black output:
the next main-color pass binds alias `0x20000010000003e` at texture stage 0 for
`280` indexed draws. Despite that semantic risk, no mutation proof is needed
because there is no useful performance numerator. Per-draw effective locality
predicts `486,697` LRU64 misses per pass, within `0.086%` of Xcode's `486,280`
VS invocations. Reaching the draw-local unique-index floor in both passes would
save at most `16,832` invocations, only `0.56%` of the whole frame.

Do not add alpha-test eligibility, run an unsafe reorder, or spend another
gputrace on this branch. The remaining R32F cost is per required transformed
vertex or higher-level submitted geometry, not missed index reorder coverage.
See [hidden-backend-storage-shape.38](hidden-backend-storage/hidden-backend-storage-shape.38.md).

### R32F subresource liveness and alias-hazard correction

A source-order texture trace resolves the two dominant passes beyond the
surface-handle level. Alias `0x20000010000003e` is a `2048x2048`, one-level,
one-slice `TwoD R32F` texture, and every observed surface maps to
`subresource=0, mip=0, slice=0`. Frame279 executes:

1. `418` writes to the first R32F surface.
2. A main-color interval samples the owning texture.
3. `418` writes to the second R32F surface.

The first pass is therefore mandatory. The final pass has no later sample
before the next frame clears/writes the same subresource and is a strong
higher-level DCE candidate, potentially much larger than the closed
sub-`0.6%` index-locality branch. It is not yet droppable: the current graph is
per chunk and cannot prove the future-chunk overwrite, and query/readback
protections must remain conservative.

The investigation also found that the pre-fix framegraph recorded attachment
writes under surface handles and shader reads under the owning texture handle.
No RAW/WAW/WAR edge joined the chain, so passcoalesce moved the merged main
consumer before both R32F writers. The implementation now canonicalizes
aliased surface accesses to the owning texture for hazards while retaining
exact surface handles in `AttachmentSet`.

Fixed frame279 validation records:

| Stage | Render passes | Canonical R32F order |
|---|---:|---|
| pre-opt | `18` | pass `1` Clear -> pass `2` Read x`134` -> pass `3` Clear |
| post-opt | `16` | pass `0` Clear -> pass `1` Read x`134` -> pass `2` Clear |

The actual encoder order is `R32F 367 draws -> main 585 -> R32F 367`; all three
RAW/WAW/WAR edges are present. This supersedes the old `18 -> 15` result.
The alias-aware all-production-options GT2 rerun completes with no observed GPU
or pipeline failure and cuts render-pass/present volume by about `11.2%`.
Together with the clean GT1/GT3 runs and exact GT3 glitch-window captures, this
clears the wild gate for promoting only the passcoalesce L1 policy. Because the
run also carried unrelated experimental options, it is not a passcoalesce-only
FPS A/B. Device-backed pixel parity remains open. See
[hidden-backend-storage-shape.40](hidden-backend-storage/hidden-backend-storage-shape.40.md).

A whole-run closure now strengthens the final-writer result beyond frame279.
All `503` measured target frames have two R32F write runs and no read-first
frame. Current-default DAGs for frames `278..280` repeat
`R32F Clear -> main Read x133 -> final R32F Clear`; the next frame Clears
before reading, and the final pass's shared depth is also cleared immediately
by the following pass. Query/readback counters are zero. This makes the final
R32F pass an accepted large DCE target, but not a current optimization:
ready-depth is exactly one in all `531` samples, so neither per-chunk DCE nor
ready-only batching can see the overwrite. A safe prototype needs a fail-open
cross-chunk scheduling window and TLA+ coverage. See
[hidden-backend-storage-shape.41](hidden-backend-storage/hidden-backend-storage-shape.41.md).

### Cross-chunk DCE implementation and GT2 gate

The bounded cross-chunk prototype confirms the liveness result in the real
renderer. With an already-available successor proof, the corrected
passcoalesce-order prefix drops `389` passes and `56,283` commands and lowers
GPU-CB p50 `2.886 -> 1.064ms`. Waiting for that proof is not viable:
instantaneous FPS falls `9.157 -> 6.902` (`-24.6%`) and wall p50 grows
`106.731 -> 142.400ms` because the cheap prefix no longer overlaps producer
publication.

The accepted implementation therefore never waits. It encodes only a
proof-independent prefix, samples the ready FIFO once, and immediately
fail-opens when the successor is absent. A bootstrap GT2 run exposes `498`
prefixes but finds only one already-ready successor in `536` frames, dropping
one pass and `30` commands with zero GPU or proof-validation errors. This is a
safe opt-in mechanism, not a GT2 performance win, so `dce` remains outside the
default feature set. See
[hidden-backend-storage-shape.42](hidden-backend-storage/hidden-backend-storage-shape.42.md).

The practical order after the Immediate-default policy is:

1. Treat cross-chunk DCE as a completed, rejected GT2 default candidate. Reopen
   it only for a workload or scheduling design with meaningful already-ready
   successor volume; do not add an encode-lane wait to recover proof coverage.
2. Treat the alias-aware `18 -> 16` passcoalesce topology as the default L1
   policy. The old `+3.35%` pooled and `~9.9%` workload-normalized results
   crossed a missing producer/consumer edge and remain historical only; close
   device-backed pixel parity before promoting another framegraph feature.
3. Revisit the residual snapshot (`10.2ms/present`) and argument-buffer CPU only with an
   A/B that also reduces the encode-lane total or frame wall time.
4. Treat acquire relocation and compositor-cadence tuning as diagnostics, not
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
