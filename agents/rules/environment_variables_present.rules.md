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
| `DXMT9_OPEN_CB_PREENCODE_TAIL_PRESENT` | Experimental open-command-buffer P4 carrier. When paired with `DXMT9_STAGE_PRE_PRESENT_COMMAND_LIMIT=N`, pre-Present split heads are encoded early into an uncommitted Metal command buffer; subsequent split heads and the Present tail append into the same command buffer, and the final tail submission carries strict `completionSources` for all source seqIds. Default off; treat as a runtime candidate only after P4/locality counters and qualitative inspection against the `v0.0.3` visual-safe anchor pass. | `0` |
| `DXMT9_OPEN_CB_CARRY_RENDER_SESSION` | Experimental companion for `DXMT9_OPEN_CB_PREENCODE_TAIL_PRESENT=1`: intended to keep the explicit `EncodeChunkSession` alive across open-CB preencoded heads so chunk boundaries do not force `flushRender(Final)` until the Present tail finalizes the shared command buffer. Current HEAD includes the H140 safety guard: if a `PresentSplitBefore` head is seen without an existing tail chain/fail-open finalizer, the runtime suppresses the deferred pending-head start, records `open_cb_tail_present_pending_suppressed_no_tail`, and submits the source normally. Pair with `DXMT9_STAGE_PRE_PRESENT_COMMAND_LIMIT=N`; judge only with no-gputrace render-pass/final-reopen/locality gates and the `v0.0.3` visual-safe anchor before `.gputrace` promotion. | `0` |
| `DXMT9_OPEN_CB_PENDING_TAIL_WAIT_US` | Experimental bounded-release companion for `DXMT9_OPEN_CB_PREENCODE_TAIL_PRESENT=1` + `DXMT9_OPEN_CB_CARRY_RENDER_SESSION=1`. A positive value disables the H140 tail-less-head suppression, lets the open-CB path start a pending pre-Present head, and waits up to this many microseconds for the Present tail before finalizing/submitting the head alone. Default `0` preserves the suppression. Watch `open_cb_tail_present_pending_tail_wait_timeout`, `open_cb_tail_present_pending_timeout_submitted`, render-pass counters, `gpu_command_buffer_errors`, and qualitative output before using FPS as evidence. | `0` |
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
