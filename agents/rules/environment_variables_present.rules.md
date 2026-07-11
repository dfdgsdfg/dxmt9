# dxmt9 Environment Variables — Present policy

Part of the [`environment_variables.rules.md`](environment_variables.rules.md)
index (present-acquire / boundary / latency tuning). A flag is "set" when its
value is a non-empty string that is not `0`, unless documented otherwise. See
the index for global notes.

## Present policy

| Var | Purpose | Default |
|---|---|---|
| `DXMT9_PRESENT_ASYNC_ACQUIRE` | Request drawable on the encode thread async | `0` |
| `DXMT9_PRESENT_PREACQUIRE` | Pre-acquire drawable before encode | `0` |
| `DXMT9_PRESENT_ACQUIRE_ON_SUBMIT` | Acquire drawable at submit time | `0` |

The three acquire-policy vars above are mutually exclusive in effect:
the runtime resolves them once at Presenter construction into a single
`AcquirePolicy` value with priority `Async > SyncOnSubmit > PreAcquire
> Sync`. With multiple vars set the highest-priority one wins; the
others are ignored. See `dxmt9::resolveAcquirePolicy` in
`src/dxmt9/dxmt9_presenter.hpp` and the matrix spec
`tests/native/backend/present_acquire_policy_spec.cpp`.

| Var | Purpose | Default |
|---|---|---|
| `DXMT9_PRESENT_BOUNDARY_AFTER_ACQUIRE` | Move `notePresentDequeued` to after acquire (selects `BoundaryPolicy::AfterAcquire`) | `0` |
| `DXMT9_PRESENT_BOUNDARY_COMPLETION` | Wait on command-buffer `completedSeqId_` (selects `BoundaryPolicy::Completion`) | `0` |
| `DXMT9_PRESENT_BOUNDARY_DEFERRED` | Experimental frame-latency run-ahead policy. The final Present tail is committed immediately, and the present-completion frame-latency wait is deferred until the next `Present` tail is about to be submitted. This differs from `DXMT9_DISABLE_PRESENT_BOUNDARY`: it still enforces configured frame-latency backpressure before another present tail, but gives the producer a chance to record and publish the next frame's offscreen work while the previous present tail is completing. Default off; do not promote until R-BACK-2.43 pass-streaming, ordered completion, visual, P4, and locality gates pass. | `0` |
| `DXMT9_PRESENT_BOUNDARY_PRESENT_COMPLETION` | Wait on `presentCompletedSeqId_` (selects `BoundaryPolicy::PresentCompletion`) — default on; explicit `0` opts out | `1` |
| `DXMT9_PRESENT_REFRESH_HZ` | Override refresh rate (numeric Hz) | derived |
| `DXMT9_LAYER_DISPLAY_SYNC` | CAMetalLayer display sync opt-in; when set non-zero, the presenter sets `CAMetalLayer.displaySyncEnabled` from the D3D9 PresentationInterval. Default is **off** in code (`dxmt9_presenter.mm::layerDisplaySyncEnabled`), and the runtime instead enforces the per-present minimum duration via `MTLCommandBuffer::presentDrawableAfterMinimumDuration` | `0` (off — code default; the docs row historically said `1`, the code path defaults to off) |
| `DXMT9_DISABLE_VSYNC` | Runtime "vsync off" override. When set non-zero, the presenter forces both `CAMetalLayer.displaySyncEnabled = NO` and the software `minimumPresentDuration = 0` regardless of the D3D9 PresentationInterval the app requested. Production-side counterpart to per-swapchain `D3DPRESENT_INTERVAL_IMMEDIATE`. Useful for perf triage and user-controlled "vsync off" without modifying the D3D9 app. If the app already requests Immediate, this is expected to be a no-op; verify with `present_schedule_requested_immediate`, `present_schedule_after_minimum_duration`, and `present_schedule_immediate` before claiming a perf delta. Resolver: `resolveDisableVsync()` in `dxmt9_presenter.hpp`. Tested by `dxmt9-present-disable-vsync-spec` | `0` |
| `DXMT9_DISABLE_PRESENT_BOUNDARY` | Skip the present-boundary wait entirely (selects `BoundaryPolicy::Disabled`) | `0` |
| `DXMT9_SPLIT_STRETCH_CHUNK` | Split stretch-rect chunks | `0` |
| `DXMT9_DRAW_CHUNK_COMMAND_LIMIT` | Max commands per chunk (numeric) | derived |
| `DXMT9_CHUNK_DRAW_PAYLOAD_ARENA_LIMIT_BYTES` | Cap (numeric bytes) on the per-chunk draw-payload arena; `0`/unset/unparseable disables the cap | `0` |
| `DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS` | Limit max frame latency to backbuffer count | `0` |
| `DXMT9_MAX_FRAME_LATENCY` | Override max frame latency (numeric) | unset |
| `DXMT9_SYNC_PRESENT_FLUSH` | Flush synchronously after present for present-path triage | `0` |

## Retired present/overlap experiments

The earlier run-ahead prototype envs `DXMT9_OFFSCREEN_RUN_AHEAD`,
`DXMT9_ENCODE_COALESCE_READY_SLOTS`, and
`DXMT9_ENCODE_COALESCE_READY_SLOT_LIMIT` are not honored by the current HEAD.
The implementation was reverted after the H74/H75 GT1 probes showed overlap
mechanism but failed locality, total-wait, and visual-correctness gates. Do not
schedule new runs with those envs unless the run-ahead code is intentionally
reintroduced and this rules file is updated in the same change.

The open-CB command-buffer carrier and split/stage/tail-Present precursor
envs — `DXMT9_OPEN_CB_PREENCODE_TAIL_PRESENT`,
`DXMT9_OPEN_CB_CARRY_RENDER_SESSION`, `DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_PUBLISH`,
`DXMT9_OPEN_CB_CPU_READY_COMMAND_LIMIT`,
`DXMT9_OPEN_CB_WRITER_ACTIVE_CPU_READY_PUBLISH`,
`DXMT9_OPEN_CB_ACTIVE_WAIT_CPU_READY_APPEND`,
`DXMT9_OPEN_CB_WAIT_START_CPU_READY_PUBLISH`,
`DXMT9_OPEN_CB_DRAW_ATTACHMENT_BOUNDARY_PUBLISH`,
`DXMT9_OPEN_CB_DRAW_CONTINUATION_BOUNDARY_PUBLISH`,
`DXMT9_OPEN_CB_DRAW_CONTINUATION_COMMAND_LIMIT`,
`DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_RELEASE_MODE`,
`DXMT9_OPEN_CB_PENDING_TAIL_WAIT_US`, `DXMT9_SPLIT_PRESENT_CHUNK`,
`DXMT9_SPLIT_PRESENT_ACQUIRE`, `DXMT9_ENCODE_TAIL_PRESENT_BATCH`,
`DXMT9_STAGE_TAIL_PRESENT_CHUNK`, and `DXMT9_STAGE_PRE_PRESENT_COMMAND_LIMIT`
— are not honored by the current HEAD; their runtime machinery (the
`runOpenCbTailPresentEncodeLoop`/`runEncodeBatchLoop` encode-thread lanes,
`render::tail_present_batch` helpers, `IRenderBackend::onChunkBatchReady`, and
the `submitPresent`/`submitDrawRun*` branches they gated) was removed. The
`present-pacing` H86-H189 experiment chain (see
`docs/perfomance/present-pacing/log.md`) proved the open-CB carrier could open
a P4 overlap window (H183-H188) but never recovered H187's command-buffer/
render-pass locality shape without it; the family was superseded by the
producer-side `DXMT9_OFFLOAD_COMMIT_REPLAY` win (H190+) and formally retired
once that offload plus the accepted index-cache locality opt-in covered the
proven average-FPS gains. Do not schedule new runs with these envs unless the
open-CB/tail-Present carrier is intentionally reintroduced and this rules file
is updated in the same change. R-BACK-2.39/2.40/2.43 (pass-streaming source
attachment) remain open requirements for a future design; see
`specs/backend/gap.md`.

The commit-replay draw-run preflush envs `DXMT9_ENABLE_DRAW_RUN_PREFLUSH_MERGE`
(H187 opportunity / H188 runtime A/B) and
`DXMT9_ENABLE_DRAW_RUN_PREFLUSH_MIXED_CARRIER` (H192 follow-up) are not
honored by the current HEAD; their resolvers, the merge-lane
`queueImportedDrawRunAsSubmissions` helper, and the always-on
`commit_chunk_replay_draw_run_preflush_{opportunities,pending_records,
run_records,combined_records}` opportunity-sizing counters were removed from
`src/d3d9/device_c_chunk_replay.cpp` and `src/dxmt9/dxmt9_perf_counters.{hpp,cpp}`.
Both lanes tried to avoid flushing a pending draw-submission batch before an
immediately following imported draw-run; H188 showed materializing the run as
submissions (the merge lane) raised snapshot/queue CPU, and H192's mixed
backend call was the corrected shape for the same H187 opportunity. The
reopen premise behind both was invalidated once the commit-replay offload
(`DXMT9_OFFLOAD_COMMIT_REPLAY`, H195) landed and the H212 producer
attribution showed the residual wall is the game's own CPU, not a
producer-serial preflush boundary dxmt9 can shrink. The `submitDrawRunBatchAndRun*`
/ `submitDrawSubmissionBatchAndDrawRunCanonical*` `CommandQueue`/`Device`
family was removed together with the chunk-end carry lane (next paragraph):
its only surviving production caller was the carry's forced-resource-marking
mixed-carrier path (`pendingRequiresResourceMarking` in
`device_c_chunk_replay.cpp`), and its exclusive coverage in
`tests/native/core/core_device_coverage_spec.cpp` went with it. Do not
schedule new runs with the two removed envs unless the merge/mixed-carrier
lanes are intentionally reintroduced and this rules file is updated in the
same change.

The chunk-end pending-submission carry env `DXMT9_ENABLE_CHUNK_END_CARRY`
(H201 implementation of the H198/H199 opportunity) is not honored by the
current HEAD; its resolver, the `D9CDevice` carry store/adopt/forced-flush
machinery, the `pendingRequiresResourceMarking` cascade, the orphaned
`submitDrawRunBatchAndRun*` / `submitCompactDrawRunBatchAndRun*` and
`submit{Compact}DrawSubmissionBatch[AndDrawRunCanonical]WithResourceMarking`
`CommandQueue`/`Device` family, and the
`commit_chunk_replay_end_carry_{stored,adopted,flushed}[_records]` plus
`commit_chunk_replay_pending_flush_forced_resource_marking_*` counters were
removed. H202 runtime-rejected the lane: the mechanism was proven (99.84%
adoption) but FPS was null — the carried work shifted into larger batch
submits rather than being removed. The reopen premise is structurally dead:
the engine-default commit-replay offload (`DXMT9_OFFLOAD_COMMIT_REPLAY`,
H195/`d45af067`) moved the whole cost class onto a worker that idles
~39.4ms/present, and the H212 producer attribution showed the residual wall
is the game's own CPU. The companion `DXMT9_PERF_CHUNK_END_FLUSH_PROBE`
opportunity probe — which existed only to size that carrier — was removed in
the follow-up sweep together with its `D9CDevice::ChunkEndFlushProbe` stamp
storage and the fifteen `commit_chunk_replay_end_flush_probe_*` counters
(`summarize_3dmark05_perf.py` keeps its tolerant report section for
historical `result.json` files). Do not schedule new runs with either env
unless the carry lane is intentionally reintroduced and this rules file is
updated in the same change.

The compact uniform submission envs `DXMT9_ENABLE_COMPACT_UNIFORM_SUBMISSIONS`
(H156) and its attribution companion `DXMT9_PERF_UNIFORM_COMPACT_BREAKDOWN`
are not honored by the current HEAD; their resolvers, the
`DrawRunCompactSubmission` / `DrawUniformCompactSubmissionPayload` /
`DrawSubmissionUniformScratch` producer carrier types, the
`queueCompactDraw*Submission` replay lanes, the
`submitCompactDraw{Submission,Run}Batch` `Device`/`CommandQueue`/`BackendDevice`
family, the backend `DrawUniformCompactPayloadView` find/append overloads, the
`d3d9_snapshot_uniform_compact_*` timers, and the always-on
`d3d9_snapshot_uniform_materialized_compact_*` opportunity-sizing counters
were removed. H156 proved the per-submission byte reduction but rejected the
scratch carrier as a default CPU win. The H132 accepted always-on compact
stage-constant storage (usage-live VS/PS constant byte prefixes in the
backend `ChunkSlot` uniform arenas) is unrelated and remains in place. The
reopen premise died with the engine-default commit-replay offload
(`DXMT9_OFFLOAD_COMMIT_REPLAY`, H195/`d45af067`), which moved the whole
producer replay cost class onto a worker idling ~39.4ms/present, and the H212
producer attribution showing the residual wall is the game's own CPU. Do not
schedule new runs with these envs unless the compact carrier is intentionally
reintroduced and this rules file is updated in the same change.

The five `DXMT9_*PRESENT_BOUNDARY*` vars above resolve once at
process init into a single `dxmt9::BoundaryPolicy` value with priority
`Disabled > DeferredPresentCompletion > PresentCompletion > Completion >
AfterAcquire > Default` — `Disabled` short-circuits the whole boundary;
`DeferredPresentCompletion` keeps the present-completion target but moves the
wait to the next `Present`; `PresentCompletion` is the historical default-on
branch (null / empty env counts as set, only explicit `0` demotes).
`AfterAcquire` is observationally a no-op when a higher-precedence wait branch
is selected (those branches do not consult `presentDequeuedSeqId_`). See
`dxmt9::resolveBoundaryPolicy` in `src/dxmt9/dxmt9_presenter.hpp`,
the switch in `CommandQueue::presentBoundary`
(`src/dxmt9/dxmt9_command_queue.cpp`), the AfterAcquire site in
`src/dxmt9/dxmt9_draw_encoder.mm`, and the matrix spec
`tests/native/backend/present_boundary_policy_spec.cpp`.
