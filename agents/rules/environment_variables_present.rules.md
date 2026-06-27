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
| `DXMT9_PRESENT_BOUNDARY_PRESENT_COMPLETION` | Wait on `presentCompletedSeqId_` (selects `BoundaryPolicy::PresentCompletion`) — default on; explicit `0` opts out | `1` |
| `DXMT9_PRESENT_REFRESH_HZ` | Override refresh rate (numeric Hz) | derived |
| `DXMT9_LAYER_DISPLAY_SYNC` | CAMetalLayer display sync opt-in; when set non-zero, the presenter sets `CAMetalLayer.displaySyncEnabled` from the D3D9 PresentationInterval. Default is **off** in code (`dxmt9_presenter.mm::layerDisplaySyncEnabled`), and the runtime instead enforces the per-present minimum duration via `MTLCommandBuffer::presentDrawableAfterMinimumDuration` | `0` (off — code default; the docs row historically said `1`, the code path defaults to off) |
| `DXMT9_DISABLE_VSYNC` | Runtime "vsync off" override. When set non-zero, the presenter forces both `CAMetalLayer.displaySyncEnabled = NO` and the software `minimumPresentDuration = 0` regardless of the D3D9 PresentationInterval the app requested. Production-side counterpart to per-swapchain `D3DPRESENT_INTERVAL_IMMEDIATE`. Useful for perf triage and user-controlled "vsync off" without modifying the D3D9 app. If the app already requests Immediate, this is expected to be a no-op; verify with `present_schedule_requested_immediate`, `present_schedule_after_minimum_duration`, and `present_schedule_immediate` before claiming a perf delta. Resolver: `resolveDisableVsync()` in `dxmt9_presenter.hpp`. Tested by `dxmt9-present-disable-vsync-spec` | `0` |
| `DXMT9_DISABLE_PRESENT_BOUNDARY` | Skip the present-boundary wait entirely (selects `BoundaryPolicy::Disabled`) | `0` |
| `DXMT9_SPLIT_PRESENT_CHUNK` / `DXMT9_SPLIT_PRESENT_ACQUIRE` | Diagnostic present splitting. `DXMT9_SPLIT_PRESENT_CHUNK` publishes the current pre-Present writing slot, then commits a separate Present slot; this is not the locality-preserving tail-Present CPU-ready design by itself and should be treated as a split/CB-shape probe unless H57/H86 gates prove otherwise. `DXMT9_SPLIT_PRESENT_ACQUIRE` splits around drawable acquire for acquire-path triage. | `0` |
| `DXMT9_ENCODE_TAIL_PRESENT_BATCH` | Experimental encode-thread repair for `DXMT9_SPLIT_PRESENT_CHUNK`: when the ready queue has a non-present head followed by a Present-only tail slot, dequeue both and encode them as one tail Metal submission with expanded `completionSources`. This keeps the split CPU-ready surface while recovering the pre-split command-buffer/pass locality shape. Default off; pair with the `v0.0.3` visual-safe anchor before reading perf numbers. | `0` |
| `DXMT9_STAGE_TAIL_PRESENT_CHUNK` | Experimental P4/run-ahead carrier. Active only when `DXMT9_ENCODE_TAIL_PRESENT_BATCH=1` is also enabled. On Present, publish the pre-Present head into a Pending but encode-invisible staged lane, then release it back before the Present-only tail so the tail batch encoder can submit one combined Metal command buffer. Default off; judge only with no-gputrace P4/locality gates and the `v0.0.3` visual-safe anchor. | `0` |
| `DXMT9_STAGE_PRE_PRESENT_COMMAND_LIMIT` | Experimental P4/run-ahead carrier. Active when either `DXMT9_ENCODE_TAIL_PRESENT_BATCH=1` or `DXMT9_OPEN_CB_PREENCODE_TAIL_PRESENT=1` is enabled. In the staged-tail path, a pre-Present writing slot that reaches this command count is published into the Pending but encode-invisible staged lane used by `DXMT9_STAGE_TAIL_PRESENT_CHUNK`; the later Present releases all staged heads before the tail so the complete-prefix selector can encode `[head..., Present-only tail]` as one Metal submission. In the open-CB path, the same split reason makes the pre-Present head encode-visible to the open-CB carrier while the final submit is deferred until the Present tail. Default off; this is not a visual-safe or FPS claim until a no-gputrace run passes ready-depth, no-enqueue/P4, locality, and `v0.0.3` gates. | `0` |
| `DXMT9_OPEN_CB_PREENCODE_TAIL_PRESENT` | Experimental open-command-buffer P4 carrier. On `Present`, any remaining non-empty pre-Present writing slot is first published as a `PresentSplitBefore` head so the final Present command is a drawable/present-only tail. When paired with earlier source publishers such as `DXMT9_STAGE_PRE_PRESENT_COMMAND_LIMIT=N`, `DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_PUBLISH=1`, or `DXMT9_OPEN_CB_CPU_READY_COMMAND_LIMIT=N`, pre-Present heads can be encoded into an uncommitted Metal command buffer; subsequent compatible heads and the Present tail append into the same command buffer, and the final tail submission carries strict `completionSources` for all source seqIds. Default off; treat as a runtime candidate only after P4/locality counters and qualitative inspection against the `v0.0.3` visual-safe anchor pass. | `0` |
| `DXMT9_OPEN_CB_CARRY_RENDER_SESSION` | Experimental companion for `DXMT9_OPEN_CB_PREENCODE_TAIL_PRESENT=1`: keeps the explicit `EncodeChunkSession` alive across open-CB preencoded sources so source boundaries do not force `flushRender(Final)` until a Present tail or deterministic fail-open release finalizes the shared command buffer. Tail-less compatible heads may start a pending session; if no compatible source arrives and the writer stops/drains, the runtime must finalize and submit the visible prefix instead of completing it inline. Pair with a source-publication carrier such as `DXMT9_STAGE_PRE_PRESENT_COMMAND_LIMIT=N` or `DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_PUBLISH=1`; judge only with no-gputrace render-pass/final-reopen/locality gates and the `v0.0.3` visual-safe anchor before `.gputrace` promotion. | `0` |
| `DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_PUBLISH` | Experimental source-publication carrier for `DXMT9_OPEN_CB_PREENCODE_TAIL_PRESENT=1` + `DXMT9_OPEN_CB_CARRY_RENDER_SESSION=1`. Before semantic non-draw boundary commands (`Clear`, surface copy, `StretchRect`, color fill, depth resolve), publish any non-empty non-present writing slot with `chunk_publish_reason_semantic_boundary`; the boundary command itself remains in the next source. The encode session may stream a compatible render encoder across those sources when the next source is already ready. If the semantic-boundary source is the pending chain tail and the ready queue is empty, release is selected by `DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_RELEASE_MODE`: default `completion_wait` waits for an active completion wait and releases at most one such prefix for that wait; `deterministic` releases at the semantic boundary independent of the wait window. Default off; do not promote until visual, ordered completion, P4/no-enqueue, and locality gates pass. | `0` |
| `DXMT9_OPEN_CB_CPU_READY_COMMAND_LIMIT` | Experimental producer-side source-publication probe, active only with `DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_PUBLISH=1`. After draw submit appends work, publish a non-empty non-present writing slot as `chunk_publish_reason_semantic_boundary` once its command count reaches this numeric limit. Unlike `DXMT9_OPEN_CB_WRITER_ACTIVE_CPU_READY_PUBLISH`, this cut is deterministic producer-side CPU-readiness rather than a reactive encode-thread miss handler. The source boundary is intended to be metadata-only for `EncodeSession` and must not force an open render encoder to close. Default unset/off; treat as diagnostic until visual, ordered completion, P4/no-enqueue, and CB/pass/tile/load-store gates pass. | unset |
| `DXMT9_OPEN_CB_WRITER_ACTIVE_CPU_READY_PUBLISH` | Experimental H161 source-publication probe, active only with `DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_PUBLISH=1`. When a tail-less semantic-boundary pending session is waiting with no ready sources, no active completion wait, an active writer, and a non-empty non-present writing slot, publish that current writing slot as `chunk_publish_reason_semantic_boundary` so the open `EncodeSession` can append it as a CPU-ready source. The path refuses to run without queue headroom and remains default off because it cuts the writer slot from the encode thread; treat it as a diagnostic for R-BACK-2.40/R-BACK-2.43, not a production policy, until visual, ordered completion, no-inline-completion, and locality gates pass. | `0` |
| `DXMT9_OPEN_CB_ACTIVE_WAIT_CPU_READY_APPEND` | Experimental active-wait session-append probe, active only with `DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_PUBLISH=1`. When a completion wait is already active and a tail-less semantic-boundary pending `EncodeSession` can be released, the encode loop first tries to keep appendable ready sources inside that session instead of immediately submitting the prefix; if no ready source exists but the current writer slot holds non-present work, it may publish that writer slot as a semantic CPU-ready source and re-enter the append path. This is intended to test whether source boundaries can stay metadata-only under the actual P4 wait window. Default off; do not promote until visual, ordered completion, no-inline-completion, and non-increasing CB/pass/tile/load-store gates pass. | `0` |
| `DXMT9_OPEN_CB_WAIT_START_CPU_READY_PUBLISH` | Experimental wait-start source-publication probe, active only with `DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_PUBLISH=1`. When the completion thread is already inside `waitUntilCompleted()`, the ready queue is empty, no pending open-CB `EncodeSession` exists yet, and the writer owns non-present work, publish the current writing slot as `chunk_publish_reason_semantic_boundary` so the encode thread can start a pending session inside the wait window. This tests the H167 gap where active waits almost never overlap an existing pending session. Default off; do not promote until visual, ordered completion, no-inline-completion, P4 movement, and non-increasing CB/pass/tile/load-store gates pass. | `0` |
| `DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_RELEASE_MODE` | Release policy for `DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_PUBLISH=1`. `completion_wait` preserves the original P4 diagnostic behavior: a tail-less semantic-boundary prefix is submitted only while the completion thread is actively waiting, and at most once per wait window. `deterministic` is the R-BACK-2.39 probe mode: a tail-less semantic-boundary prefix is finalized and submitted from source/queue state alone, without depending on the completion-wait window. Both modes are experimental and must pass visual, ordered completion, no-inline-completion, and locality gates before promotion. | `completion_wait` |
| `DXMT9_OPEN_CB_PENDING_TAIL_WAIT_US` | Default-off bounded-release diagnostic for `DXMT9_OPEN_CB_PREENCODE_TAIL_PRESENT=1` + `DXMT9_OPEN_CB_CARRY_RENDER_SESSION=1`. A positive value permits a tail-less `PresentSplitBefore` head to encode into a pending `EncodeSession` before the Present tail exists; if the tail does not arrive within the timeout, the runtime must fail-open by finalizing and submitting the encoded prefix rather than completing it inline. This is a wallclock diagnostic knob, not the deterministic production gate required by R-BACK-2.39. Treat any positive value as experimental until visual, render-pass locality, completion-source, and `gpu_command_buffer_errors` gates pass. | `0` |
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

The four `DXMT9_*PRESENT_BOUNDARY*` vars above resolve once at
process init into a single `dxmt9::BoundaryPolicy` value with priority
`Disabled > PresentCompletion > Completion > AfterAcquire > Default`
— `Disabled` short-circuits the whole boundary; `PresentCompletion`
is the historical default-on branch (null / empty env counts as set,
only explicit `0` demotes). `AfterAcquire` is observationally a no-op
when a higher-precedence wait branch is selected (those branches do
not consult `presentDequeuedSeqId_`). See
`dxmt9::resolveBoundaryPolicy` in `src/dxmt9/dxmt9_presenter.hpp`,
the switch in `CommandQueue::presentBoundary`
(`src/dxmt9/dxmt9_command_queue.cpp`), the AfterAcquire site in
`src/dxmt9/dxmt9_draw_encoder.mm`, and the matrix spec
`tests/native/backend/present_boundary_policy_spec.cpp`.
