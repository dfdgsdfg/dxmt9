---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "H401: Primitive-count query defaults versus real visibility"
type: evidence
status: semantic-workload-difference-confirmed-gap-attribution-open
updated: 2026-09-27
source: traces/stkcop-gap-h401/{run.py,analyze.py,invocation.json,summary.json,native-mid.sample.txt}; traces/stkcop-gap-h401/visibility/{invocation.json,summary.json}; experiments/output/app-d3d9-stkcop-bench-gap-h401-{query,visibility}/result.json
related: present-pacing-stalker-renderer-comparison.399.md; present-pacing-occlusion-timeline-stateblock.320.md; present-pacing-query-queue-drain-attribution.321.md
---

# H401: Completion correctness does not establish visibility correctness

The current default does **not** return real GPU passing-sample counts for
occlusion queries. `src/dxmt9/dxmt9_query_policy.hpp` / `metalOcclusionEnabled`
defaults OFF. `src/d3d9/core_resources.cpp` / `Device::issueQuery` clears a
primitive accumulator at BEGIN and resolves it at END; the accepted draw
paths in `src/d3d9/core_draw.cpp` add `draw.primitiveCount`. The immutable
polling receipt waits for the actual fence, but does not transform this
resolved value into GPU visibility. A fully occluded nonempty draw can still
produce a positive primitive count.

The read-only D9VK reference (`src/d3d9/d3d9_query.cpp` / `GetQueryData`)
instead returns the backend query's `occlusion.samplesPassed`. This is a
source-level reference, not a measured per-query transcript from the released
D9VK DLL. H399's runtime log advertises `occlusionQueryPrecise=1`; that is
capability evidence, not an actual-result census.

Consequently matched game settings need not produce equal command workloads.
This is a concrete candidate for the H399 gap, not proof of its full cause.

## Current-binary diagnostic pair

Both runs use HEAD `dbc216d5`, the same five staged artifacts as H399,
Sikarugir CX24.0.7, Day/high, 1280x720, two game threads and 16 sound targets.
Requested game config SHA-256 is identical across both and H399:
`1f17283bd2ac940ed4c813e5a186f332241d1335b2a1edbd90b7e8a93747205e`.
Counters, frame sampling and the bounded query observer are ON; Info logging,
skip first 5,000 ENDs, retain one in 31 subsequent ENDs. The second process
adds only `DXMT9_METAL_OCCLUSION=1` to renderer policy. No code/build changes.

Both exit normally, runner pass, zero sampled GPU command-buffer errors.
Automatic screenshots contain buildings/terrain, but are at different camera
positions and are not an exact or whole-sequence visual oracle. These are
diagnostic runs, not an observer-OFF performance/promotion pair. The default
run additionally has one 3-second native sample; the ON run does not.

| Observation | Default primitive count | Real visibility candidate |
|---|---:|---:|
| Unique retained timeline rows | 87 | 128 |
| Complete occlusion rows with known values | 78 | 93 |
| Zero results in that selected population | 0 | 90 |
| Positive result detail | 65 values equal 160; others 30–1,793 | 1, 4, 17,132 |
| Scene-like sampled Presents | 3,214 | 3,346 |
| Draws / scene-like Present | 2,961.88 | 2,646.40 |
| Render passes / scene-like Present | 30.65 | 30.74 |
| Command buffers / scene-like Present | 7.60 | 16.33 |
| Observed ENDs, entire timeline ledger | 7,694 | 36,365 |
| Benchmark FPS, diagnostic only | 26.047878 | 27.857220 |

Scene-like means sampled `draw_calls > 100`, not exact camera matching. The
10.65% lower draw average is a workload-shape observation, not a matched-frame
causal reduction. The number and timing of queries also change; the query
samples are not paired identities between runs. The ON ledger drops 884
summaries after filling its fixed store, versus zero in the default run.
Its 90/93 zero count therefore must not be called the full-run occlusion rate.
Deduplicate periodic rows by queue/query/issue/epoch before counting.

The legacy `[dxmt9-query-summary]` reports fallback calls only; its zero
occlusion polls do not mean the game stopped polling through receipts.
The timeline sees 1,796,319 and 2,616,748 polls respectively, mostly outside
the retained END set. Do not infer full query-kind or HRESULT rates from that
unqualified population.

## Why enabling visibility is not a complete performance fix

`src/dxmt9/dxmt9_command_queue.cpp` / `setOcclusionScope` publishes a populated
writer whenever membership changes. Real visibility also suppresses early
CPU-ready prefix publication (`publishEarlyCpuReadyPrefix`).
`src/dxmt9/dxmt9_render_pass_encoder.mm` / `prepareRenderPassWithStoreProofLookahead` preserves query
attachments provider-wide because source-local exhaustion cannot prove that a
following query no longer needs depth. H320/H321 record why these safeguards
exist. Do not remove them as an unverified speed optimization.

The observed CB average more than doubles while the pass average stays similar.
This suggests inspecting source/CB composition and query request patterns,
not assuming that every extra source necessarily causes an extra render pass.
The pair does not isolate membership cuts, changed query frequency, stores,
or observer overhead. Its small diagnostic FPS difference cannot explain the
H399 1.825x cross-renderer gap or qualify default promotion.

## Native sampling limits and useful negative evidence

`native-mid.sample.txt` samples the default diagnostic process at
20:54:00 KST for three seconds with a requested 5ms interval. It retains 139
stack samples per principal worker, not a calibrated CPU-time measurement.
Encode has 87/139 stacks under `reserveReadySlotBatchPrefix`'s condition-variable
wait; its source predicate is stop-or-nonempty Ready tape. Replay has 86/139
under `ReplayOffloadQueue::pop`'s condition-variable wait. Encode has 46 samples
in `FrameGraphBackend::onChunkReady`. This local window does not support a
continuously saturated serial encoder as the sole bottleneck.

Do not sum thread populations, extrapolate to the tail, or infer GPU busy time.
Wine/Rosetta guest stacks are incompletely resolved and the observed encode
body includes signpost overhead. A global app/PE/Unix CPU allocation or replay
speedup bound is not established by this sample.

## Next gate

1. Match camera/time windows and count actual D9VK draws/primitives and query
   outcomes, rather than comparing only renderer throughput.
2. Qualify current real-visibility output more broadly, then measure a repeated
   observer-OFF pair. Keep incorrect/missing-geometry output out of FPS claims.
3. Attribute ON-path publication/CB multiplication to query membership versus
   FLUSH, preserving result identity, depth contents and progress. Investigate
   batching only after that attribution; do not reintroduce false visibility.
4. Continue app supply/wait versus GPU occupancy investigation: real visibility
   is a demonstrated semantic difference, not the entire gap's proven owner.

Runtime defaults and README are unchanged. Environment overrides are local to
the completed child process. Raw logs, failed/limited evidence and summaries
are retained. Metal skill preflight passes (Xcode/xctrace 27, M1/Metal 4);
no GPU trace or capture layer was injected into these runs.
