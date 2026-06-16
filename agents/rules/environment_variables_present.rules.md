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
| `DXMT9_SPLIT_PRESENT_CHUNK` / `DXMT9_SPLIT_PRESENT_ACQUIRE` | Split present chunks | `0` |
| `DXMT9_SPLIT_STRETCH_CHUNK` | Split stretch-rect chunks | `0` |
| `DXMT9_DRAW_CHUNK_COMMAND_LIMIT` | Max commands per chunk (numeric) | derived |
| `DXMT9_CHUNK_DRAW_PAYLOAD_ARENA_LIMIT_BYTES` | Cap (numeric bytes) on the per-chunk draw-payload arena; `0`/unset/unparseable disables the cap | `0` |
| `DXMT9_OFFSCREEN_RUN_AHEAD` | Producer/encode overlap A/B knob: directly publish the writing chunk at offscreen pass-boundary candidates (currently color0/depth handle change) so the encode thread can commit non-present work during the previous frame's present-completion wait. It never allocates a frame-latency present token (present-only sync; `PresentFrameLatency` `CommitNonPresent`). When set, `dxmt9_queue.cpp` also enables encode-side non-present ready-slot coalescing unless `DXMT9_ENCODE_COALESCE_READY_SLOTS` explicitly overrides it, so promoted slots can be folded back into one Metal command-buffer chain. Pair with `compare_3dmark05_perf_counters.py` H43 overlap + H57 locality gates; a valid result needs overlap improvement without increasing `command_buffers_per_present`, render passes, or tile-preservation bytes. Still missing full `R-BACK-2.40` CpuReady staging. Currently reuses the `chunk_publish_reason_draw_limit` counter and should not be combined with `DXMT9_DRAW_CHUNK_COMMAND_LIMIT` in A/B runs. Read once at first use in `dxmt9_command_queue.cpp::offscreenRunAheadEnabled`; coalescing is read once in `dxmt9_queue.cpp::encodeReadySlotCoalescingEnabled` | `0` |
| `DXMT9_ENCODE_COALESCE_READY_SLOTS` | Override encode-side ready-slot coalescing. Unset inherits `DXMT9_OFFSCREEN_RUN_AHEAD`; non-zero forces coalescing on for consecutive non-present, non-readback ready slots; `0` forces it off. Coalescing preserves record order and emits completion for every source seqId after the shared tail command buffer completes | unset |
| `DXMT9_ENCODE_COALESCE_READY_SLOT_LIMIT` | Maximum number of ready slots the encode thread may fold into one coalesced non-present Metal command-buffer chain. Parsed once; values below `2` disable grouping; values above `16` clamp to `16` | `4` |
| `DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS` | Limit max frame latency to backbuffer count | `0` |
| `DXMT9_MAX_FRAME_LATENCY` | Override max frame latency (numeric) | unset |
| `DXMT9_SYNC_PRESENT_FLUSH` | Flush synchronously after present for present-path triage | `0` |

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
