# Metal Render-Pass Lifecycle and Encoding Models

This note compares three mechanisms that are easy to conflate when designing a
Metal translation layer:

1. ANGLE Metal's long-lived `ContextMtl` and encoder reuse policy.
2. `MTLParallelRenderCommandEncoder`, which records one render pass from
   multiple CPU threads.
3. Metal 4 render-pass suspend/resume, which stitches render encoders from
   multiple command buffers into one GPU render pass.

The dxmt9 question is narrower: which parts help keep a logical D3D9 render pass
continuous across queue-source boundaries without weakening D3D9 ordering,
resource lifetime, or completion semantics?

This document is comparative research, not a dxmt9 requirement. The normative
contract is [Encode Scheduling](../../specs/backend/encode-scheduling/spec.md)
and its [requirements](../../specs/backend/encode-scheduling/requirements.md).

## Scope and Research Question

The research question is:

> Can dxmt9 stop treating a CPU-ready source boundary as a Metal render-pass
> boundary, and can it later distribute encoding across CPU threads without
> introducing extra attachment stores, loads, or observable D3D9 reordering?

The three mechanisms address different parts of that question:

| Mechanism | Primary unit | What it solves | What it does not solve |
|---|---|---|---|
| ANGLE `ContextMtl` | Backend context | Central ownership of API state, current command buffer, and active encoder; compatible-pass reuse | CPU-parallel encoding or command-buffer stitching |
| `MTLParallelRenderCommandEncoder` | One render pass in one `MTLCommandBuffer` | Concurrent CPU recording into ordered child encoders | Pass continuation across command buffers or future queue sources |
| Metal 4 suspend/resume | One jointly committed command-buffer array | One GPU render pass assembled from encoders in separate command buffers | Arbitrary delayed continuation after an earlier commit |
| dxmt9 `EncodeSession` | Ordered source group plus live Metal state | Pass continuity across dxmt9 source and `seqId` boundaries | CPU-parallel encoding by itself |

## Terminology and Ownership Axes

A design should state ownership independently on these axes:

| Axis | Meaning in this note |
|---|---|
| API semantic state | D3D9 or GLES bindings and dynamic state that survive native encoder changes |
| Logical render pass | The ordered draws that may remain in one attachment-local GPU pass |
| Native encoder | A live `MTLRenderCommandEncoder` or `MTL4RenderCommandEncoder` |
| Command buffer | The native submission container holding one or more encoders |
| CPU encoding partition | The draw range assigned to one recording thread |
| Source publication | An immutable CPU-ready prefix becoming scheduler-visible |
| Partition edge | A replay-range subdivision; not a Metal boundary by itself |
| Physical encoder segment | One serial encoder, parallel child, or Metal 4 segment |
| Logical render-pass boundary | A D3D9/Metal semantic end to attachment-local rendering |
| Submission boundary | A command buffer or joint group entering Metal execution |
| Source lifetime | Imported records, payload arenas, retained resources, and allocator ranges |
| Completion identity | The D3D9-visible `seqId` values completed by one native submission |

The boundaries are not interchangeable:

- Ending a dxmt9 source does not imply ending a logical render pass.
- Ending a native encoder does not always imply a GPU pass break in Metal 4.
- Dividing CPU work among threads does not permit reordering draw commands.
- Joining native work does not join dxmt9 source ownership or completion records
  automatically.

## ANGLE Metal: `ContextMtl`

### Audited facts

The source audit is pinned to ANGLE revision
[`cd05752a5137b5f068c11a7a3561e7441a34df75`](https://chromium.googlesource.com/angle/angle/+/cd05752a5137b5f068c11a7a3561e7441a34df75/).
Later ANGLE revisions may change details.

At that revision, `ContextMtl` owns the current Metal command buffer, render,
blit, and compute encoder wrappers, a dirty-bit set, and render-pipeline and
dynamic-state shadows. This makes the context longer-lived than either a
command buffer or an encoder. See
[`ContextMtl.h`](https://chromium.googlesource.com/angle/angle/+/cd05752a5137b5f068c11a7a3561e7441a34df75/src/libANGLE/renderer/metal/ContextMtl.h#585).

Its render-pass acquisition policy is:

- Reuse the current render encoder when its render-pass descriptor is
  compatible. Compatibility compares attachment identity while ignoring
  load/store options.
- Otherwise end current encoding, apply the command-buffer flush policy, ensure
  a command buffer is ready, mark all render state dirty, and restart the
  render encoder with the new descriptor.
- Before switching to a normal blit or compute encoder, end the active render
  encoder and force in-progress attachment contents to be stored.
- At command-buffer flush, finish active encoders before commit. At present,
  finish active encoders, attach the drawable presentation, and commit.

These behaviors are visible in
[`ContextMtl.mm`](https://chromium.googlesource.com/angle/angle/+/cd05752a5137b5f068c11a7a3561e7441a34df75/src/libANGLE/renderer/metal/ContextMtl.mm#1755).
The context also limits accumulated render passes and draw work before flushing;
it does not assume that one command buffer must represent one API frame.

### dxmt9 inference

The reusable pattern is not the class name. It is the ownership split:

- Semantic state belongs to a context/session that survives encoder restart.
- Pass compatibility is decided from attachment identity and hazards, not from
  the caller or packet boundary.
- Native encoder restart invalidates native binding assumptions, so state must
  be marked dirty and rebound.
- A forced encoder-kind transition must preserve attachment contents before
  blit, compute, observation, or submission.

This supports dxmt9's `EncodeSession` direction. It does not prove that ANGLE's
compatibility predicate is sufficient for D3D9. dxmt9 additionally needs exact
render-target sampling hazards, deferred-clear ordering, query semantics,
resource-initializer waits, Present tails, and per-source completion expansion.

## `MTLParallelRenderCommandEncoder`

### API facts

Apple defines `MTLParallelRenderCommandEncoder` as a way to split one render
pass into multiple subordinate `MTLRenderCommandEncoder` instances that record
to the same command buffer and attachments. The subordinate encoders may be
assigned to different CPU threads. Commands execute in subordinate-encoder
creation order, and the application is responsible for thread synchronization.
Attachment load and store actions apply once at the beginning and end of the
whole pass. See Apple's
[`MTLParallelRenderCommandEncoder`](https://developer.apple.com/documentation/metal/mtlparallelrendercommandencoder)
documentation and the archived
[`Metal Programming Guide`](https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/Render-Ctx/Render-Ctx.html#//apple_ref/doc/uid/TP40014221-CH7-SW17).

The parent parallel encoder is created from one `MTLCommandBuffer` and one
`MTLRenderPassDescriptor`. Each worker obtains a child render encoder. All child
encoders must finish before the parent ends encoding.

### dxmt9 inference

This API addresses CPU recording width only after dxmt9 can form independent,
ordered draw ranges for a single known pass. It does not replace:

- `EncodeSession` source retention and ordered completion;
- pass-boundary and hazard classification;
- a state snapshot for the first draw assigned to each child encoder;
- coordination of queries, clears, sidecars, and indirect resource use; or
- a policy for waiting for later CPU-ready sources.

Because every child has an independent native state stream, each partition
needs a complete or proven-minimal initial binding snapshot. A sequential D3D9
state stream cannot be divided safely merely at source or draw-count
thresholds.

`MTLParallelRenderCommandEncoder` is therefore a candidate only if profiles show
Metal API encoding on one CPU thread as the limiting stage and a prior analysis
shows sufficiently large, partitionable passes. It is not a remedy for drawable
waits, GPU-bound workloads, or excessive semantic pass splits.

## Metal 4 Render-Pass Suspend/Resume

### API facts

Metal 4 adds `MTL4RenderEncoderOptions.suspending` and `.resuming`. Apple
specifies the following sequence for parallel render-pass encoding:

1. The first command buffer starts the pass with `suspending`.
2. Intermediate command buffers use both `resuming` and `suspending`.
3. The final command buffer uses `resuming`.
4. All participating command buffers are submitted together, in order, in one
   `MTL4CommandQueue` commit array.
5. The sequence must not intermix compute, blit, acceleration-structure, or
   machine-learning encoding between its render-pass segments.

The exact restrictions are in
[`MTL4RenderEncoderOptions`](https://developer.apple.com/documentation/metal/mtl4renderencoderoptions),
and array submission is defined by
[`MTL4CommandQueue`](https://developer.apple.com/documentation/metal/mtl4commandqueue).
Apple's WWDC25 session
[`Explore Metal 4 games`](https://developer.apple.com/videos/play/wwdc2025/254/)
describes the result as merging the separately encoded segments into one GPU
render pass, avoiding intermediate attachment store/load work.

Apple describes this model as conceptually replacing
`MTLParallelRenderCommandEncoder`: separate threads can own separate command
buffers and render encoders instead of sharing a parallel parent encoder. See
[`Understanding the Metal 4 core API`](https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api).

Metal 4 is a new API lane, not an availability-neutral extension of the legacy
encoder model. Apple states that Metal 4 supports Apple M1 and later and A14
Bionic and later in
[`Discover Metal 4`](https://developer.apple.com/videos/play/wwdc2025/205/).
A dxmt9 design must retain a legacy Metal path for unsupported deployment
targets unless the project raises its platform baseline.

### dxmt9 inference

Suspend/resume could let dxmt9 encode selected source ranges into separate
command buffers, possibly on separate threads, while preserving one
attachment-local GPU pass. This could reduce the need to keep one live legacy
encoder and command buffer open while sources become CPU-ready.

It is not arbitrary pass continuation. The complete ordered group must be known
and committed together. Once an earlier segment has been committed alone,
future work cannot retroactively resume it under this contract. A Metal 4 lane
would therefore need a bounded gather/finalization policy, not an indefinitely
open stream.

The no-intermix rule also means that dxmt9 must end a stitched group before a
blit, resolve, initializer operation, readback, query operation requiring a
different encoder, or Present tail. Those are already semantic-boundary
candidates in the dxmt9 specification.

## Comparison with dxmt9 `EncodeSession`

At the repository state reviewed for this note, dxmt9 has an opt-in
open-command-buffer carrier and an `EncodeChunkSessionState`. The session
retains ordered source identities and carries native encoder state across
`encodeChunk()` calls.
Relevant implementation points are:

- [`open_cb_carrier.hpp`](../../src/dxmt9/render/open_cb_carrier.hpp) for source
  classification, release, and fail-open decisions;
- [`dxmt9_draw_encoder.mm`](../../src/dxmt9/dxmt9_draw_encoder.mm) for carried
  encode state and session finalization; and
- [`dxmt9_queue.hpp`](../../src/dxmt9/dxmt9_queue.hpp) for bounded, consecutive
  completion-source aggregation.

The normative owner, boundary vocabulary, session states, and execution lanes
are in [Encode Scheduling](../../specs/backend/encode-scheduling/spec.md).

| Concern | ANGLE `ContextMtl` | Parallel encoder | Metal 4 suspend/resume | dxmt9 `EncodeSession` |
|---|---|---|---|---|
| Long-lived semantic-state owner | Yes | No | No | Yes |
| Reuses a live compatible encoder | Yes | Children share one parent pass | No; creates pass segments | Yes |
| Spans native command buffers | No | No | Yes, in one commit array | No in the current legacy-Metal carrier |
| CPU-parallel recording | No intrinsic requirement | Yes | Yes, by separate command buffers | No |
| Knows D3D9 source/`seqId` identity | No | No | No | Yes |
| Expands one native completion to sources | No | No | No | Yes |
| Defines D3D9 semantic boundaries | No | No | No | Yes |

The closest conceptual reference for current dxmt9 is ANGLE's context ownership,
not either parallel API. Metal 4 is the more relevant future mechanism if dxmt9
later needs both pass continuity and CPU-parallel encoding across independently
owned command buffers.

## Adoption Implications

### Legacy Metal lane

- Keep semantic pass classification independent of source publication.
- Make every native encoder-owned binding or cache explicitly session-owned or
  explicitly invalidated on restart.
- Preserve exact ordered source completion and source payload lifetime when one
  Metal submission backs several `seqId`s.
- Treat forced store/load, render-pass count, command-buffer count, and tile
  locality as measured promotion gates, not assumed improvements.

### Parallel-encoding experiment

- First measure encode-thread CPU time separately from producer, replay,
  drawable wait, commit, and GPU time.
- Build immutable partition-entry state snapshots and prove child creation order
  matches D3D9 draw order.
- Reject partitions crossing any semantic boundary or query-observation point.
- Compare single-encoder, parallel-parent, and Metal 4 segmented variants on the
  same pass-locality and visual-correctness corpus.

### Metal 4 lane

- Isolate Metal 4 objects and submission behind a capability-selected Unix-side
  backend; do not expose them across the PE/unix ABI.
- Gather a finite source prefix before choosing suspending/resuming options.
- Submit all stitched command buffers together and expand completion only after
  that joint submission completes.
- Maintain a legacy Metal fallback and test both lanes against the same D3D9
  ordering and lifetime model.

## Explicit Non-Conclusions

This research does not establish that:

- any of the three mechanisms improves dxmt9 frame rate;
- ANGLE's pass compatibility rule is a complete D3D9 rule;
- CPU-parallel encoding is useful before profiles identify encode CPU as the
  bottleneck;
- Metal 4 removes the need for source retention, hazard tracking, fail-open
  finalization, or ordered completion expansion;
- a source boundary, fixed draw count, payload threshold, `EndScene`, or
  `seqId` boundary is a safe render-pass split; or
- the current opt-in dxmt9 carrier is ready to become the default path.

## Verification Questions

Before adopting or promoting any lane, answer these with native tests, Metal
capture/counters, and the relevant TLA+ models:

1. Which exact D3D9 operations close a logical pass, including query and
   resource-initializer cases?
2. Does every encoder restart invalidate and restore all required pipeline,
   dynamic, argument-buffer, stream, index, residency, and sidecar state?
3. Can source payload storage remain alive without pinning a scarce producer
   ring slot until GPU completion?
4. Does one native completion advance every covered `seqId` exactly once and in
   order, including fail-open and device-loss paths?
5. Does partitioning preserve draw order and provide a complete entry snapshot
   for every child or Metal 4 segment?
6. Do Metal captures show one intended GPU pass rather than hidden store/load
   pairs, and do pass/command-buffer counts improve rather than regress?
7. Is encode-thread CPU time large enough that parallel recording can move the
   frame critical path?
8. What OS, SDK, GPU-family, Rosetta, and Wine-runtime matrix selects the Metal 4
   lane, and is legacy fallback behavior identical?
9. How large may a gather window become before latency or producer backpressure
   outweighs pass-locality gains?

## Primary References

- ANGLE Metal backend at pinned revision
  [`cd05752a5137b5f068c11a7a3561e7441a34df75`](https://chromium.googlesource.com/angle/angle/+/cd05752a5137b5f068c11a7a3561e7441a34df75/src/libANGLE/renderer/metal/).
- Apple,
  [`MTLParallelRenderCommandEncoder`](https://developer.apple.com/documentation/metal/mtlparallelrendercommandencoder).
- Apple, archived
  [`Metal Programming Guide: Graphics Rendering`](https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/Render-Ctx/Render-Ctx.html).
- Apple,
  [`MTL4RenderEncoderOptions`](https://developer.apple.com/documentation/metal/mtl4renderencoderoptions).
- Apple,
  [`MTL4CommandQueue`](https://developer.apple.com/documentation/metal/mtl4commandqueue).
- Apple,
  [`Understanding the Metal 4 core API`](https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api).
- Apple, WWDC25,
  [`Explore Metal 4 games`](https://developer.apple.com/videos/play/wwdc2025/254/).
- Apple, WWDC25,
  [`Discover Metal 4`](https://developer.apple.com/videos/play/wwdc2025/205/).
