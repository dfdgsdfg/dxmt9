# Present-Pacing — display sync, frame latency, and the wallclock cap

> Part of the 3DMark05 GT1 perf-bottleneck investigation. Root map:
> [[overview-3dmark05-gt1]].

## Scope & question

This domain owns the **CPU/sync side** of GT1 wallclock: why
`completion_wait_ms` (the time the completion handler thread spends in
`MTLCommandBuffer.waitUntilCompleted()`) reached 28-31 s while
`gpu_command_buffer_time_ms` was only ~4 s, where that gap actually lives,
and which production-safe knobs can recover the slack without breaking
visual sync.

Every finding here moves *wallclock fps* directly, not GPU frame time. The
GPU frame-time story is owned by [[hidden-backend-storage]] /
[[index-cache-locality]]; the per-CB encode story by [[state-churn-encode]].

## Hypotheses & verdicts

| # | Hypothesis | Verdict | Evidence |
|---|------------|---------|----------|
| H1 | `completion_wait_ms` is dominated by Present-bearing CBs, not draw/blit/sync/queue waits | accepted | [[present-pacing-display-sync.01]] (100% `completion_present_wait_ms`) |
| H2 | The wait is display/present-completion pacing (`waitUntilCompleted()` returning after drawable/compositor acceptance) rather than pure GPU compute | accepted historically; current mechanism refined | [[present-pacing-display-sync.01]] showed the original display-sync attribution. Current [[present-pacing-current-immediate.02]] shows GT1 direct now requests Immediate and never uses `presentDrawableAfterMinimumDuration`, yet `completion_present_wait_ms` remains ~39 s. [[present-pacing-completion-watcher-status.03]] shows the watcher pops immediately (`p50 0.041ms`, `pending_depth_max=0`) and waits mostly from `Committed` status. |
| H3 | Per-CB encode (`encode_draw_cpu_ms / CB`) sits at ~11 ms, near the 16.67 ms vsync budget | accepted | [[present-pacing-display-sync.01]] (per-CB encode 11.45 ms baseline, 11.23 ms DSync-off) |
| H4 | `DXMT9_MAX_FRAME_LATENCY=3` + `DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS=0` recovers slack with vsync on | rejected | [[present-pacing-frame-latency.01]] (wallclock Δ +0.07%, p95 +31%) |
| H5 | `DXMT9_PRESENT_ASYNC_ACQUIRE=1` reduces the completion-path acquire cost | rejected (axis not load-bearing) | [[present-pacing-async-acquire.01]] (acquire wait −37.5% but axis < 0.5% of total; wallclock Δ +0.22%) |
| H6 | Reducing per-CB encode below the vsync budget (<= 16.67 ms / CB) restores fps without changing pacing policy | open, with CPU wins | [[present-pacing-encode-budget.01]] sized the gap; [[present-pacing-bind-cache-work-a.01]] rejected broad bind-cache as the main lever; cbuf identity repoint, plan-direct binding-packet cache, component-hash reuse, usage-aware payload hashing, and FFP known-zero usage are accepted CPU wins. Latest [[snapshot-cache-snapshot.09]] sample is still not a vsync-on fps proof: `completion_wait_ms=39978.924`, `encode_draw_cpu_ms=17711.215`, and `d3d9_snapshot_draw_submission_cpu_ms=7196.881` over `1740` presents. [[state-churn-encode-encode-phase.09]] splits argbuf open: reserve `745.942ms`, `setArgumentBuffer=115.192ms`, table bind `192.913ms`, table-bind skip `0`; [[state-churn-encode-encode-phase.10]] then cuts reserve by `-51.95%` and `encode_draw_cpu_ms` by `-3.87%` in a same-present A/B. [[state-churn-encode-encode-phase.11]] rejects dirty-category identity repoint (`0` hits). [[state-churn-encode-encode-phase.12]] splits `stream_bind` into texture/sampler (`1065ms`), index (`670ms`), shader stream (`497ms`), and raster (`389ms`) phases; [[state-churn-encode-encode-phase.13]] narrows texture/sampler to fragment resolve (`575ms`) and fragment direct bind (`317ms`); [[state-churn-encode-encode-phase.14]] accepts sampler pre-handle skip as a CPU win (`texture_sampler` -18.84%, `encode_draw` -117ms); [[state-churn-encode-encode-phase.15]] then reuses the packet sampler-state hash (`fragment_direct` -68ms, texture/sampler parent -69.6ms); [[state-churn-encode-encode-phase.16]] rejects texture pre-resolve skip as a default win (`1.206M` skips but texture/sampler parent +48ms), removes the rejected branch, and verifies texture/sampler parent returns to baseline (`822.864 -> 821.007`). The next no-gputrace targets remain named encode buckets plus residual snapshot hash/indexed-fallback work. |
| H7 | A user-opt-in vsync-off env recovers fps without a code-side encode optimisation | accepted historically; not load-bearing for current GT1 direct path | [[present-pacing-vsync-off.01]] measured a full-workload wallclock win when the present path was sync-paced. Current [[present-pacing-current-immediate.02]] shows both default and `DXMT9_DISABLE_VSYNC=1` already use `present_schedule_immediate=1680`, frame p50/p95 are flat, and `present_schedule_after_minimum_duration=0`. |
| H8 | Completion wait is caused by completion watcher backlog | rejected | [[present-pacing-completion-watcher-status.03]]: `completion_pending_depth_max=0`, dequeue age p50/p95 `0.041/0.067ms`, and `completion_dequeue_status_completed=0`. The watcher is not late; it waits on just-committed command buffers. |
| H9 | Current average FPS should be attacked through P2/P3 CPU cadence plus P4 present-completion wait, not hot-frame GPU locality first | accepted gate split | [[present-pacing-current-fps-owner.04]]: current low-overhead scout has sampled FPS p50/p95 `18.102/26.630`, frame wall p50/p95 `55.242/84.648ms`, `completion_present_wait_ms=25.091ms/present`, `gpu_command_buffer_time_ms=3.113ms/present`, immediate presents only, `present_boundary_wait_ms=0`, and `completion_pending_depth_max=0`. GPU locality remains a hot-frame/ceiling lane, but average FPS requires P2/P3 wins and P4 pipeline-depth recovery. |
| H10 | Completion wait is overlapped by next-frame enqueue/encode work | rejected; hard under-pipelining accepted | [[present-pacing-pipeline-overlap.05]]: `1799 / 1799` waits have no later CB enqueue during `waitUntilCompleted()`, `completion_wait_with_enqueue_ms=0`, `completion_wait_without_enqueue_ms=44789.044`, `completion_enqueue_while_waiting=0`, and enqueue-side pending depth max is only `1`. Stage run: wait-end -> publish/encode-dequeue/Metal-commit/enqueue p50 `16.645/20.116/36.470/36.502ms`, so producer and encode work both run after the wait instead of hiding under it. |
| H11 | `DXMT9_DISABLE_PRESENT_BOUNDARY` or deeper `DXMT9_MAX_FRAME_LATENCY` makes the producer run while completion waits | rejected as current owner | [[present-pacing-boundary-latency-ab.06]]: fresh baseline, boundary-disabled, and latency6 scouts all keep `present_boundary_waits=0`, `completion_wait_without_enqueue_ms≈44.0-44.5s`, and sampled FPS p50 `17.8-18.0`. `DXMT9_DISABLE_PRESENT_BOUNDARY=1` reaches the runtime (`present_boundary_skipped=1740`) but creates only one noise-level overlap event also seen in baseline. The remaining owner is before `CommitPublish` or outside dxmt9's explicit boundary wait. |
| H12 | The post-wait producer gap is app/Wine/macdrv-side, before unix `commit_chunk` entry | rejected; unix replay/submit owner accepted | [[present-pacing-prepublish-stage.07]]: wait-end -> `commit_chunk` entry p50/p95 is only `1.040/2.668ms`, while wait-end -> `CommitPublish` is `15.894/29.912ms` and wait-end -> Metal commit is `22.276/54.146ms`. `commit_chunk_replay_cpu_ms=18981.064`, nested queue draw submission is `8154.509ms`, and nested snapshot submission is `6636.191ms`, so the average-FPS lane remains commit/replay/snapshot/encode, not app/Wine event pacing. |
| H13 | Same-sample stage deltas can split the exposed no-enqueue gap without subtracting unrelated percentiles | accepted | [[present-pacing-stage-delta.08]]: current 120s scout keeps `completion_wait_with_enqueue_ms=0`, then splits the exposed path into `commit_chunk entry -> CommitPublish` p50/p95 `6.172/28.101ms`, `CommitPublish -> EncodeDequeue` `2.535/5.086ms`, and `EncodeDequeue -> commandBuffer.commit()` `11.384/22.232ms`. Queue wake is secondary; pre-publish replay/submit/snapshot and backend encode are the two load-bearing CPU stages. |
| H14 | The app thread is blocked inside PE `Present()` for the whole completion wait | rejected | [[present-pacing-pe-present-timing.09]]: with `DXMT9_PE_RECORDER_STATS=1 DXMT_LOG_LEVEL=info`, PE `Present total_ms` p50/p95/max is only `2.580/5.077/22.659ms`, almost entirely `flush_ms`, while `completion_wait_ms` p50/p95/max is `28.419/39.576/52.217ms`. The next unix chunk still crosses quickly after completion (`wait_to_commit_chunk_entry` p50/p95 `0.888/3.025ms`), but that is not a PE API-call timestamp. |
| H15 | The app/Wine loop does not call D3D9 until Metal completion | rejected; PE-local next-frame recording accepted | [[present-pacing-pe-call-cadence.10]]: after successful PE `Present`, the next PE D3D9 call is almost always `BeginScene` (`1702 / 1703` rows) and arrives with `entry_delta_ms` p50/p95/p99 `0.310/0.436/1.811ms`. `completion_wait_with_enqueue_ms=0` still holds, so the missing overlap is not app API-call cadence; it is PE-recorder/unix submission boundary or later (`commit_chunk` replay/publish/snapshot and backend encode). |
| H16 | PE-local next-frame work crosses into unix immediately after the first PE call | rejected; chunk-fill cadence accepted | [[present-pacing-pe-chunk-cadence.11]]: first PE call p50 is `0.308ms`, but the first non-empty chunk after `Present` is always `capacity_post`, always `64` records, and crosses into unix at steady p50/p95 `19.908/34.810ms` after `Present` return. The first bridge itself is cheap (`bridge_ms` p50/p95 `0.504/0.617ms`). |
| H17 | Lowering PE chunk capacity from `64` to `32` creates producer run-ahead | rejected as a simple knob | [[present-pacing-pe-chunk-size-ab.12]]: chunk32 reaches the recorder (`recordCount=32`) and slightly lowers first-chunk p50 (`19.908 -> 19.034ms`) and no-enqueue wait-to-commit-entry p50 (`0.917 -> 0.471ms`), but `completion_wait_with_enqueue_ms` remains `0.000` and replay/encode do not materially improve. Earlier publish remains an architecture experiment, not a global capacity knob. |
| H18 | The first chunk is late because filling 64 records takes most of the time | rejected; first-record append delay accepted | [[present-pacing-pe-record-milestones.13]]: after `Present`, `BeginScene` still arrives at p50 `0.306ms`, but the first appendable record is `apply_state` at p50 `18.061ms`. Records `1 -> 64` then arrive quickly (`18.061 -> 19.683ms` p50), and the first chunk follows at `19.706ms`. Chunk threshold is not the front gate; the first record-producing state/draw boundary is. |
| H19 | The hidden N+1 dependency sits on one of the first D3D9 calls after `Present` | rejected; initial call-sequence attribution superseded | [[present-pacing-pe-call-sequence.14]]: calls `1..4` after `Present` are `BeginScene`, `GetRenderTarget`, `GetRenderTarget`, and `SetRenderTarget`, all at p50 `<= 0.532ms`. The run initially placed the long gap before `SetVertexShaderConstantF`, but that was incomplete because `Clear` and `EndScene` were not in the call milestone sequence. |
| H20 | The first record-producing front gate is `Clear`, after a post-RT-setup gap | accepted | [[present-pacing-pe-clear-gate.15]]: with `Clear`/`EndScene` included, call 5 is steady `Clear` at p50 `18.408ms`; `SetRenderTarget` return is p50 `0.581ms` with only `0.015ms` duration, so `SetRenderTarget` is not the sleeper. The p50 gap is `SetRenderTarget` return -> `Clear` entry `17.635ms`; record 1 `apply_state` is appended inside `Clear` at p50 `18.554ms`, and the first `capacity_post` chunk follows at p50 `20.400ms`. |
| H21 | Frame-sampling logs create or amplify the post-RT-setup `Clear` gate | rejected | [[present-pacing-pe-clear-nosampling.16]]: removing `--frame-sampling` drops `dxmt9-perf-frame` lines from `1,726` to `0`, but `SetRenderTarget` return -> `Clear` entry remains p50 `17.651ms -> 17.646ms` and first chunk remains p50 `20.402ms -> 20.386ms`. The perf-frame line is a correlation marker, not the stall owner. |
| H22 | The apparent `SetRenderTarget` -> `Clear` gap is really an uninstrumented child getter stall | rejected; hidden calls found but not sleeper | [[present-pacing-pe-wide-call-coverage.17]]: wider coverage reveals `Surface::GetDesc` and `Texture::GetSurfaceLevel` before `Clear`, but child return logging shows they return quickly (`Surface::GetDesc` call 7 duration p50 `0.013ms`, `Texture::GetSurfaceLevel` p50 `0.054ms`). The last logged return is p50 `0.674ms`, and `Clear` still enters at p50 `18.421ms`, leaving a `17.656ms` p50 gap. |
| H23 | The remaining front gap is a broad app/Wine runloop or macdrv sleep | rejected as broad owner; exact PC owner open | [[present-pacing-xctrace-threadstate.18]]: re-exported `Metal System Trace` CPU tables show the D3D/Wine producer thread `0x3b1b5c` has `15,354ms` Running sample weight across a `15.563s` trace, while `runloop-events` has only two `3DMark05.exe` rows on a different main thread. This does not pinpoint the current `SetRenderTarget` -> `Clear` PC, but it weakens sleep/runloop as the broad explanation. |
| H24 | The `Clear` front gate is a hidden dxmt9 D3D9 API wait | rejected; wrapper-level attribution accepted, higher owner superseded | [[present-pacing-pe-caller-pc.19]]: PE caller-PC/module logging shows the steady sequence is identical in `1,716 / 1,716` ordinals. `SetRenderTarget` returns from 3DMark05.exe wrapper RVA `0x2AF4F` at p50 `0.730ms`, its nested `Surface::GetDesc` caller resolves to `d3d9.dll!0x13EE9` and takes only p50 `0.020ms`, and `Clear` enters later from 3DMark05.exe wrapper RVA `0x2B061` at p50 `18.373ms`. The p50 `17.484ms` gap is not inside `SetRenderTarget`, `Clear`, or a hidden child getter. Caller-stack follow-up supersedes the claim that these wrapper RVAs are the higher render-loop owner. |
| H25 | The stable owner above the wrapper stubs is still hidden | rejected; 3DMark05 command-dispatch cadence accepted | [[present-pacing-pe-caller-stack.20]]: PE stack logging shows milestones 2..8 share higher frame `3DMark05.exe+0x88760` in `1,707 / 1,707` matching ordinals. `0x2AF4F` and `0x2B061` are D3D wrapper stubs; `0x88760` is the return site of a command-object dispatcher (`0x4886E0`) whose virtual `call *0x18(%eax)` executes the D3D wrapper command. `SetRenderTarget` return -> `Clear` entry remains p50 `17.429ms`. The front gate is therefore when 3DMark05 dispatches the record-producing `Clear` command object, not a dxmt9 boundary/latency/ring wait. |
| H26 | The `Clear` front gate or next Metal enqueue waits on dxmt9 completed-seq/waterline publication | rejected for dxmt9 completion signal; actual Metal/CA completion remains separate | [[present-pacing-completion-signal-delay.21]]: `DXMT9_PERF_COMPLETION_SIGNAL_DELAY_MS=8` applies `1696` sleeps / `13568ms` after `waitUntilCompleted()` and before completed-seq publication, but `SetRenderTarget -> Clear` p50 stays `17.631 -> 17.550ms`, first chunk p50 stays `20.802 -> 20.827ms`, and next enqueue p50 stays `21.558 -> 20.274ms`. The front gate is not waiting on dxmt9's completion signal. |
| H27 | Flushing the PE chunk immediately after `Clear` creates producer overlap | rejected as a simple early-publish lever | [[present-pacing-pe-clear-flush.22]]: `DXMT9_PE_FLUSH_AFTER_CLEAR=1` changes the first chunk from `capacity_post` / `64` records to `clear` / `2` records and moves first-chunk p50 `20.582 -> 18.935ms`, but `completion_wait_with_enqueue_ms` remains `0.000`, `completion_enqueue_while_waiting=0`, and `commitCount` rises `41947 -> 45857`. Keep it diagnostic-only; continue P2/P3 replay/snapshot/encode reductions or a larger producer-overlap design. |
| H28 | The current low-overhead code state makes `DXMT9_PE_FLUSH_AFTER_CLEAR=1` useful after recent encode/copy cleanup | rejected-current | [[present-pacing-pe-clear-flush.23]]: matching recorder-stats runs still move the first chunk earlier (`20.710 -> 19.089ms`) and shrink it (`64 -> 2` records), but `completion_wait_with_enqueue` moves `2 -> 0`, tail-600 FPS p50 moves `15.788 -> 15.681`, and `commitCount` rises `41,429 -> 45,617`. |
| H29 | Current low-overhead FPS is blocked by serialized post-wait P2/P3 work, not by missing app/D3D9 calls after completion | accepted attribution | [[present-pacing-lowoverhead-serial.24]]: the next unix commit entry follows completion quickly (`0.861ms` p50), while same-cycle post-wait stages remain large: `commit_chunk entry -> CommitPublish` `14.068ms` p50, `CommitPublish -> EncodeDequeue` `3.678ms`, and `EncodeDequeue -> commandBuffer.commit()` `11.528ms`. Disabling Stage 2 argbuf cuts encode per present (`9.388 -> 6.143ms`) but raises completion wait (`27.116 -> 31.148ms/present`), so local CPU wins need a P4/overlap proof. |
| H30 | Raising the mid-chunk sub-command-buffer cap recovers average FPS by allowing more early commits | rejected-current | [[present-pacing-subcb-cap.25]]: `DXMT9_MID_CHUNK_COMMIT_CAP_PER_RENDER_PASS=8` reaches the runtime (`chunk_subcb_count_max 4 -> 8`), doubles sub-CBs (`5,355 -> 12,173`), and drops cap suppression (`8,658 -> 1,471`), but tail-600 FPS p50 worsens (`16.849 -> 16.665`), completion wait per present rises (`27.116 -> 28.900ms`), and wait-end -> next-enqueue p50 worsens (`15.135 -> 19.980ms`). |
| H31 | Moving publish-time PSO prefetch out of the serialized Present publish path can improve sampled FPS even if encode lookup rises | accepted placement signal; superseded by H32 default | [[present-pacing-publish-pso-prefetch.26]]: `DXMT9_DISABLE_PUBLISH_PSO_PREFETCH=1` cuts Present replay by `-2.488ms/present`, raises encode pipeline lookup by `+2.222ms/present`, reduces `completion_wait_ms` by `-2.420ms/present`, and improves repeated warm FPS p50 by `+0.564`. |
| H32 | Encode-slot PSO prefetch is a better default than publish-time PSO prefetch for current GT1 | accepted default | [[present-pacing-publish-pso-prefetch.27]]: default run keeps `encode_draw_pso_prefetch_handle_missing=0`, moves `prepare_slot_pso_prefetch_cpu_ms` to `0`, records `encode_slot_pso_prefetch_cpu_ms=2.605ms/present`, cuts completion wait by `-1.509ms/present`, and improves warm FPS avg by `+0.717`. |
| H33 | The remaining PSO prefetch work is now an encode-stage key/lookup problem, not a Present pacing problem | accepted attribution | [[state-churn-encode-encode-phase.71]]: split counters show `encode_slot_pso_prefetch_cpu_ms=2.806ms/present`, dominated by draw PSO lookup/key work at `2.506ms/present`; `prepare_slot_pso_prefetch_cpu_ms` remains effectively zero and handle misses remain `0`. |
| H34 | Wine's synchronous `OnMainThread()` marshaling is a plausible transmission path for the `SetRenderTarget` return -> `Clear` entry gate | source-audit hypothesis | [[present-pacing-winemac-onmainthread.28]]: Wine `OnMainThread()` can block an app thread until the Cocoa main thread runs the request; event-queue threads use `kevent(..., NULL)` and non-queue threads use `dispatch_semaphore_wait(..., DISPATCH_TIME_FOREVER)`. `ClipCursor`, cursor get/set, and window-frame getters can use it; dxmt9 does not call `macdrv_view_get_metal_layer` per frame and winemac does not issue `presentDrawable`. `GetCursorPos` is lower-priority because of win32u's 100ms cache, but a stale cursor timestamp can still fall through to winemac. The exact winemac call and main-thread holder remain unproven and require threshold logging joined to PE milestones. |
| H35 | The non-invasive P4 fallback should export xctrace CPU thread summaries alongside Metal timing | accepted tooling | [[present-pacing-xctrace-cpu-summary-tooling.29]] adds `summarize_xctrace_cpu_threads.py` and `run_3dmark05_system_trace_sidecar.sh --export-cpu-summary`. Existing `phase43` smoke parses `20,964` `time-profile` rows and `20,989` `time-sample` rows; producer thread `0x3b1b5c` remains `15,354ms` running with zero `OnMainThread` / `kevent` / `dispatch_semaphore_wait` hits, and the generated P4 scout verdict is `producer-running-negative-scout`, while `5` non-producer wait hits stay on callback-like threads. |
| H36 | PE `thread_id=...` rows can be extracted on a current-head sidecar, but they do not directly select xctrace threads | inconclusive; native id mapping required | [[present-pacing-xctrace-cpu-summary-current.30]]: `winemac-onmainthread-xctrace-r2` completed with xctrace/wrapper status `0`, joined `1528/1528` encoder rows, and parsed `13,509` `time-profile` plus `13,540` `time-sample` rows. The PE log had `45,053` `pe_present_*` rows with a single `thread_id=0xd0`, selected from `pe-log-clear-return`, but no xctrace thread label or `thread-info` `tid` matched `0xd0`; verdict `producer-thread-not-found`. This proves the PE id is in the Win32 namespace for this purpose. The next P4 proof needs native Mach/pthread id mapping or Wine/macdrv threshold telemetry before promoting or rejecting `OnMainThread` as the owner. |
| H37 | Same-run native producer-thread selection should come from the unix replay boundary, not PE `GetCurrentThreadId()` | accepted tooling; needs next trace | `dxmt9c_device_commit_chunk` now logs `unix_commit_chunk_entry native_tid=0x...` under the existing `DXMT9_PE_RECORDER_STATS=1` diagnostic gate, and the xctrace CPU summarizer prefers that native id before falling back to PE `thread_id=0x...`. This directly addresses H36's namespace mismatch while preserving the same sidecar flow. The next proof run is `--export-cpu-summary --cpu-producer-from-pe-log`; pass condition is a selected native producer row with decisive `OnMainThread`/wait evidence or a strong negative on the actual producer. |
| H38 | Native-selector xctrace scout does not find producer-thread `OnMainThread`/wait evidence | negative scout; P2/P3 remains primary | [[present-pacing-native-selector-xctrace.31]]: `winemac-onmainthread-xctrace-r3` completed with xctrace/wrapper status `0`. The direct log had `40,044` `unix_commit_chunk_entry native_tid=0x5cef8b` rows and `46,031` PE rows with `thread_id=0xd0`; the CPU summary selected native source `native-log-commit-chunk-entry` and matched xctrace `tid=0x5cef8b`. The selected producer was sampled `10427/10427` rows Running, with `0` producer wait keyword hits and only `2` non-producer wait hits. The same run still has `completion_present_wait_ms/present=27.589ms`, `completion_present_wait_with_enqueue_ms=0`, `commit_entry_to_publish` p50/p95 `16.701/38.664ms`, and `encode_chunk_cpu_ms/present=14.597ms`. |
| H39 | Default-on resource-shape path still does not show producer-thread wait evidence | negative scout; P2/P3 remains primary | [[present-pacing-native-selector-xctrace.32]] repeats the native-selector System Trace after [[state-churn-encode-encode-phase.81]] makes the resource-shape PSO memo default-on. The selected native producer `0x61e72f` is sampled running in `10439 / 10439` rows with `0` producer wait keyword hits; non-producer wait hits are `3`. The run still has `completion_present_wait_with_enqueue_ms=0`, `completion_present_wait_ms/present=25.208ms`, `commit_entry_to_publish` p50/p95 `34.071/64.333ms`, and `encode_dequeue_to_command_buffer_commit` p50/p95 `26.705/37.060ms`. Because all-frame encoder breakdown is enabled, this is not a low-overhead FPS baseline, but it keeps broad winemac `OnMainThread` below replay/snapshot/encode serialization as the next average-FPS target. |
| H40 | Short System Trace sidecar remains useful while `.gputrace` is blocked, but does not find P4 producer wait evidence | negative scout; sidecar fallback accepted | [[present-pacing-systemtrace-p4-smoke.34]] uses a 2-second normal-rendering Metal System Trace while Xcode `.gputrace` attach is blocked by Developer Mode. It joins `306/306` encoder rows over seq `1114..1148`, selects native producer `0x6572ff`, samples it running in `2519 / 2519` rows, and finds `0` producer wait keyword hits. GPU timing remains vertex dominated (`93.07%` vertex share), with top rows all `opaque-depth-indexed` / `needs-programmable-color-route`; pacing counters still show no overlap (`completion_wait_with_enqueue_ms=0`) and large replay/snapshot/encode work after wait. |
| H41 | Current P4 System Trace sidecar still finds no producer wait-stack hit, but one blocked sample keeps the verdict inconclusive | accepted current constraint | [[present-pacing-systemtrace-p4-current.35]] repeats the 2-second System Trace sidecar on the current code state. It joins `386/386` encoder rows over seq `1052..1087`, selects native producer `0x665ec1`, and records `producer_wait_keyword_hits=0`; however the selected producer has `1` blocked row out of `2,429`, so the CPU verdict is `producer-state-inconclusive`, not a strict negative. Pacing is unchanged for the current owner split: `completion_wait_with_enqueue_ms=0`, `completion_wait_ms=26.319ms/present`, `commit_chunk_replay_cpu_ms=8.510ms/present`, and `encode_chunk_cpu_ms=13.254ms/present`. |
| H42 | Seq-range System Trace sidecar is the preferred blocked-gputrace P4 fallback | accepted fallback; negative scout | [[present-pacing-systemtrace-p4-range.36]] repeats the current sidecar with `--encoder-breakdown-seq-range 1000:1125`. It joins `395/395` encoder rows over seq `1037..1073`, cuts probe output from `553MiB` to `130MiB`, selects native producer `0x668652`, and returns `producer-running-negative-scout` with `2515/2515` producer samples running, `0` blocked rows, and `0` producer wait keyword hits. Pacing still has no overlap: `completion_wait_with_enqueue_ms=0`, `completion_wait_ms=27.606ms/present`, `commit_chunk_replay_cpu_ms=8.516ms/present`, and `encode_chunk_cpu_ms=10.874ms/present`. |
| H43 | P4 compare gates must distinguish total present wait from recovered overlap | accepted tooling | [[present-pacing-compare-gates.37]] extends `compare_3dmark05_perf_counters.py` with derived `completion_present_wait`, `completion_wait_with_enqueue`, and `completion_wait_without_enqueue` per-present/share metrics plus failure gates. Future average-FPS candidates can now require `--require-completion-present-wait-decrease`, `--require-completion-wait-with-enqueue-increase`, or `--require-completion-wait-without-enqueue-decrease` instead of accepting a local CPU win that only shifts wait between buckets. |
| H44 | P2/P3 serial-stage compare gates should be per-present and paired with P4 gates | accepted tooling | [[present-pacing-serial-stage-compare-gates.38]] extends the same compare report with per-present replay, queue draw-submission, snapshot, snapshot-cache, encode, and no-enqueue stage metrics. Future CPU-path candidates can require `--require-commit-chunk-replay-cpu-per-present-decrease`, `--require-encode-chunk-cpu-per-present-decrease`, or no-enqueue stage gates, then pair them with H43 P4 gates before claiming average-FPS movement. |
| H45 | Current low-overhead frame sampling still shows almost no completion overlap and large serialized CPU stages | accepted current baseline | [[present-pacing-frame-sampling-current.39]] runs a normal no-gputrace scout with `DXMT9_PERF_FRAME_SAMPLING=1` and the current compact-uniform opportunity gate. The run records `1,860` presents, sampled avg FPS `17.019`, wall p50/p95 `53.475/82.502ms`, completion wait p50/p95 `27.485/39.849ms`, GPU CB p50/p95 `1.058/13.878ms`, and encode chunk p50/p95 `9.413/18.048ms`. Only `9` waits overlap later enqueues (`398ms` total) while `completion_wait_without_enqueue_ms=50.645s`, so average-FPS work remains P2/P3 CPU reduction plus a larger overlap design rather than GPU-only locality. |
| H46 | Current-run summaries should classify P4 overlap and exposed CPU stages directly | accepted tooling | [[present-pacing-summary-triage.40]] adds a `Pacing / CPU Stage Derived` block to `summarize_3dmark05_perf.py`. The single-run summary now reports completion wait with/without enqueue, overlap/no-enqueue shares, replay/snapshot/encode per-present rows, no-enqueue stage p50/p95 rows, and a current-run verdict such as `under-pipelined-no-enqueue`. This does not replace H43/H44 A/B gates; it makes standalone scouts auditable before selecting the next CPU or overlap candidate. |
| H47 | Fresh summary-triage scout confirms current average-FPS owner | accepted current baseline | [[present-pacing-summary-triage-current.41]] runs the new summary block on a fresh low-overhead scout. It records `1,842` presents, sampled avg FPS `16.822`, wall p50/p95 `53.842/84.123ms`, `completion_wait_ms_per_present=27.599`, overlap share only `0.180%`, no-enqueue share `99.820%`, `commit_chunk_replay_cpu_ms_per_present=8.207`, and `encode_chunk_cpu_ms_per_present=10.566`. The summary verdict is `under-pipelined-no-enqueue`, with largest p50 no-enqueue row `encode dequeue -> command buffer commit`. |
| H48 | xctrace CPU summary should split producer wait evidence from main-thread/present holder evidence | accepted tooling | [[present-pacing-xctrace-holder-summary.42]] adds `p4_holder_keyword_hits`, `holder_status`, `main_thread_holder_keyword_hits`, and `nonproducer_holder_keyword_hits` to `summarize_xctrace_cpu_threads.py`. Producer verdicts still use `OnMainThread`/wait/macdrv keywords; holder fields separately expose `CA::Transaction`, `CAMetalLayer`, `presentDrawable`, and `nextDrawable` samples. The sidecar one-line verdict and manual stdout now surface the same holder split for quick triage. This keeps a future winemac-positive sidecar from conflating producer blocking with callback/main-thread holder noise. |
| H49 | Current post-cbuf-observer low-overhead scout keeps the same P4/P2/P3 owner split | accepted current baseline | [[present-pacing-current-lowoverhead.43]] runs a no-gputrace, no-encoder-breakdown, frame-sampling scout after the cbuf content observer was made opt-in. It renders a normal fog-heavy GT1 frame, records `1,860` presents, `completion_wait_without_enqueue_ms_per_present=26.839`, `completion_wait_with_enqueue_ms_per_present=0.210`, `gpu_command_buffer_time_ms_per_present=3.111`, `commit_chunk_replay_cpu_ms_per_present=8.074`, and `encode_chunk_cpu_ms_per_present=10.902`. The conclusion stays under-pipelined: average-FPS work is still serial replay/snapshot/encode reduction plus an overlap design, not another cbuf-content attribution pass. |
| H50 | Wine `OnMainThread()` should not be promoted to current-owner/capstone without new runtime proof | accepted review | [[present-pacing-winemac-onmainthread.44]] reviews the source audit against later native-selector System Trace scouts. The Wine mechanism is real and remains a patch point, but current runtime evidence samples the selected producer running with `0` wait-keyword hits while low-overhead P2/P3 work remains large. Keep broad winemac debugging below serialized replay/snapshot/encode and P4-overlap work unless a future native producer wait stack, x86_64 Wine threshold row, or low-overhead P4 movement contradicts it. |
| H51 | Stage 2b direct-cbuf removes argbuf table churn without moving the P4 owner | accepted local CPU win; P4 still open | [[present-pacing-direct-cbuf.45]] reviews the `DXMT9_ARGBUF_DIRECT_CBUF=1` no-gputrace scout. It removes the local argbuf table path (`argbuf_table_bind_calls=0`, `argbuf_setup=0`, cbuf update calls `0`) and lowers encode to `5.982ms/present`, but the run remains `under-pipelined-no-enqueue`: `completion_wait_without_enqueue=28.565ms/present`, overlap share `1.955%`, and `sampled_avg_fps=16.864`. The largest exposed p50 no-enqueue row is `commit entry -> publish`, so next average-FPS work returns to replay/snapshot/submit, backend encode children only with P4 gates, or an explicit overlap design. |
| H52 | Current default P2/P3 scout keeps the under-pipelined no-enqueue owner after capture-layer recovery | accepted current baseline | [[present-pacing-current-p2p3.46]] runs the current default no-gputrace scout after the capture-layer route was revalidated. It records `1,800` presents, sampled avg FPS `16.766`, `gpu_command_buffer_time_ms_per_present=3.218`, `completion_wait_without_enqueue_ms_per_present=27.475`, overlap share `0.086%`, `commit_chunk_replay_cpu_ms_per_present=8.325`, snapshot lookup `2.850ms/present`, and `encode_chunk_cpu_ms_per_present=11.152`. A same-run compare against direct-cbuf cuts encode by `-24.44%` but leaves FPS flat and no-enqueue wait worse, so average-FPS candidates still need P2/P3 gates paired with P4 overlap proof. |
| H53 | No-enqueue gaps already include many `commit_chunk` entries before the first publish | accepted attribution | [[present-pacing-noenqueue-beforepublish.47]] adds before-publish counters to the current scout. The run remains `under-pipelined-no-enqueue` (`completion_wait_without_enqueue_ms_per_present=27.151`, overlap share `2.114%`), but the first `CommitPublish` after a no-enqueue wait is preceded by p50 `12` `commit_chunk` entries/replay starts and p50 `11` replay ends. That rejects the broad "producer absent" framing; the exposed owners are first-publish formation (`commit entry -> publish` p50 `14.866ms`) and backend encode-to-Metal-commit (`17.218ms` p50), still gated by P4/FPS movement. |
| H54 | Before-publish chunks are draw/const heavy, and a draw-count publish limit creates overlap but hurts total cost | accepted attribution; rejected simple knob | [[present-pacing-drawchunk-limit.48]] classifies the current before-publish chunks: `93.1%` of scanned chunks have draw records, with `372.366` draw records and `348.008` const records per publish sample. `DXMT9_DRAW_CHUNK_COMMAND_LIMIT=64` proves earlier publish can create overlap (`completion_wait_with_enqueue_ms_per_present` `1.191 -> 21.032`, no-enqueue `26.568 -> 15.289`), but it worsens total completion wait (`27.759 -> 36.321ms/present`), GPU CB time (`3.309 -> 24.519ms/present`), command buffers (`7,199 -> 22,846`), render passes (`21,234 -> 26,280`), and tile preservation bytes (`+75.63%`). The next design must recover overlap without render-pass fragmentation. |
| H55 | Capture-layer repair does not change the current low-overhead P2/P3/P4 owner | accepted current baseline | [[present-pacing-current-lowoverhead.49]] reruns the normal no-gputrace low-overhead scout after file `.gputrace` capture and Xcode counter export were repaired. The run is visually normal and clean (`draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0`), records `1,812` presents, sampled avg FPS `16.557`, GPU CB time `3.231ms/present`, completion wait `27.916ms/present`, no-enqueue wait `27.717ms/present`, replay `8.519ms/present`, snapshot lookup `2.919ms/present`, and encode chunk `11.348ms/present`. Verdict remains `under-pipelined-no-enqueue`; use this as the current-head low-overhead baseline before choosing another CPU/P4 candidate. |
| H56 | A larger draw-chunk limit still creates overlap by adding too much Metal work | rejected threshold sweep; mechanism accepted | [[present-pacing-drawchunk-limit-sweep.50]] tests `DXMT9_DRAW_CHUNK_COMMAND_LIMIT=256` against the current-head low-overhead baseline. It reaches the runtime (`chunk_publish_reason_draw_limit=1,423`) and creates real overlap (`completion_wait_with_enqueue_ms_per_present` `0.199 -> 14.569`, no-enqueue `27.717 -> 15.828`), but total completion wait worsens (`27.916 -> 30.397ms/present`), GPU CB time worsens (`3.231 -> 4.646ms/present`), command buffers rise `7,247 -> 11,153`, render passes rise `21,367 -> 22,686`, tile preservation rises `+5.52%`, encode rises `11.348 -> 12.488ms/present`, and sampled FPS stays flat (`16.557 -> 16.586`, tail-600 p50 slightly worse). This generalizes H54: the carrier is wrong, not just the `64` threshold. |
| H57 | P4 overlap candidates must preserve command-buffer, render-pass, and tile-preservation shape | accepted tooling | [[present-pacing-overlap-locality-gates.51]] adds compare gates for `command_buffers_per_present`, `passes_per_present`, and `tile_preservation_mib` not increasing. These gates intentionally fail the known-bad `DXMT9_DRAW_CHUNK_COMMAND_LIMIT=256` shape even though it increases `completion_wait_with_enqueue_ms`, so a future overlap candidate cannot pass by trading exposed wait for extra Metal command-buffer/render-pass fragmentation. |
| H58 | Synchronously prefetching PSOs from the unpublished slot moves work but does not create P4 overlap | rejected sync placement | [[present-pacing-unpublished-pso-prefetch.53]] tests `DXMT9_PREFETCH_UNPUBLISHED_SLOT_PSO=1` against [[present-pacing-current-lowoverhead.52]]. The mechanism fires (`encode_slot_pso_prefetch_cpu_ms_per_present` `1.169 -> 0.002`, new `unpublished_slot_pso_prefetch_cpu_ms_per_present=1.812`) and preserves Metal shape (`command_buffers_per_present=3.999`, `passes_per_present` flat), but it does not recover overlap (`completion_wait_with_enqueue_ms_per_present` `0.115 -> 0.070`, no-enqueue share `99.608% -> 99.761%`) and sampled FPS falls (`16.666 -> 16.264`). Keep the knob default-off; useful overlap still needs asynchronous producer/encode progress, not more synchronous pre-publish work. |
| H59 | Active present-bearing chunk replay is not the missing first-publish residual | rejected active-replay owner | [[present-pacing-noenqueue-active-replay.54]] adds `completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_*` and runs a fresh no-gputrace scout. The active counter fires for `1,710` publish samples but contributes only `0.862ms` total (`0.003%` of `commit entry -> publish`), while completed replay explains `24.995%` and the residual remains `11.461ms/present`. This is superseded by H60, which proves the residual is inter-replay producer gap, not active present-chunk replay. |
| H60 | Inter-replay producer gap explains the first-publish residual | accepted attribution | [[present-pacing-noenqueue-inter-replay-gap.55]] adds `completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_*` plus publish-wait/onBefore counters. The diagnostic scout records `commit entry -> publish=29.191ms/present`, completed replay `6.833ms/present`, active replay `0.001ms/present`, inter-replay producer gap `22.399ms/present`, and queue publish wait `0.000ms/present`; completed+active+inter-gap closes the row at `100.143%`. The next FPS design target is PE/unix chunk cadence or run-ahead that preserves H57 locality gates, not queue publish wait. |
| H61 | The inter-replay gap is PE chunk-fill cadence, not synchronous bridge overhead | accepted attribution refinement | [[present-pacing-pe-chunk-cadence-all.56]] adds PE recorder all-chunk `chunkFillGap*` and `chunkBridge*` stats. The clean r3 scout records `completion_wait_without_enqueue=28.998ms/present`, first-publish inter-replay gap `13.813ms/present`, completed replay `4.076ms/present`, and PE all-chunk fill gap `55.324ms/present` with average `2.215ms` between chunk returns and next flush entries. PE chunk bridge/replay duration is separately `9.741ms/present`. This proves there is enough PE-local chunk-fill time to account for the H60 inter-replay gaps; the next design target is record/publish run-ahead or state/copy elision that lowers fill cadence while preserving command-buffer/render-pass locality. |
| H62 | PE chunk-fill cadence splits about evenly between first-record gap and active chunk fill | accepted attribution refinement | [[present-pacing-pe-chunk-fill-split.57]] adds `chunkFirstRecordGap*` and `chunkActiveFill*`. The scout records PE all-chunk fill gap `55.331ms/present`, split into first-record gap `25.340ms/present` (`45.8%`) and active fill `29.991ms/present` (`54.2%`), with closure `100.001%`. First-publish inter-replay gap is `14.894ms/present`, completed replay is `4.296ms/present`, and completion remains no-enqueue dominated. A fix that only flushes earlier before the first record or only micro-optimizes record append cannot cover the whole owner; the next design needs both earlier useful publish/run-ahead and active-fill/replay copy reduction. |
| H63 | Active PE chunk fill is mostly inter-append producer wall time, not append CPU | accepted attribution refinement | [[present-pacing-pe-active-fill-split.58]] adds `chunkInterAppendGap*` and `recordAppend*` stats. The scout records active fill `30.127ms/present`; same-chunk inter-append gap is `27.405ms/present` (`90.97%` of active fill), while clean no-flush append CPU is only `2.577ms/present` (`8.56%`). Inter-append plus no-flush append closes `99.52%` of active fill. This lowers raw append-copy microfix priority as the primary FPS lever; the bigger owner is record materialization / D3D9 producer work between appendable records or a run-ahead design that turns those gaps into overlap without H57 locality regressions. |
| H64 | Inter-append producer gap is dominated by draw-to-const/state materialization | accepted attribution refinement | [[present-pacing-pe-inter-append-pairs.59]] adds fixed pair accumulators for `previous record type -> next record type` and exposes the top four pairs. The scout records `chunkInterAppendGap=27.001ms/present`; top pairs are `draw_indexed -> set_vs_const_f` at `12.340ms/present` (`45.70%`), `draw_indexed -> apply_state` at `6.704ms/present` (`24.83%`), `draw_indexed -> draw_indexed` at `4.142ms/present` (`15.34%`), and `draw_indexed -> set_ps_const_f` at `2.301ms/present` (`8.52%`). The top four explain `94.39%` of the inter-append gap. Next no-gputrace work should split VS const dirty-span/flush materialization and apply-state packet/barrier materialization before another `.gputrace` spend. |
| H65 | Const/apply-state leaf CPU does not explain most of the top inter-append gaps | accepted attribution refinement | [[present-pacing-pe-const-apply-split.60]] adds setter, const-flush, `chunkBarrierFlush`, and APPLY_STATE build counters to `pe_recorder_stats`. The scout records `draw_indexed -> set_vs_const_f=14.019ms/present`, but `SetVertexShaderConstantF` body is only `1.000ms/present`; VS const flush is a real inclusive CPU bucket at `3.866ms/present`, but it includes append work after the inter-append timer stops. `draw_indexed -> apply_state=6.819ms/present`, while `chunkBarrierFlush` const drain is `0.006ms/present` and APPLY_STATE packet build is `0.009ms/present`. This demotes APPLY_STATE packet-build micro-optimization and shifts the remaining owner toward producer/state cadence, broader state-setter attribution, or a run-ahead design that preserves H57 locality gates. |
| H66 | Hot-state setter family split rejects immediate setter CPU as the apply-state gap owner | accepted attribution refinement | [[present-pacing-pe-hotsetter-split.61]] adds call/dirty/CPU counters for PE hot-state setter families. The scout records `draw_indexed -> apply_state=6.672ms/present`, but all hot-state setter families combined are only `0.729ms/present`; the largest family is vertex input at `0.332ms/present`, followed by render-target `0.178ms/present`, texture `0.112ms/present`, and shader `0.046ms/present`. This rejects broad setter-body micro-optimization as the next average-FPS lever and leaves broader producer cadence, deferred record materialization, or run-ahead overlap as the current target. |
| H67 | Focused inter-append call-family attribution splits draw const flush and barrier apply-state | accepted attribution refinement + tooling fix | [[present-pacing-pe-gap-callfamily.62]] adds a short `pe_recorder_gap_call_stats` line and an append-family scope around draw/barrier helpers. The r3 scout resolves the former `unknown` rows: `draw_indexed -> set_vs_const_f=15.245ms/present` is `draw`, `draw_indexed -> apply_state=6.895ms/present` is `barrier`, `draw_indexed -> draw_indexed=4.760ms/present` is `draw`, and `draw_indexed -> set_ps_const_f=2.879ms/present` is mostly `draw` with a small `barrier` tail. This confirms VS/PS const rows are deferred const-shadow flushes before later draws, while APPLY_STATE wall time is barrier-path pending-state materialization. |
| H68 | Focused inter-append phase split moves top gaps to pre-call producer cadence | accepted attribution refinement | [[present-pacing-pe-gap-phase-split.63]] splits focused gaps at the next PE D3D9 call entry. The dominant rows are mostly pre-call: `draw_indexed -> set_vs_const_f=15.901ms/present` has `12.983ms/present` pre-call and `2.918ms/present` inside-call; `draw_indexed -> apply_state=6.789ms/present` has `6.780ms/present` pre-call and only `0.009ms/present` inside-call; `draw_indexed -> draw_indexed=4.698ms/present` has `3.259ms/present` pre-call; `draw_indexed -> set_ps_const_f=3.047ms/present` has `2.471ms/present` pre-call. This demotes helper-body/barrier micro-optimization and moves the next owner toward producer cadence, next-call source, or locality-preserving run-ahead, with draw const flush as a smaller local bucket. |
| H69 | Focused pre-call tail split proves the gap is between D3D9 calls | accepted attribution refinement | [[present-pacing-pe-gap-tail-split.64]] splits H68's pre-call phase at previous `DrawIndexedPrimitive` return. Previous draw-call tail is tiny: `draw_indexed -> set_vs_const_f` pre-call `12.949ms/present` is only `0.151ms/present` tail and `12.798ms/present` between-calls; `draw_indexed -> apply_state` is `0.001` tail vs `6.789` between-calls; `draw_indexed -> draw_indexed` is `0.055` tail vs `3.230` between-calls; `draw_indexed -> set_ps_const_f` is `0.018` tail vs `2.397` between-calls. This rejects `DrawIndexedPrimitive` post-append tail as the owner and moves the current target to producer/app/Wine next-call cadence or a locality-preserving run-ahead design. |
| H70 | Between-calls gap is populated by D3D9 producer work, not empty idle wait | accepted attribution refinement | [[present-pacing-pe-between-call-family.65]] counts PE D3D9 call-entry families inside H69's between-calls window. `draw_indexed -> set_vs_const_f` has `14.597ms/present` between-calls led by `vs_const` at `3,429.576` entries/present; `draw_indexed -> set_ps_const_f` has `2.818ms/present` led by `ps_const` and `vs_const`; `draw_indexed -> draw_indexed` includes `vertex_input` at `353.960` entries/present. This rejects the broad "producer absent/idle" framing and moves the next target to constant/state traffic compression or locality-preserving run-ahead that overlaps this producer work without violating H57 locality gates. |
| H71 | Exact between-calls names identify VS const setters and IB desc getters | accepted attribution refinement | [[present-pacing-pe-between-call-name.66]] adds exact call-name buckets for the same H69 windows. The largest row, `draw_indexed -> set_vs_const_f`, is `15.912ms/present` between-calls with `SetVertexShaderConstantF=3,489.217` entries/present and `IndexBuffer::GetDesc=902.976` entries/present. `draw_indexed -> draw_indexed` is led by `IndexBuffer::GetDesc=374.757` entries/present, while `draw_indexed -> apply_state` splits into `SetRenderTarget` and nested `Surface::GetDesc`. This promotes PE child desc caching / getter fast paths as the next local P2/P3 candidate, alongside const traffic compression. |
| H72 | PE child desc caching is a local cleanup, not the current average-FPS lever | rejected average-FPS lever; cleanup accepted | [[present-pacing-pe-desc-cache.67]] caches immutable buffer/surface descs in PE child wrappers and reruns the H71 no-gputrace scout. The local focused rows move slightly (`draw_indexed -> set_vs_const_f` `15.912 -> 15.345ms/present`, `draw_indexed -> draw_indexed` `3.873 -> 3.669`, `draw_indexed -> apply_state` `6.839 -> 6.772`), but aggregate P2/P3/P4 rows stay flat (`completion_wait_without_enqueue` `27.326 -> 27.472ms/present`, replay `7.887 -> 7.871`, encode `10.959 -> 11.020`, overlap `0`). Keep desc caching as a hot-path cleanup, but return average-FPS focus to constant traffic compression or locality-preserving run-ahead. |
| H73 | Run-ahead must decouple logical readiness from Metal command-buffer publication | accepted design gate | [[present-pacing-run-ahead-design.68]] combines the current low-overhead baseline, direct-cbuf repeat, draw-count publish A/B, locality gates, and queue code inspection. In the current queue shape, `CommitPublish` makes one ready `ChunkSlot`, and `encodeChunk()` turns that slot into one Metal command buffer. Simple early publish therefore recovers overlap by creating more command buffers/render-pass splits/tile preservation, which is the known-bad carrier. The next FPS-facing design must use CPU run-ahead staging, encode-side multi-slot coalescing, or a tightly gated render-pass-boundary publish experiment that preserves command-buffer and tile locality. |
| H74 | Current run-ahead/coalescing implementation proves overlap but fails locality | accepted mechanism; rejected current carrier | [[present-pacing-run-ahead-coalesce.69]] runs `DXMT9_OFFSCREEN_RUN_AHEAD=1 DXMT9_ENCODE_COALESCE_READY_SLOTS=1 DXMT9_ENCODE_COALESCE_READY_SLOT_LIMIT=4` against a fresh baseline. Present wait collapses (`completion_present_wait_ms_per_present` `29.839 -> 0.202`), overlap rises (`completion_wait_with_enqueue_ms_per_present` `1.915 -> 20.855`), and no-enqueue wait falls (`27.924 -> 16.135`). But total completion wait worsens (`29.839 -> 36.990`), command buffers per present explode (`3.999 -> 19.156`), GPU command-buffer time rises (`3.718 -> 35.197ms/present`), and `chunk_publish_reason_draw_limit` becomes `15852`. The path validates H73's design constraint; it is not an FPS promotion. |
| H75 | CPU-ready staging restores much of the CB shape but misses FPS and correctness gates | accepted locality refinement; rejected current promotion | [[present-pacing-run-ahead-cpu-ready.70]] reruns the same env after R-BACK-2.40 CPU-ready staging and stronger encode grouping. It improves the carrier versus prior coalescing (`command_buffers_per_present` `19.156 -> 5.741`, `sub_command_buffers_per_present` `10.394 -> 1.287`) and keeps present wait near zero (`0.116ms/present`). However, versus baseline it still raises CBs (`3.999 -> 5.741`), worsens total completion wait (`29.839 -> 40.347ms/present`), worsens wait-to-next-enqueue (`31.632 -> 52.724ms/present`), and inflates commit replay (`8.363 -> 40.441ms/present`). Its `actual.png` also has a large black vertical scene artifact, so `status=pass` is not a visual smoke pass. CB locality recovery is necessary but not sufficient; the next split must explain replay/staging cost and total cadence, and remove the visual artifact, before FPS promotion. |

## Verification methods

- **`countCompletionWait` exclusive sub-buckets** (`dxmt9_perf_counters.cpp:2972`)
  classify every completion-thread `waitUntilCompleted()` into one of
  `present` / `draw` / `blit` / `stretch` / `other`. Already wired; reading
  `result.json` is the attribution.
- **Sibling wait counters**: `present_acquire_wait_ms` (drawable acquire),
  `present_boundary_wait_ms` (present-boundary policy), `sync_wait_ms`
  (synchronous flush), `queue_writer_wait_ms` / `queue_commit_wait_ms` /
  `queue_sequence_wait_ms` (queue ring / submit ordering). Together they
  enumerate every CPU stall the runtime can attribute.
- **Present scheduling counters**: `present_schedule_requested_sync`,
  `present_schedule_requested_immediate`,
  `present_schedule_after_minimum_duration`, `present_schedule_immediate`,
  and `present_minimum_duration_ms` prove whether a run actually enters
  the software `presentDrawableAfterMinimumDuration` path. Current GT1 direct
  is already immediate, so `DXMT9_DISABLE_VSYNC=1` is not a valid fps lever
  unless these counters first show sync-paced scheduling.
- **Completion watcher status counters**: `completion_dequeue_age_ms`,
  `completion_pending_depth_max`, `completion_dequeue_status_*`, and
  `completion_wait_status_*` split completion wait between dxmt9's pending
  queue and Metal command-buffer status. A backlog diagnosis requires
  non-trivial dequeue age or pending depth; current GT1 has neither.
- **Completion overlap counters**: `completion_wait_with_enqueue_ms`,
  `completion_wait_without_enqueue_ms`, `completion_enqueue_while_waiting`,
  `completion_enqueue_pending_depth_max`, and
  `completion_no_enqueue_wait_to_next_enqueue_p*_ms` decide whether completion
  wait is hidden behind later command-buffer production. Current GT1 has no
  overlap: every wait is a no-enqueue wait, followed by a non-trivial
  wait-end to next-enqueue gap.
- **P4 A/B compare gates**:
  `scripts/tools/compare_3dmark05_perf_counters.py` reports completion
  present wait, wait-with-enqueue, and wait-without-enqueue as per-present
  metrics and overlap shares. Use
  `--require-completion-present-wait-decrease`,
  `--require-completion-wait-with-enqueue-increase`, and
  `--require-completion-wait-without-enqueue-decrease` when a candidate claims
  average-FPS movement through P4. Use the `completion_present_*` variants when
  the proof must be restricted to Present-bearing command buffers. The
  wrapper/finalizer path accepts the same flags through
  `--compare-baseline-output`, so no-gputrace scouts and post-Xcode finalization
  use the same gate definitions.
- **P4 locality-preservation gates**:
  `--require-command-buffers-per-present-not-increase`,
  `--require-render-passes-per-present-not-increase`, and
  `--require-tile-preservation-not-increase` must accompany overlap candidates
  that claim a pipeline-depth fix. They reject carriers that create
  `completion_wait_with_enqueue_ms` only by splitting Metal command buffers,
  render passes, or tile-preservation traffic.
- **No-enqueue stage-delta counters**:
  `completion_no_enqueue_stage_commit_entry_to_publish_*`,
  `completion_no_enqueue_stage_publish_to_encode_dequeue_*`, and
  `completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_*`
  record same-cycle deltas after a no-enqueue wait. Use these instead of
  subtracting independent wait-end percentile rows.
- **No-enqueue before-publish replay attribution counters**:
  `completion_no_enqueue_commit_chunk_completed_replay_cpu_before_publish_*`
  accumulates replay CPU from chunks that fully end before the first
  `CommitPublish` after a no-enqueue wait, while
  `completion_no_enqueue_commit_chunk_active_replay_cpu_before_publish_*`
  samples the current present-bearing chunk immediately before
  `dxmt9c_device_present()`.
  `completion_no_enqueue_commit_chunk_inter_replay_gap_before_publish_*`
  accumulates wall time between completed chunk replay and the next
  `commit_chunk` entry before the first publish. If completed+active replay
  does not cover `completion_no_enqueue_stage_commit_entry_to_publish_ms`,
  read this gap before blaming queue publish or hidden present replay.
  `completion_no_enqueue_commit_publish_wait_before_publish_*` then separates
  any actual queue `writeCv` wait at the final publish boundary.
- **P2/P3 A/B compare gates**:
  `compare_3dmark05_perf_counters.py` reports replay, queue draw-submission,
  snapshot, snapshot-cache lookup, encode, and no-enqueue stage totals as
  per-present metrics. Use these gates when a candidate targets the serialized
  CPU path: `--require-commit-chunk-replay-cpu-per-present-decrease`,
  `--require-queue-draw-submission-cpu-per-present-decrease`,
  `--require-snapshot-cpu-per-present-decrease`,
  `--require-snapshot-cache-lookup-cpu-per-present-decrease`,
  `--require-encode-chunk-cpu-per-present-decrease`, and the
  `--require-no-enqueue-*` stage gates. Pair them with P4 gates before
  promoting a local CPU cleanup as an average-FPS fix. These flags also pass
  through `run_3dmark05_perf_probe.sh` and
  `finalize_3dmark05_perf_probe.sh` under the same
  `--compare-baseline-output` contract.
- **Present publish counters**:
  `submit_present_*_cpu_ms` and
  `prepare_slot_{publish,resource_mark,pso_prefetch}_cpu_ms` decide whether a
  Present record is display/acquire/boundary cost or serialized queue publish
  work. After [[present-pacing-publish-pso-prefetch.27]], the normal path should
  report `prepare_slot_pso_prefetch_cpu_ms=0` and non-zero
  `encode_slot_pso_prefetch_cpu_ms`; use `DXMT9_ENABLE_PUBLISH_PSO_PREFETCH=1`
  only to restore the legacy serialized placement for A/B or cold-PSO tests.
- **Completion-signal perturbation**:
  `DXMT9_PERF_COMPLETION_SIGNAL_DELAY_MS=N` delays completed-seq/waterline
  publication after `waitUntilCompleted()` returns. Confirm application through
  `completion_signal_delay` and `completion_signal_delay_ms`, then compare PE
  cadence and `completion_no_enqueue_wait_to_next_enqueue_*` against a baseline.
- **PE cadence telemetry**: with `DXMT9_PE_RECORDER_STATS=1` and
  `DXMT_LOG_LEVEL=info`, `pe_present_timing` measures PE `Present()` itself,
  `pe_present_next_call` measures the first PE D3D9 call after `Present`
  returns, `pe_present_call_milestone` samples the first calls in that
  post-Present sequence, `pe_present_call_return` measures selected call
  durations/return timestamps, and `pe_present_next_chunk` measures the first
  non-empty PE chunk crossing into unix after that return. This distinguishes
  "the app did not call D3D9" from "the app recorded PE-local work but
  chunk/Metal submission did not happen yet." `pe_present_record_milestone`
  names appendable record cadence; its `call` field is last-call context and
  may be stale when append happens through a helper/flush path. Current builds
  also add all-chunk `pe_recorder_stats` fields:
  `chunkFillGapSamples`, `chunkFillGapMs`, `chunkFillGapMaxMs`,
  `chunkFirstRecordGapSamples`, `chunkFirstRecordGapMs`,
  `chunkFirstRecordGapMaxMs`, `chunkActiveFillSamples`,
  `chunkActiveFillMs`, `chunkActiveFillMaxMs`,
  `chunkInterAppendGapSamples`, `chunkInterAppendGapMs`,
  `chunkInterAppendGapMaxMs`, `chunkBridgeSamples`, `chunkBridgeMs`,
  `chunkBridgeMaxMs`, `recordAppendCalls`, `recordAppendCpuMs`,
  `recordAppendCpuMaxMs`, `recordAppendNoFlushCalls`,
  `recordAppendNoFlushCpuMs`, `recordAppendNoFlushCpuMaxMs`, and
  `interAppendTop{1..4}{PrevType,Prev,NextType,Next,Samples,Ms,MaxMs}`.
  `chunkFillGapMs` measures time from the previous PE `commit_chunk` bridge
  return to the next PE flush entry, while `chunkBridgeMs` measures the
  synchronous `dxmt9c_device_commit_chunk` call duration.
  `chunkFirstRecordGapMs` splits the idle/front half between chunk return and
  the first appendable record in the next chunk, `chunkActiveFillMs` splits
  the first-record-to-flush span, `chunkInterAppendGapMs` splits same-chunk
  wall gaps between append returns and the next append entry, and
  `recordAppendNoFlushCpuMs` is the clean append CPU bucket. `recordAppendCpuMs`
  also includes capacity-flush append calls, so it can include synchronous
  bridge/replay time and should not be used as the active-fill closure term.
  The `interAppendTop*` fields rank same-chunk previous-record -> next-record
  pairs by total wall time and are the first cut for choosing which producer
  materialization path to split next. Current builds also emit focused
  `gapDrawIndexed{VsConstF,ApplyState,DrawIndexed,PsConstF}PhaseSamples`,
  `PreCallMs`, `PreCallMaxMs`, `InsideCallMs`, and `InsideCallMaxMs` fields
  in `pe_recorder_gap_call_stats`; these split the same focused pairs at the
  next PE D3D9 call entry so pre-call producer cadence is not mistaken for
  helper-body CPU. `pe_recorder_gap_tail_stats` then adds focused
  `TailSplitSamples`, `PrevCallTailMs`, `PrevCallTailMaxMs`, `BetweenCallsMs`,
  and `BetweenCallsMaxMs` fields; these split H68's pre-call phase into
  previous draw-call tail and draw-return-to-next-call-entry time. The follow-up
  `pe_recorder_gap_between_call_stats` line adds
  `BetweenTop{1,2}CallFamily` and `BetweenTop{1,2}Samples` fields for the same
  focused pairs; the summary renders these as the focused between-calls entry
  family table so an apparently idle gap can be separated from real PE D3D9
  producer traffic. The same line also carries
  `BetweenTop{1,2}CallName` and `BetweenTop{1,2}CallNameSamples` fields; these
  keep exact buckets for hot device/child calls such as
  `SetVertexShaderConstantF`, `IndexBuffer::GetDesc`, and
  `Surface::GetDesc`.
  Use these with H60 before blaming queue publish or Metal completion for
  inter-replay gaps.
- **winemac OnMainThread threshold telemetry**: not yet implemented in dxmt9.
  The next P4 probe should instrument Wine's `OnMainThread()` and candidate
  wrappers (`ClipCursor`, cursor get/set, window-frame, metal-layer getter) with
  caller tag, queue-to-start ms, body ms, and app-thread id. Join the rows to
  PE `SetRenderTarget` return / `Clear` entry milestones before claiming a
  winemac owner. The active 3DMark05 runtime uses an x86_64 unix-side
  `winemac.so`, while the local Wine build artifacts currently inspected are
  arm64, so runtime proof needs an x86_64 Wine driver build/replacement or a
  non-invasive `xctrace` sample. For the non-invasive route, run
  `run_3dmark05_system_trace_sidecar.sh --export-cpu-summary -- ...` so the
  trace exports required `time-profile` plus optional `time-sample` /
  `thread-info` and writes `xctrace-cpu-thread-summary.{csv,md}` plus
  `xctrace-cpu-thread-verdict.json`; the sidecar also prints
  `system_trace_cpu_summary_verdict:` on completion. PE `pe_present_*`
  milestone rows now carry `thread_id=0x...`; pass
  `--cpu-producer-from-pe-log` to have the sidecar enable PE recorder stats and
  pass the actual `3dmark05-direct.log` path to the CPU summary parser. Current
  builds also log `unix_commit_chunk_entry native_tid=0x...` from the unix replay
  boundary, and the parser prefers that native id before falling back to PE
  `thread_id=0x...`; use `--cpu-producer-thread-regex REGEX` when a selector has
  already been proven.
  The summary matches selectors against both xctrace thread labels and
  `thread-info` `tid` when available. Treat PE-log ids as a same-run selector
  attempt, not as proof that PE and xctrace thread ids are identical; missing
  ids report `producer-thread-selector-missing`, and extracted ids that do not
  match xctrace rows or `tid` report `producer-thread-not-found`. The current
  same-run scout proves that PE `thread_id=0xd0` does not match xctrace native
  thread ids, so PE-log fallback selection remains non-decisive without native
  mapping. Use
  `--require-cpu-p4-positive` only for a
  confirmation run that should fail unless the selected producer-thread verdict
  is `producer-wait-stack-positive`. Full `WINEDEBUG=+macdrv,+timestamp` is too
  noisy for final low-overhead evidence.
- **xctrace holder split**:
  `summarize_xctrace_cpu_threads.py` now reports producer wait keywords and
  CoreAnimation/Metal present holder keywords separately. Read
  `p4_wait_keyword_hits` / `producer_wait_keyword_hits` for the selected
  producer-thread `OnMainThread` question, then read `holder_status` and
  `main_thread_holder_keyword_hits` for `CA::Transaction`, `CAMetalLayer`,
  `presentDrawable`, or `nextDrawable` evidence on the Cocoa main thread. A
  holder-only positive is not a P4 owner by itself; it must align with PE
  milestones and producer wait evidence. [[present-pacing-xctrace-holder-summary.42]]
- **Env-knob A/B**: parent shell prefix
  (`DXMT9_LAYER_DISPLAY_SYNC=0 bash scripts/tools/run_3dmark05_perf_probe.sh
  --no-gputrace --suffix <name>`). The wrapper's `env "${env_args[@]}"
  ${cmd[@]}` invocation has no `-i`, so parent env propagates through to
  the Wine app.
- **Decisive scalar**: `process_elapsed_sec` (run wallclock). 3DMark05 GT1
  renders a fixed-scene workload, so faster scene completion = higher
  internal fps. Cross-checked with `completion_waits` (CB count) — same
  workload should produce roughly the same CB count.
- **CB-budget arithmetic**: `encode_draw_cpu_ms / completion_waits`
  yields per-CB encode cost; `gpu_command_buffer_time_ms / completion_waits`
  yields per-CB GPU cost. If per-CB encode + GPU > 16.67 ms (60 Hz slot),
  the frame misses vsync.

## Experiment dependency graph

```mermaid
flowchart TD
  classDef accepted fill:#d6f5d6,stroke:#2b7a2b,color:#063
  classDef open fill:#fff3cd,stroke:#a80,color:#640
  classDef rejected fill:#f8d7da,stroke:#a33,color:#600
  classDef proposed fill:#e0e7ff,stroke:#445588,color:#102

  DSync["display-sync.01\n100% present wait;\nDSync=0 → 2.99× scene throughput\n(diagnostic only)"]
  CurrentImmediate["current-immediate.02\nGT1 direct already immediate;\nDISABLE_VSYNC flat"]
  CompletionStatus["completion-watcher-status.03\nno pending backlog;\nwait mostly from Committed CB"]
  CurrentOwner["current-fps-owner.04\nFPS owner split\nP2/P3 CPU cadence + P4 wait"]
  PipelineOverlap["pipeline-overlap.05\n1799/1799 waits have no later enqueue\npublish p50 16.6ms, commit p50 36.5ms after wait"]
  BoundaryLatency["boundary-latency-ab.06\nDISABLE_PRESENT_BOUNDARY / latency6 flat\nexplicit boundary not owner"]
  PrePublish["prepublish-stage.07\ncommit_chunk entry p50 1.0ms\npublish p50 15.9ms\nreplay/submit owner"]
  StageDelta["stage-delta.08\nsame-sample split\nentry→publish p50 6.2ms\nencode→commit p50 11.4ms"]
  PePresent["pe-present-timing.09\nPE Present p50 2.6ms\ncompletion wait p50 28.4ms\nPresent-block rejected"]
  PeCallCadence["pe-call-cadence.10\nnext PE call p50 0.31ms\n1702/1703 BeginScene\napp-call wait rejected"]
  PeChunkCadence["pe-chunk-cadence.11\nfirst chunk p50 19.9ms\ncapacity_post 64 records\nchunk-fill cadence accepted"]
  PeChunkSize["pe-chunk-size-ab.12\n64→32 capacity\nfirst chunk p50 19.9→19.0ms\nno enqueue overlap"]
  PeRecordMilestones["pe-record-milestones.13\nBeginScene p50 0.31ms\nrecord1 p50 18.1ms\nrecord64 p50 19.7ms"]
  PeCallSequence["pe-call-sequence.14\ncalls 1..4 <=0.53ms\nClear omitted\nsuperseded"]
  PeClearGate["pe-clear-gate.15\nSetRT return p50 0.58ms\nClear p50 18.41ms\nrecord1 inside Clear"]
  PeClearNoSampling["pe-clear-nosampling.16\nframe sampling off\nperf-frame lines 1726->0\ngap p50 17.65->17.65ms"]
  PeWideCall["pe-wide-call-coverage.17\nSurface::GetDesc / Texture::GetSurfaceLevel visible\nlast return p50 0.674ms\nClear p50 18.421ms"]
  PeThreadState["xctrace-threadstate.18\nproducer 15.354s Running / 15.563s\nrunloop sleep weakened\nexact PE PC open"]
  PeCallerPc["pe-caller-pc.19\nsteady wrapper PCs 1716/1716\nSetRT wrapper RVA 0x2AF4F\nClear wrapper RVA 0x2B061\ngap p50 17.484ms"]
  PeCallerStack["pe-caller-stack.20\nhigher caller frame 0x88760\ncommand dispatcher 0x4886E0\nSetRT→Clear gap p50 17.429ms"]
  WinemacAudit["winemac-onmainthread.28\nOnMainThread can block app thread\ncandidate macdrv path open"]
  CpuSummaryTool["xctrace-cpu-summary.29\nCPU summary sidecar tooling\nphase43 producer running"]
  CpuSummaryCurrent["xctrace-cpu-summary.30\nPE thread_id=0xd0 present\nno native xctrace tid match"]
  NativeSelector["native-selector-xctrace.31\nnative_tid selects producer\nproducer running; wait hits 0"]
  NativeSelectorDefault["native-selector-xctrace.32\ndefault-on PSO memo\nproducer still running; wait hits 0"]
  SystemTraceP4Smoke["systemtrace-p4-smoke.34\n2s sidecar joins 306/306 rows\nproducer running; wait hits 0"]
  CurrentP4Sidecar["systemtrace-p4-current.35\n2s sidecar joins 386/386 rows\nwait hits 0; 1 blocked sample"]
  RangeP4Sidecar["systemtrace-p4-range.36\nseq-range sidecar joins 395/395\nproducer running; wait hits 0"]
  CompareGate["compare-gates.37\nP4 A/B gates\npresent wait vs overlap vs no-enqueue"]
  SerialCompareGate["serial-stage-compare-gates.38\nP2/P3 per-present gates\nreplay/snapshot/encode/no-enqueue stages"]
  CurrentLowOverhead43["current-lowoverhead.43\npost cbuf-observer opt-in\nno-enqueue 26.84ms/present\nencode 10.90ms / replay 8.07ms"]
  DirectCbuf45["direct-cbuf.45\nargbuf table path removed\nencode 5.98ms/present\nP4/FPS flat"]
  CurrentP2P3Scout46["current-p2p3.46\ncurrent default scout\nno-enqueue 27.48ms/present\nencode 11.15ms / replay 8.33ms"]
  NoEnqueueBeforePublish47["noenqueue-beforepublish.47\np50 12 commit_chunks before first publish\nproducer absence rejected"]
  DrawChunkLimit48["drawchunk-limit.48\nbefore-publish chunks are draw/const heavy\nlimit64 creates overlap but explodes CB/pass/tile cost"]
  CurrentLowOverhead49["current-lowoverhead.49\npost capture-layer repair scout\nno-enqueue 27.72ms/present\nencode 11.35ms / replay 8.52ms"]
  DrawChunkLimit50["drawchunk-limit-sweep.50\nlimit256 creates overlap\nbut CB/pass/GPU/encode cost rises\nFPS flat"]
  OverlapLocalityGate51["overlap-locality-gates.51\nP4 gates now require\nno CB/pass/tile increase"]
  ActiveReplay54["noenqueue-active-replay.54\nactive present chunk ~0ms\nfirst-publish residual remains"]
  InterReplayGap55["noenqueue-inter-replay-gap.55\ninter-replay producer gap\n~76.7% of first-publish row"]
  PeChunkCadence56["pe-chunk-cadence-all.56\nPE fill gap accounts for\ninter-replay residual"]
  PeChunkFill57["pe-chunk-fill-split.57\nfirst-record + active-fill\nsplit closes fill gap"]
  PeActiveFill58["pe-active-fill-split.58\nactive fill is mostly\ninter-append producer wall time"]
  PePairs59["pe-inter-append-pairs.59\ndraw->const/apply pairs\nexplain top gap"]
  PeConstApply60["pe-const-apply-split.60\nconst/apply leaf bodies\ndo not own pair gaps"]
  PeHotSetter61["pe-hotsetter-split.61\nall hot setters only\n0.729ms/present"]
  PeGapCallFamily62["pe-gap-callfamily.62\nappend source resolved\ndraw/barrier helper"]
  PeGapPhase63["pe-gap-phase-split.63\nfocused gaps mostly\npre-call producer cadence"]
  PeGapTail64["pe-gap-tail-split.64\npre-call is mostly\nbetween D3D9 calls"]
  PeBetweenCalls65["pe-between-call-family.65\nbetween-calls filled by\nD3D9 producer entries"]
  PeBetweenCallNames66["pe-between-call-name.66\nVS const setters +\nIB desc getters named"]
  PeDescCache67["pe-desc-cache.67\nchild desc cache cleanup\nP2/P3/P4 flat"]
  RunAheadDesign68["run-ahead-design.68\nlogical readiness must decouple\nfrom Metal CB publication"]
  RunAheadCoalesce69["run-ahead-coalesce.69\npresent wait removed\noverlap created\nCB locality failed"]
  RunAheadCpuReady70["run-ahead-cpu-ready.70\nCB/sub-CB shape improved\npresent wait near zero\nreplay/total wait regressed\nblack vertical artifact"]
  CompletionSignalDelay["completion-signal-delay.21\n8ms x1696 completion-signal delay\nSetRT→Clear and first chunk flat"]
  PeClearFlush["pe-clear-flush.22\nClear publishes 2-record chunk\nfirst chunk 20.6→18.9ms\nno enqueue overlap"]
  PeClearFlushRefresh["pe-clear-flush.23\ncurrent low-overhead refresh\nfirst chunk 20.7→19.1ms\nFPS flat/worse"]
  LowOverheadSerial["lowoverhead-serial.24\nnext unix entry ~0.9ms\npublish/encode still serial\nStage1 CPU win no FPS"]
  SubCBCap["subcb-cap.25\ncap 4→8 doubles sub-CBs\nFPS flat/worse\nnext enqueue p50 worse"]
  FrameLatency["frame-latency.01\nMAX_FRAME_LATENCY=3 + CAP=0\n(rejected: Δ +0.07%)"]
  AsyncAcq["async-acquire.01\nPRESENT_ASYNC_ACQUIRE=1\n(rejected: axis < 0.5% of wait)"]
  EncodeBudget["encode-budget.01\nencode_chunk p50 20.45 ms\nvs 16.67 ms vsync slot\n(attribution accepted)"]
  FixProposal["encode-budget-fix-proposal.01\nhistorical proposal\nbind-cache part superseded"]
  WorkA["bind-cache-work-a.01\n5 bind classes tested\n(rejected: 0% wallclock)"]
  Attribution["state-churn encode-phase.01\nargbuf 5.07s;\nbinding packet 2.57s"]
  NextSplit["state-churn encode-phase.02\ncbuf mirror 3.92s;\npacket cache 1.87s"]
  CleanGate["state-churn encode-phase.03\nclean cbuf gate\n272k skips / micro-win"]
  ReopenMask["state-churn encode-phase.04\n0 repoints;\n760k no-dirty hash mismatches"]
  Identity["state-churn encode-phase.05\ncategory identity repoint\nencode_draw -3.42s"]
  IdentitySmoke["state-churn encode-phase.06\ncurrent smoke\nnormal GT1 frame"]
  PacketSplit["state-churn encode-phase.07\npacket cache split\nprobe/key hot"]
  PlanDirect["state-churn encode-phase.08\nplan-direct cache\nencode_draw -1.16s"]
  SnapshotSplit["snapshot-cache snapshot.04\ncache lookup 18.1s\ncopy/override small"]
  SnapshotHash["snapshot-cache snapshot.05\ncomponent-hash reuse\nsnapshot/present -39.2%"]
  StateNoop["snapshot-cache snapshot.06\nstate-set no-op guard\n0 hits (rejected)"]
  PayloadSplit["snapshot-cache snapshot.07\npayload split\nhashDrawUniformPayload 9.75s"]
  UsageHash["snapshot-cache snapshot.08\nusage-aware payload hash\nhash/build 11.32→2.59us"]
  FfpZero["snapshot-cache snapshot.09\nFFP known-zero usage\nPS full 84k→0\nhash/build 2.59→2.08us"]
  ArgbufOpen["state-churn encode-phase.09\nargbuf open split\nreserve 746ms / table skip 0"]
  FastAppend["state-churn encode-phase.10\ntransient fast append\nreserve -51.95% / encode_draw -3.87%"]
  DirtyIdentity["state-churn encode-phase.11\ndirty identity probe\n0 hits / rejected"]
  StreamBindSplit["state-churn encode-phase.12\nstream bind split\ntexture 1065ms / index 670ms\nshader 497ms / raster 389ms"]
  TextureSplit["state-churn encode-phase.13\ntexture sampler split\nfragment resolve 575ms\nfragment direct 317ms"]
  SamplerSkip["state-churn encode-phase.14\nsampler pre-handle skip\n2.11M lookups avoided"]
  SamplerHash["state-churn encode-phase.15\nsampler-state hash reuse\nfragment direct -68ms"]
  TexturePreResolve["state-churn encode-phase.16\ntexture pre-resolve skip\nrejected parent +48ms"]
  Remaining["remaining no-gputrace A/B\nargbuf structure / binding / stream / issue\nplus nonconst / VS indexed fallback"]
  SCE["state-churn-encode\n(implementation owner)"]

  DSync --> FrameLatency
  DSync --> AsyncAcq
  DSync --> CurrentImmediate
  CurrentImmediate --> CompletionStatus
  CompletionStatus --> CurrentOwner
  CurrentOwner --> PipelineOverlap
  PipelineOverlap --> BoundaryLatency
  BoundaryLatency --> PrePublish
  PrePublish --> StageDelta
  StageDelta --> PePresent
  PePresent --> PeCallCadence
  PeCallCadence --> PeChunkCadence
  PeChunkCadence --> PeChunkSize
  PeChunkSize --> PeRecordMilestones
  PeRecordMilestones --> PeCallSequence
  PeCallSequence --> PeClearGate
  PeClearGate --> PeClearNoSampling
  PeClearNoSampling --> PeWideCall
  PeWideCall --> PeThreadState
  PeThreadState --> PeCallerPc
  PeCallerPc --> PeCallerStack
  PeCallerStack --> WinemacAudit
  WinemacAudit --> CpuSummaryTool
  CpuSummaryTool --> CpuSummaryCurrent
  CpuSummaryCurrent --> NativeSelector
  NativeSelector --> NativeSelectorDefault
  NativeSelectorDefault --> SystemTraceP4Smoke
  SystemTraceP4Smoke --> CurrentP4Sidecar
  CurrentP4Sidecar --> RangeP4Sidecar
  RangeP4Sidecar --> CompareGate
  CompareGate --> SerialCompareGate
  SerialCompareGate --> CurrentLowOverhead43
  CurrentLowOverhead43 --> DirectCbuf45
  DirectCbuf45 --> CurrentP2P3Scout46
  CurrentP2P3Scout46 --> NoEnqueueBeforePublish47
  NoEnqueueBeforePublish47 --> DrawChunkLimit48
  DrawChunkLimit48 --> CurrentLowOverhead49
  CurrentLowOverhead49 --> DrawChunkLimit50
  DrawChunkLimit50 --> OverlapLocalityGate51
  OverlapLocalityGate51 --> ActiveReplay54
  ActiveReplay54 --> InterReplayGap55
  InterReplayGap55 --> PeChunkCadence56
  PeChunkCadence56 --> PeChunkFill57
  PeChunkFill57 --> PeActiveFill58
  PeActiveFill58 --> PePairs59
  PePairs59 --> PeConstApply60
  PeConstApply60 --> PeHotSetter61
  PeHotSetter61 --> PeGapCallFamily62
  PeGapCallFamily62 --> PeGapPhase63
  PeGapPhase63 --> PeGapTail64
  PeGapTail64 --> PeBetweenCalls65
  PeBetweenCalls65 --> PeBetweenCallNames66
  PeBetweenCallNames66 --> PeDescCache67
  PeDescCache67 --> RunAheadDesign68
  RunAheadDesign68 --> RunAheadCoalesce69
  RunAheadCoalesce69 --> RunAheadCpuReady70
  OverlapLocalityGate51 --> LowOverheadSerial
  PeCallerStack --> CompletionSignalDelay
  CompletionSignalDelay --> PeClearFlush
  PeClearFlush --> PeClearFlushRefresh
  PeClearFlushRefresh --> LowOverheadSerial
  LowOverheadSerial --> SubCBCap
  PrePublish --> EncodeBudget
  StageDelta --> EncodeBudget
  PeChunkCadence --> EncodeBudget
  PeChunkSize --> EncodeBudget
  PeRecordMilestones --> EncodeBudget
  PeCallSequence --> EncodeBudget
  PeClearGate --> EncodeBudget
  PeClearNoSampling --> EncodeBudget
  PeWideCall --> EncodeBudget
  PeClearFlush --> EncodeBudget
  LowOverheadSerial --> EncodeBudget
  SubCBCap --> EncodeBudget
  PipelineOverlap --> EncodeBudget
  FrameLatency --> EncodeBudget
  AsyncAcq --> EncodeBudget
  EncodeBudget --> FixProposal
  FixProposal --> WorkA
  WorkA --> Attribution
  Attribution --> NextSplit
  NextSplit --> CleanGate
  CleanGate --> ReopenMask
  ReopenMask --> Identity
  Identity --> IdentitySmoke
  IdentitySmoke --> PacketSplit
  PacketSplit --> PlanDirect
  PlanDirect --> SnapshotSplit
  SnapshotSplit --> SnapshotHash
  SnapshotHash --> StateNoop
  StateNoop --> PayloadSplit
  PayloadSplit --> UsageHash
  UsageHash --> FfpZero
  FfpZero --> ArgbufOpen
  ArgbufOpen --> FastAppend
  FastAppend --> DirtyIdentity
  DirtyIdentity --> StreamBindSplit
  StreamBindSplit --> TextureSplit
  TextureSplit --> SamplerSkip
  SamplerSkip --> SamplerHash
  SamplerHash --> TexturePreResolve
  TexturePreResolve --> Remaining
  Remaining --> SCE

  class DSync,EncodeBudget,CurrentImmediate,CompletionStatus,CurrentOwner,PipelineOverlap,PrePublish,StageDelta,PeChunkCadence,PeRecordMilestones,PeClearGate accepted
  class FrameLatency,AsyncAcq,WorkA,StateNoop,DirtyIdentity,TexturePreResolve,BoundaryLatency,PePresent,PeCallCadence,PeChunkSize,PeCallSequence,PeClearFlush,PeClearFlushRefresh,SubCBCap,DrawChunkLimit50,PeDescCache67,RunAheadCoalesce69,RunAheadCpuReady70 rejected
  class LowOverheadSerial accepted
  class PeClearNoSampling,PeWideCall,PeThreadState rejected
  class PeCallerPc,PeCallerStack,CompletionSignalDelay accepted
  class FixProposal,Remaining,SCE,WinemacAudit,CpuSummaryCurrent proposed
  class Attribution,NextSplit,CleanGate,ReopenMask,Identity,IdentitySmoke,PacketSplit,PlanDirect,SnapshotSplit,SnapshotHash,PayloadSplit,UsageHash,FfpZero,ArgbufOpen,FastAppend,StreamBindSplit,TextureSplit,SamplerSkip,SamplerHash accepted
  class CpuSummaryTool,CompareGate,SerialCompareGate,CurrentLowOverhead43,DirectCbuf45,CurrentP2P3Scout46,NoEnqueueBeforePublish47,DrawChunkLimit48,OverlapLocalityGate51,PeChunkCadence56,PeChunkFill57,PeActiveFill58,PePairs59,PeConstApply60,PeHotSetter61,RunAheadDesign68 accepted
```

## Detail map

- [[present-pacing-display-sync.01]] — Display-sync attribution; 100% of
  `completion_wait_ms` is Present-bearing; `DXMT9_LAYER_DISPLAY_SYNC=0`
  cuts scene wallclock 251 s → 83 s (−66.9%, ~3× fps). Diagnostic only,
  not a production fix.
- [[present-pacing-frame-latency.01]] — REJECTED.
  `MAX_FRAME_LATENCY=3 + CAP=0` does not move wallclock under vsync
  (Δ +0.07%); the compositor paces presents at refresh rate regardless
  of queue depth.
- [[present-pacing-async-acquire.01]] — REJECTED.
  `PRESENT_ASYNC_ACQUIRE=1` reduces `present_acquire_wait_ms` by 37.5%
  but that axis is < 0.5% of the total wait budget; wallclock unchanged
  (Δ +0.22%); encode CPU rose +8.3% (added to encode thread).
- [[present-pacing-encode-budget.01]] — HISTORICAL SIZING.
  Per-chunk encode CPU p50 = 20.45 ms vs 16.67 ms vsync budget;
  average draw-run = 1.88 records vs cap of 32. Its broad bind-call
  attribution is superseded by [[present-pacing-bind-cache-work-a.01]] and
  [[state-churn-encode-encode-phase.01]].
- [[present-pacing-encode-budget-fix-proposal.01]] — PROPOSED.
  Synthesis of Steps 1-4. Two concrete work items:
  (A) extend `_skipped` cache to 7 currently-uncached bind classes;
  (B) reduce `mixed_pair_stream_*` draw-run breaks. Conservative
  expected wallclock impact +44% on 3DMark05 GT1; ceiling +199% if
  matching DSync=0. Acceptance criterion:
  `encode_chunk_cpu_p50_ms ≤ 16.67 ms`. Implementation lives in
  [[state-churn-encode]].
- [[present-pacing-bind-cache-work-a.01]] — REJECTED (mechanism not
  the right lever). Work A landed for 5 classes (vertex_buffer,
  pipeline, depth_state, viewport, scissor) — commits e07cbfe +
  5eef5d4. On 3DMark05 GT1: wallclock 251.07 → 251.07 s (no change),
  `bind_*` counts essentially unchanged across all five classes,
  `encode_chunk_cpu_ms` +12.7% (added comparison overhead). Bind
  diversity is genuinely high per-draw, so the shadow cache rarely
  hits. The infrastructure stays in place for future use but the
  proposal's expected gain doesn't materialise on this workload.
- [[state-churn-encode-encode-phase.01]] — ACCEPTED ATTRIBUTION.
  A watchdog-supervised no-gputrace run keeps the workload shape stable and
  splits the old encode-draw remainder: `encode_draw_argbuf_setup_cpu_ms =
  5.07s` (31%) and `encode_draw_binding_packet_cpu_ms = 2.57s` (15.7%).
  The timers are not fully exclusive, but the next CPU work should split these
  two buckets before another mutating implementation bet.
- [[state-churn-encode-encode-phase.02]] — ACCEPTED ATTRIBUTION.
  Child counters split the hot buckets further: dirty cbuf mirror
  `3.92s`, binding-packet cache lookup/store `1.87s`, and argbuf
  open/repoint `1.21s`. The run shape stayed stable (`draws +0.03%`,
  GPU CB +0.26%), so these are the next no-gputrace CPU A/B targets.
- [[state-churn-encode-encode-phase.03]] — ACCEPTED MICRO / REJECTED AS
  MAJOR LEVER. A clean/no-op dirty-mask gate skipped `272,956`
  `updateDirtyArgbufRegions()` calls, but only saved `39.7ms` in the cbuf
  update bucket and `91.4ms` in total `encode_draw_cpu_ms`; the remaining
  `778,587` dirty/write calls are the load-bearing path.
- [[state-churn-encode-encode-phase.04]] — ACCEPTED ATTRIBUTION / REJECTED
  AS CURRENT OPTIMIZATION. Dirty-bit-only partial repoint produced `0`
  cached repoints. The hot reopen case is `760,157` whole-payload hash
  mismatches with no dirty category bits, which forces conservative
  VS/PS/FFPPS upload. The follow-up category identity implementation is
  recorded in [[state-churn-encode-encode-phase.05]].
- [[state-churn-encode-encode-phase.05]] — ACCEPTED CPU WIN. Category identity
  repoint turns the reopen-mask attribution into an implementation win:
  `encode_draw_cpu_ms` -3.42s (`-17.74%`), transient upload bytes `-62.05%`,
  dirty cbuf calls `-41.88%`, with only `91.9ms` of identity probe cost. This
  remains a no-gputrace CPU result, not a GPU/fps claim.
- [[state-churn-encode-encode-phase.06]] — ACCEPTED SMOKE. A fresh current run
  keeps the identity counter shape (`cached_repoint_calls` +0.02% vs
  identity-r1) and writes a normal visible GT1 `actual.png` frame. This is
  useful visual smoke, not same-input exact image proof.
- [[state-churn-encode-encode-phase.07]] — ACCEPTED ATTRIBUTION. The
  binding-packet cache split keeps the run shape stable and shows
  `probe/equality=939ms`, `key build=495ms`, hit rate 85.51%, and collision
  share of misses 99.92%. The extra timers raise parent CPU counters, so this
  is an attribution result only. The next CPU A/B should reduce compact packet
  identity/probe cost before trying broad cache-size changes.
- [[state-churn-encode-encode-phase.08]] — ACCEPTED CPU WIN. Plan-direct
  binding-packet cache lookup removes the separate key object from the hot path:
  `encode_draw_binding_packet_cache_cpu_ms` -57.34% and `encode_draw_cpu_ms`
  -1.16s (`-7.40%`) versus current identity smoke. The visual smoke is normal,
  but this is still a no-gputrace CPU result rather than a vsync-on fps proof.
- [[snapshot-cache-snapshot.04]] — ACCEPTED ATTRIBUTION. The next PE-side
  bucket is not copy/override work: `d3d9_snapshot_cache_lookup_cpu_ms =
  18.085s`, or `94.08%` of `d3d9_snapshot_draw_submission_cpu_ms`. The next
  no-gputrace implementation target should split or redesign
  `cachedBaseDrawState*()` lookup before spending Xcode budget.
- [[snapshot-cache-snapshot.05]] — ACCEPTED CPU WIN. Reusing uniform component
  hashes removes duplicated cache-lookup hashing: normalized snapshot CPU drops
  `13.603ms→8.269ms` per present and lookup CPU drops `12.807ms→7.463ms` per
  present. The watchdog run processed more presents before timeout, but GPU and
  completion-wait time per present stayed flat, so this is not yet a vsync-on
  fps proof.
- [[snapshot-cache-snapshot.06]] — REJECTED. A temporary same-value D3D9
  state-set guard produced `0` no-op hits across all guarded categories, and
  normalized snapshot CPU stayed flat. Do not spend the next encode-budget work
  on broad D3D9 setter skips without first proving non-zero hits.
- [[snapshot-cache-snapshot.07]] — ACCEPTED ATTRIBUTION. Splitting
  `makeDrawUniformPayloadFromState()` shows the remaining payload-build owner is
  the first full `hashDrawUniformPayload()` pass: `9.75s`, `11.322us` per build,
  about `85.75%` of the combined parent payload-build bucket. Constant copies
  and FFP/texture/clip construction are secondary.
- [[snapshot-cache-snapshot.08]] — ACCEPTED CPU WIN. Shader-usage/range-aware
  payload hashing cuts the hot hash pass from `11.322us` to `2.590us` per build
  and combined parent payload build from `13.204us` to `4.372us` per build.
  Full payload equality still guards handle reuse; lookup collision telemetry is
  acceptable for this run. `encode_draw_cpu_ms`, GPU CB time, and
  `completion_wait_ms` remain flat per present, so this is not yet a
  vsync-on fps proof.
- [[snapshot-cache-snapshot.09]] — ACCEPTED CPU WIN. Fallback reason counters
  prove the bytecode scanner is not failing (`*_unknown_bytecode=0`); all PS
  full fallback was non-bytecode/FFP. Treating non-bytecode programmable
  constant usage as known-zero removes PS full fallback (`84,380→0`) and cuts
  the hot hash pass from `2.590us` to `2.082us` per build. `encode_draw_cpu_ms`,
  GPU CB time, and `completion_wait_ms` remain flat/noisy per present, so this
  is still not a vsync-on fps proof. The same run leaves the current pacing CPU
  shape as `completion_wait_ms=39978.924`, `encode_draw_cpu_ms=17711.215`, and
  `d3d9_snapshot_draw_submission_cpu_ms=7196.881` over `1740` presents.
- [[state-churn-encode-encode-phase.09]] — ACCEPTED ATTRIBUTION. Splits
  `encode_draw_argbuf_open_cpu_ms`: transient table reservation is `745.942ms`,
  `MTLArgumentEncoder.setArgumentBuffer` is only `115.192ms`, slot-30 table bind
  is `192.913ms`, and table-bind skip is `0`. Do not chase slot-30 bind
  shadowing; future argbuf work needs structural table allocation/open reduction
  or narrower cache/repoint decision work.
- [[state-churn-encode-encode-phase.10]] — ACCEPTED CPU WIN. Transient arena
  fast append skips the live-allocation overlap scan only when the slab deque is
  non-wrapped. In a same-present no-gputrace A/B, `transient_upload_cpu_ms`
  drops `961.534 -> 223.304`, argbuf reserve drops `745.942 -> 358.422`
  (`-51.95%`), and `encode_draw_cpu_ms` drops `17593.130 -> 16911.650`
  (`-3.87%`). GPU time and completion wait remain flat/noisy, so this is still
  a CPU-budget win rather than a vsync-on fps proof.
- [[state-churn-encode-encode-phase.11]] — REJECTED. A temporary dirty-category
  identity repoint branch found `0` VS/PS/FFPPS hits across `19,769`
  partial-dirty reopen candidates and added `8.609ms` of probe overhead. The
  branch was removed; do not pursue dirty-category identity repoint for GT1.
- [[state-churn-encode-encode-phase.12]] — ACCEPTED ATTRIBUTION. The next
  `stream_bind` split is attribution-only because the run processed more
  presents and the extra timers perturb the parent path. It still names the
  child owners: texture/sampler `1065.369ms`, index `669.907ms`, shader stream
  `496.708ms`, raster/base-state `389.388ms`, FFP stream only `6.845ms`.
  Future no-gputrace work should split texture/sampler skip opportunities,
  index setup/source resolve, and shader-stream diversity before broad
  bind-cache guesses.
- [[state-churn-encode-encode-phase.13]] — ACCEPTED ATTRIBUTION. The
  texture/sampler child is fragment-stage only in GT1: fragment resolve
  `575.228ms`, fragment direct bind/shadow/set `316.761ms`, and resource-array,
  vertex texture, and LOD-bias lanes all `0`. Existing bind counters show
  texture skip share ~52.69% and sampler skip share ~92.05%, so the next
  specific bet is avoiding sampler cache lookup/materialization before a
  pre-handle shadow identity can prove the sampler bind will be skipped.
- [[state-churn-encode-encode-phase.14]] — ACCEPTED CPU WIN. The sampler
  pre-handle skip stores exact sampler identity in the bind shadow and skips
  `samplerStateFor()` before materializing a `MTLSamplerState` when identity
  matches. Same-present A/B: `sampler_lookup_skipped_prehandle=2,108,453`,
  remaining lookup calls `181,844`, `texture_sampler_bind_cpu_ms`
  `1099.703 -> 892.480`, and `encode_draw_cpu_ms` `17842.278 -> 17724.955`.
  GPU and completion wait stay flat/noisy.
- [[state-churn-encode-encode-phase.15]] — ACCEPTED CPU WIN. The binding packet
  now carries `samplerStateHash`, avoiding per-entry rehashing in the direct
  sampler shadow key. Default-profile same-present A/B:
  `fragment_direct_cpu_ms` `495.039 -> 426.614`, `texture_sampler_bind_cpu_ms`
  `892.480 -> 822.864`, and `encode_draw_cpu_ms` `17724.955 -> 17598.333`.
  Heavy direct-lane split counters are opt-in via
  `DXMT9_PERF_TEXTURE_SAMPLER_DIRECT_SPLIT=1`.
- [[state-churn-encode-encode-phase.16]] — REJECTED AS DEFAULT CPU WIN.
  Texture pre-resolve matching proves `1,206,015` skipped texture resolves and
  cuts local fragment resolve `186.426 -> 157.524`, but the texture/sampler
  parent regresses `822.864 -> 871.088`. A temporary default-off smoke proved
  the skip counter stayed `0`; the rejected branch/counter/env were then removed
  from the hot path, and the removed-branch default run returns
  `texture_sampler_bind_cpu_ms` to baseline (`822.864 -> 821.007`).
- [[present-pacing-vsync-off.01]] — ACCEPTED (production-shippable).
  `DXMT9_DISABLE_VSYNC=1` end-to-end A/B on the full GT1 workload:
  `process_elapsed_sec` 251.07 → 133.44 s (**−46.9%**, ~×1.88 fps),
  `status: pass`, same 1,439 CB count and same 4.3 s of GPU work as
  baseline. The earlier "+199%" diagnostic figure was inflated by a
  partial-workload side effect of the diagnostic env; the +88%
  full-workload figure is what to ship against. Trade-off: tearing,
  no display sync. Opt-in only.
- [[present-pacing-current-immediate.02]] — ACCEPTED REFINEMENT.
  Current GT1 direct no-gputrace A/B shows both default and
  `DXMT9_DISABLE_VSYNC=1` already schedule every present as immediate:
  `present_schedule_requested_immediate=1680`,
  `present_schedule_after_minimum_duration=0`, and
  `present_schedule_immediate=1680`. Frame sampling is flat
  (`wall_ms` p50 `56.562 → 56.450`, p95 `90.313 → 91.178`,
  sampled wall sum `103.546s → 103.537s`). For current GT1 direct,
  vsync-off is not load-bearing; the residual wait is immediate-present
  completion/compositor behavior below dxmt9's software duration branch.
- [[present-pacing-completion-watcher-status.03]] — ACCEPTED.
  Current GT1 direct completion watcher status split shows no pending
  completion backlog: `completion_pending_depth_max=0`,
  `completion_dequeue_age_p50_ms=0.041`, and
  `completion_dequeue_age_p95_ms=0.067`. The watcher pops almost every
  command buffer while it is still `Committed`
  (`completion_dequeue_status_committed=1672/1679`) and spends
  `39520.923ms` of `39698.863ms` waiting from that state. The wait owner is
  therefore below dxmt9's pending-completion queue.
- [[present-pacing-current-fps-owner.04]] — ACCEPTED GATE SPLIT.
  Current low-overhead GT1 scout (`1800` presents, no `.gputrace`, no encoder
  breakdown) keeps the normal FPS envelope but shows average wallclock is not
  GPU-execution-time-bound: sampled FPS p50/p95 `18.102/26.630`, frame wall
  p50/p95 `55.242/84.648ms`, `completion_present_wait_ms=25.091ms/present`,
  `gpu_command_buffer_time_ms=3.113ms/present`, immediate presents only,
  `present_boundary_wait_ms=0`, and `completion_pending_depth_max=0`. Work the
  average-FPS lane through P2/P3 CPU cadence plus P4 wait collapse; keep
  `.gputrace` for hot-frame GPU locality/ceiling proof.
- [[present-pacing-pipeline-overlap.05]] — ACCEPTED REFINEMENT.
  The current P4 wait is hard under-pipelined rather than hidden behind next
  work: `1799 / 1799` waits have no later command-buffer enqueue during
  `waitUntilCompleted()`, `completion_wait_with_enqueue_ms=0`,
  `completion_wait_without_enqueue_ms=44789.044`, and
  `completion_enqueue_while_waiting=0`. The stage run adds that next
  `CommitPublish`/`EncodeDequeue`/Metal commit/enqueue arrive only after
  completion, with p50 `16.645/20.116/36.470/36.502ms`. P2/P3 wins should now
  be judged by whether they create overlap (`completion_wait_with_enqueue_ms`)
  or shrink the exposed no-enqueue wait and post-wait publish/encode gap.
- [[present-pacing-boundary-latency-ab.06]] — REJECTED AS CURRENT OWNER.
  A fresh same-code baseline plus `DXMT9_DISABLE_PRESENT_BOUNDARY=1` and
  `DXMT9_MAX_FRAME_LATENCY=6 DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS=0` A/Bs
  show the env knobs are not the missing producer-overlap lever. Boundary-off
  reaches the runtime (`present_boundary_skipped=1740`), but all three runs keep
  `present_boundary_waits=0`, `completion_wait_without_enqueue_ms≈44s`, and
  sampled FPS p50 `17.8-18.0`. The next localization target is before
  `CommitPublish` or outside dxmt9's explicit boundary wait.
- [[present-pacing-prepublish-stage.07]] — ACCEPTED ATTRIBUTION.
  New no-enqueue stage probes show the app/Wine/PE side re-enters unix
  `commit_chunk` quickly after the completion wait ends: entry p50/p95 is
  `1.040/2.668ms`. The queue still publishes much later
  (`CommitPublish` p50/p95 `15.894/29.912ms`), and the run-level owners are
  `commit_chunk_replay_cpu_ms=18981.064`,
  `commit_chunk_queue_draw_submission_cpu_ms=8154.509`, and nested
  `d3d9_snapshot_draw_submission_cpu_ms=6636.191`. The average-FPS target is
  therefore unix replay/submit/snapshot plus backend encode, not app/Wine event
  pacing.
- [[present-pacing-stage-delta.08]] — ACCEPTED ATTRIBUTION.
  Same-sample no-enqueue stage counters remove the need to subtract independent
  wait-end percentile rows. Current GT1 still has no overlap
  (`completion_wait_with_enqueue_ms=0`, `1799` no-enqueue waits). The exposed
  path splits into `commit_chunk entry -> CommitPublish` p50/p95
  `6.172/28.101ms`, `CommitPublish -> EncodeDequeue` `2.535/5.086ms`, and
  `EncodeDequeue -> commandBuffer.commit()` `11.384/22.232ms`. Queue wake is
  secondary; the two primary CPU stages are pre-publish replay/submit/snapshot
  and backend encode.
- [[present-pacing-pe-present-timing.09]] — REJECTED PRESENT-BLOCK OWNER.
  PE `Present()` is not the whole completion wait: `Present total_ms`
  p50/p95/max is `2.580/5.077/22.659ms` while `completion_wait_ms`
  p50/p95/max is `28.419/39.576/52.217ms`. The next unix chunk crossing still
  happens quickly after completion, but that is not an app API-call timestamp.
- [[present-pacing-pe-call-cadence.10]] — REJECTED APP-CALL WAIT OWNER /
  ACCEPTED PE-LOCAL RECORDING SHAPE. The first PE call after successful
  `Present` is almost always `BeginScene` (`1702 / 1703` rows) and arrives
  with `entry_delta_ms` p50/p95/p99 `0.310/0.436/1.811ms`. The app starts
  next-frame PE work quickly; the reason `completion_wait_with_enqueue_ms`
  remains zero must be PE-recorder/unix submission boundary or later.
- [[present-pacing-pe-chunk-cadence.11]] — ACCEPTED ATTRIBUTION. The first
  PE call is fast, but the first non-empty PE chunk after `Present` is always
  a `capacity_post` flush of `64` records and crosses into unix at steady
  p50/p95 `19.908/34.810ms` after `Present` return. The first bridge itself
  is cheap (`bridge_ms` p50/p95 `0.504/0.617ms`), so the gap is PE-local chunk
  fill plus later replay/publish/encode, not app API-call absence.
- [[present-pacing-pe-chunk-size-ab.12]] — REJECTED SIMPLE KNOB. Lowering
  `DXMT9_PE_CHUNK_MAX_RECORDS` from `64` to `32` proves the chunk boundary is
  capacity-driven, but it does not create producer run-ahead:
  `completion_wait_with_enqueue_ms` stays `0.000`. First-chunk p50 only moves
  `19.908 -> 19.034ms`; replay and encode stay flat/noisy. Treat earlier
  publish as a targeted architecture experiment, not a global capacity default.
- [[present-pacing-pe-record-milestones.13]] — ACCEPTED ATTRIBUTION. The
  missing middle between fast `BeginScene` and late first chunk is the first
  appendable record: `BeginScene` p50 is `0.306ms`, but record 1 is an
  `apply_state` at p50 `18.061ms`; record 64 follows at `19.683ms`, and the
  first chunk follows at `19.706ms`. The front gate is not filling 64 records.
  It is the state/draw boundary that first materializes PE-local state into a
  chunk record.
- [[present-pacing-pe-call-sequence.14]] — SUPERSEDED COVERAGE GAP. Calls
  `1..4` after `Present` are early RT setup and all arrive at p50
  `<= 0.532ms`, but the apparent call 5 `SetVertexShaderConstantF` gap was
  incomplete because `Clear` and `EndScene` were not yet in the call milestone
  sequence.
- [[present-pacing-pe-clear-gate.15]] — ACCEPTED ATTRIBUTION. With `Clear` and
  `EndScene` included, steady call 5 is `Clear` at p50 `18.408ms`.
  `SetRenderTarget` returns at p50 `0.581ms` and lasts only `0.015ms`, so it is
  not the sleeper. The exposed gap is `SetRenderTarget` return -> `Clear`
  entry p50/p95 `17.635/30.489ms`; record 1 is `apply_state` inside `Clear`
  at p50 `18.554ms`; first chunk is `capacity_post` at p50 `20.400ms`.
- [[present-pacing-pe-clear-nosampling.16]] — REJECTED SAMPLING ARTIFACT.
  Removing `--frame-sampling` drops `dxmt9-perf-frame` lines from `1,726` to
  `0`, but the exposed `SetRenderTarget` return -> `Clear` entry gap stays
  p50 `17.651 -> 17.646ms`, and the first chunk stays p50
  `20.402 -> 20.386ms`. The frame summary log is not the gap owner.
- [[present-pacing-pe-wide-call-coverage.17]] — REJECTED CHILD-GETTER STALL.
  Wider coverage finds previously hidden early calls:
  `Surface::GetDesc`, `Texture::GetSurfaceLevel`, another `GetRenderTarget`,
  `SetRenderTarget`, and another `Surface::GetDesc`. Return logging rejects
  them as sleepers: the last `Surface::GetDesc` returns at p50 `0.674ms`, but
  `Clear` enters at p50 `18.421ms`; the last-return -> `Clear` gap is
  p50/p95 `17.656/30.638ms`.
- [[present-pacing-xctrace-threadstate.18]] — REJECTED BROAD RUNLOOP SLEEP /
  ACCEPTED CPU-RUNNING SHAPE. Re-exported `phase43` `Metal System Trace` CPU
  tables show thread `3DMark05.exe (0x3b1b5c)` with `15,354ms` Running sample
  weight across a `15.563s` trace. CA present request gaps are p50/p95
  `74.398/98.436ms`, request -> presented handler p50/p95 is
  `33.637/42.348ms`, and `runloop-events` has only two `3DMark05.exe` rows on
  a different main thread. This does not identify the exact current
  `SetRenderTarget` -> `Clear` PC, but weakens broad app/Wine runloop sleep as
  the front-gap owner.
- [[present-pacing-pe-caller-pc.19]] — REJECTED HIDDEN DXMT9 API WAIT /
  ACCEPTED WRAPPER-LEVEL GAP. Caller-PC/module logging on the existing
  `DXMT9_PE_RECORDER_STATS=1` path shows the steady post-`Present` call
  sequence is identical in `1,716 / 1,716` ordinals. `SetRenderTarget`
  returns from 3DMark05.exe wrapper RVA `0x2AF4F` at p50 `0.730ms`, the nested
  `Surface::GetDesc` resolves to `d3d9.dll!0x13EE9` and takes only p50
  `0.020ms`, and `Clear` enters from 3DMark05.exe wrapper RVA `0x2B061` at
  p50 `18.373ms`. Caller-stack follow-up supersedes the higher-owner claim:
  these PCs are D3D wrapper stubs.
- [[present-pacing-pe-caller-stack.20]] — ACCEPTED COMMAND-DISPATCH CADENCE.
  PE stack logging shows milestones 2..8 share higher frame
  `3DMark05.exe+0x88760` in `1,707 / 1,707` matching ordinals. Disassembly
  identifies `0x88760` as the return site of command-object dispatcher
  `0x4886E0`, where virtual `call *0x18(%eax)` executes D3D wrapper commands.
  `SetRenderTarget` return -> `Clear` entry remains p50 `17.429ms`, so the P4
  front gate is when 3DMark05 dispatches the record-producing `Clear` command
  object, not a dxmt9 boundary/latency/ring wait.
- [[present-pacing-completion-signal-delay.21]] — REJECTED DXMT9 COMPLETION
  SIGNAL DEPENDENCY. `DXMT9_PERF_COMPLETION_SIGNAL_DELAY_MS=8` applies
  `1696` sleeps / `13568ms` after `waitUntilCompleted()` and before
  completed-seq publication, but PE cadence remains flat:
  `SetRenderTarget -> Clear` p50 `17.631 -> 17.550ms`, record 1 p50
  `19.025 -> 19.011ms`, first chunk p50 `20.802 -> 20.827ms`, and
  next-enqueue p50 `21.558 -> 20.274ms`. This rejects a dxmt9
  completed-seq/waterline dependency; it does not test a lower actual Metal/CA
  completion dependency because the perturbation occurs after Metal completion.
- [[present-pacing-pe-clear-flush.22]] — REJECTED SIMPLE EARLY-PUBLISH LEVER.
  `DXMT9_PE_FLUSH_AFTER_CLEAR=1` proves an earlier post-`Clear` publish is
  possible: the first chunk changes from `capacity_post` / `64` records to
  `clear` / `2` records and first-chunk p50 moves `20.582 -> 18.935ms`. It
  still creates no producer overlap (`completion_wait_with_enqueue_ms=0.000`,
  `completion_enqueue_while_waiting=0`) and raises PE commit count
  `41947 -> 45857`, so it stays diagnostic-only.
- [[present-pacing-pe-clear-flush.23]] — REJECTED CURRENT REFRESH. Matching
  low-overhead recorder-stats runs after the latest encode/copy work preserve
  the rejection: first chunk p50 still moves earlier (`20.710 -> 19.089ms`) and
  shrinks to `2` records, but `completion_wait_with_enqueue` does not improve
  (`2 -> 0`), tail-600 FPS p50 is flat/worse (`15.788 -> 15.681`), and
  `commitCount` rises (`41,429 -> 45,617`). Do not promote clear flush to
  `perf`; it remains diagnostic-only.
- [[present-pacing-lowoverhead-serial.24]] — ACCEPTED CURRENT ATTRIBUTION.
  The latest low-overhead Stage 2 baseline corrects the "app does not issue
  N+1" framing: completion end -> next unix `commit_chunk` entry is only
  `0.861ms` p50. The exposed time is then spent in serial P2/P3 stages:
  `commit_chunk` entry -> `CommitPublish` `14.068ms` p50, publish ->
  `EncodeDequeue` `3.678ms`, and `EncodeDequeue` -> Metal commit `11.528ms`.
  Disabling Stage 2 argbuf proves a large local encode CPU win is not enough:
  `encode_draw_cpu_ms / present` falls `9.388 -> 6.143ms`, but
  `completion_wait_ms / present` rises `27.116 -> 31.148ms` and sampled FPS is
  flat. Average-FPS promotion now requires P4/overlap movement, not just a
  smaller local CPU bucket.
- [[present-pacing-subcb-cap.25]] — REJECTED CURRENT FPS LEVER. Raising
  `DXMT9_MID_CHUNK_COMMIT_CAP_PER_RENDER_PASS` from `4` to `8` proves the cap
  is active (`sub_command_buffers 5,355 -> 12,173`,
  `subcb_split_suppressed_by_cap 8,658 -> 1,471`) but does not recover
  average FPS. Tail-600 FPS p50 moves `16.849 -> 16.665`, completion wait per
  present moves `27.116 -> 28.900ms`, and wait-end -> next-enqueue p50 moves
  `15.135 -> 19.980ms`. Keep cap tuning diagnostic-only. The no-gputrace GPU
  time counter is not a total GPU-cost proof for mid-chunk chains because frame
  rows still have one GPU-time sample while tail command buffers rise to `8`.
- [[present-pacing-run-ahead-design.68]] — ACCEPTED DESIGN GATE.
  Simple early publish is coupled to Metal command-buffer publication, so it
  creates overlap by increasing command buffers/render-pass/tile preservation.
  Valid run-ahead must separate CPU readiness from the final Metal CB boundary,
  or coalesce ready work strongly enough to preserve baseline locality.
- [[present-pacing-run-ahead-coalesce.69]] — ACCEPTED MECHANISM /
  REJECTED CARRIER. The first `DXMT9_OFFSCREEN_RUN_AHEAD=1` +
  ready-slot coalescing run removes present wait and creates overlap, but it
  raises command buffers per present `3.999 -> 19.156`, total completion wait
  `29.839 -> 36.990ms/present`, and GPU CB time
  `3.718 -> 35.197ms/present`.
- [[present-pacing-run-ahead-cpu-ready.70]] — ACCEPTED LOCALITY REFINEMENT /
  REJECTED CURRENT PROMOTION. CPU-ready staging cuts the prior coalesced carrier
  to `5.741` command buffers and `1.287` sub-command buffers per present, but
  still worsens total completion wait, wait-to-next-enqueue, and commit replay
  versus baseline. Its `actual.png` also has a large black vertical scene
  artifact, so visual correctness is broken. CB locality recovery is a gate,
  not an FPS proof.

## Cross-links

- [[overview-3dmark05-gt1]] — root domain map and priority DAG.
- [[state-churn-encode]] — per-CB encode CPU cost (the new bottleneck once
  display-sync pacing is dethroned).
- [[snapshot-cache]] — D3D9 state rebuild cost on the PE side; affects how
  many ms are spent per CB before the encode thread even runs.

## What is settled vs open

**Accepted**

- The 28-31 s `completion_wait_ms` is 100% `completion_present_wait_ms` —
  CPU completion thread waiting for Present-bearing `waitUntilCompleted()`
  to return after the compositor accepts the drawable.
  [[present-pacing-display-sync.01]]
- The wait is present-completion pacing, not pure GPU compute. Historical
  sync-paced runs attributed it to display sync; current direct GT1 refines
  that mechanism because every present is already immediate and still waits
  in `waitUntilCompleted()`.
- The completion watcher is not backlogged. It pops almost immediately
  after enqueue and waits on command buffers still in `Committed` status, so
  moving the wait to a later watcher policy would not by itself make
  resource or present waterlines advance earlier.
- Per-CB encode CPU (~11 ms) sits at ~69% of the 16.67 ms 60 Hz budget,
  which is why the average frame slips a vsync slot.
- Current low-overhead FPS attribution splits the lanes: P4
  `completion_present_wait_ms` is still the observed wallclock bucket, but the
  current failure mode is hard under-pipelining: no next command buffer is
  enqueued while the watcher waits. Hot-frame GPU locality remains open as a
  ceiling problem, not the first average-FPS lever.
- dxmt9's explicit present-boundary wait and max-frame-latency token are not the
  current under-pipelining owner. They are applied/skipped as requested, but
  `present_boundary_waits=0` and the completion no-enqueue wait remains flat.
- The post-wait producer path reaches unix `commit_chunk` quickly; the exposed
  gap expands inside commit/replay/submit before queue publish, then inside
  backend encode after `EncodeDequeue`.
- A single current-run perf summary now names the P4 overlap shape and exposed
  CPU stages directly. Use `Pacing / CPU Stage Derived` to reject local CPU
  wins that leave the run `under-pipelined-no-enqueue`, then use H43/H44
  compare gates for A/B proof. [[present-pacing-summary-triage.40]]
- The fresh summary-triage current run confirms the same owner split with
  lower manual parsing cost: `completion_wait_no_enqueue_share=99.820%`,
  `commit_chunk_replay_cpu_ms_per_present=8.207`, and
  `encode_chunk_cpu_ms_per_present=10.566`.
  [[present-pacing-summary-triage-current.41]]
- The fast post-wait unix crossing must not be misread as a PE API-call
  timestamp. PE `Present()` itself is not the hidden `completion_wait_ms`
  sleeper: its p50/p95 is `2.580/5.077ms` versus completion wait
  `28.419/39.576ms`.
- The app/Wine loop does call back into PE D3D9 almost immediately after
  `Present` returns. The first next-frame PE call is `BeginScene` in
  `1702 / 1703` rows, with `entry_delta_ms` p50/p95 `0.310/0.436ms`.
  Therefore the no-enqueue completion window is not caused by missing app API
  calls; it is caused by PE-local recording not becoming a Metal enqueue until
  the PE-recorder/unix submission boundary or later.
- The first non-empty PE chunk after `Present` is not immediate. It is a
  `capacity_post` flush of `64` records at steady p50/p95
  `19.908/34.810ms` after `Present` return, while the first bridge call itself
  is only `0.504/0.617ms` p50/p95. This identifies PE chunk-fill cadence as the
  first exposed boundary before unix replay/publish/encode.
- The first-call dependency hypothesis is now too broad, and the first
  record-producing D3D9 call is now identified. Calls `1..4` after `Present`
  are immediate early setup, then the p50 `17.635ms` gap is between
  `SetRenderTarget` return and `Clear` entry. The first appendable
  `APPLY_STATE` record is produced inside `Clear`, not inside the early RT
  setup calls.
- The `SetRenderTarget` return -> `Clear` entry gap is not caused by
  frame-sampling telemetry. A no-frame-sampling run removes all
  `dxmt9-perf-frame` lines while preserving the same p50 gap and first-chunk
  cadence.
- Wider PE call coverage found hidden descriptor/subresource getters before
  `Clear`, but return timestamps reject them as the sleeper. The final
  meaningful logged D3D9 child getter returns at p50 `0.674ms`, leaving the
  same p50 `17.656ms` gap before `Clear`.
- Existing System Trace CPU tables weaken the broad "producer is sleeping in a
  runloop/macdrv wait" explanation. The representative `phase43` sidecar shows
  the D3D/Wine producer thread sampled as Running for `15,354ms` of a
  `15.563s` trace, while CA present still has p50 `74.398ms` request cadence
  and p50 `33.637ms` request -> presented-handler latency. The exact current
  `SetRenderTarget` -> `Clear` program counter remains unmapped because that
  trace predates the PE milestone markers.
- PE caller-PC logging maps the remaining front gate to stable callsites.
  Across `1,725` steady ordinals, `SetRenderTarget` returns from
  3DMark05.exe PC `0x0042AF4F`, then `Clear` enters from 3DMark05.exe PC
  `0x0042B061` after a p50 `17.453ms` gap. `Clear` itself is short
  (p50 `0.231ms`) and produces record 1 immediately after entry. The front
  gate is therefore between two app callsites, not inside dxmt9's
  `SetRenderTarget`, `Clear`, descriptor/getter, lock, or query paths.
- Delaying dxmt9's completion signal after Metal completion does not move the
  front gate. The `DXMT9_PERF_COMPLETION_SIGNAL_DELAY_MS=8` perturbation
  applies `1696` sleeps / `13568ms` before completed-seq publication, but
  `SetRenderTarget -> Clear` p50 remains `17.631 -> 17.550ms`, record 1 p50
  remains `19.025 -> 19.011ms`, and first chunk p50 remains
  `20.802 -> 20.827ms`. Therefore the gate is not a dxmt9 completed
  seq/waterline dependency. [[present-pacing-completion-signal-delay.21]]
- Flushing immediately after the record-producing `Clear` is not enough to
  create producer run-ahead. The diagnostic `DXMT9_PE_FLUSH_AFTER_CLEAR=1`
  path changes the first unix-visible chunk from a `capacity_post` 64-record
  chunk to a `clear` 2-record chunk and moves first-chunk p50 by about
  `1.65ms`, but `completion_wait_with_enqueue_ms` remains `0.000` and chunk
  count increases. The current low-overhead refresh keeps the same conclusion:
  first chunk p50 moves `20.710 -> 19.089ms`, but tail-600 FPS p50 moves
  `15.788 -> 15.681` and `commitCount` rises `41,429 -> 45,617`.
  [[present-pacing-pe-clear-flush.22]], [[present-pacing-pe-clear-flush.23]]
- The latest low-overhead Stage2/Stage1 comparison shows why local CPU wins
  must be gated by P4 movement. Stage1 removes `~3.25ms/present` of
  `encode_draw_cpu_ms`, but `completion_wait_ms / present` rises by
  `~4.03ms` and tail FPS stays flat. The current no-enqueue path reaches unix
  quickly after completion, then spends large serial time in replay/snapshot/
  submit and backend encode. [[present-pacing-lowoverhead-serial.24]]
- The current low-overhead refresh after the latest encode/copy cleanup keeps
  that model intact. `current-lowoverhead-after-cleanup-r1-20260615` renders a
  normal frame and samples `18.878fps` average, but still shows
  `gpu_command_buffer_time_ms=3.072ms/present` versus
  `completion_wait_ms=28.834ms/present`, with only
  `0.208ms/present` in `completion_wait_with_enqueue_ms`. The serial work in
  front of useful overlap is still concrete: replay/snapshot/submit
  `8.457ms/present`, snapshot lookup `3.057ms/present`, backend encode
  `10.695ms/present`, and draw encode `8.730ms/present`.
  [[present-pacing-lowoverhead-refresh.33]]
- The latest post-cbuf-observer low-overhead scout keeps the same owner split
  after the VS/FFPVS cbuf content-history scan was made opt-in. It renders a
  normal fog-heavy GT1 frame and records `1,860` presents. Completion wait is
  still almost entirely no-enqueue wait:
  `completion_wait_without_enqueue_ms=26.839ms/present` versus
  `completion_wait_with_enqueue_ms=0.210ms/present`; GPU command-buffer time is
  only `3.111ms/present`. The exposed serialized CPU stages remain large:
  `encode_chunk=10.902ms/present`, `encode_draw=8.476ms/present`,
  `commit_chunk_replay=8.074ms/present`, queued snapshot
  `3.416ms/present`, and snapshot lookup `2.792ms/present`.
  [[present-pacing-current-lowoverhead.43]]
- The current default P2/P3 scout keeps the same model after the capture-layer
  file route was recovered. It renders normally, records `1,800` presents, and
  reports `completion_wait_without_enqueue_ms=27.475ms/present` versus only
  `0.024ms/present` in overlapped wait. GPU command-buffer time is
  `3.218ms/present`; the exposed CPU side is still large:
  `commit_chunk_replay=8.325ms/present`, snapshot lookup
  `2.850ms/present`, `encode_chunk=11.152ms/present`, and
  `encode_draw=8.580ms/present`. Comparing this run with direct-cbuf removes
  the argbuf setup/open/cbuf-update path and cuts encode by `-24.44%`, but
  sampled FPS stays flat and no-enqueue wait worsens. [[present-pacing-current-p2p3.46]]
- The before-publish record-shape scout refines that model. The first publish
  after a no-enqueue wait is preceded by draw/const-heavy work, not idle or
  state-only traffic: `93.1%` of scanned chunks include draw records, and the
  average publish sample sees `372.366` draw records plus `348.008` const
  records. A diagnostic `DXMT9_DRAW_CHUNK_COMMAND_LIMIT=64` run proves earlier
  publish can create overlap (`completion_wait_with_enqueue_ms_per_present`
  `1.191 -> 21.032`), but it also turns the Metal work into too many smaller
  submissions: command buffers `7,199 -> 22,846`, render passes
  `21,234 -> 26,280`, tile preservation `+75.63%`, and GPU command-buffer time
  `3.309 -> 24.519ms/present`. Do not promote the draw-count limit; the next
  overlap design must preserve render-pass locality. [[present-pacing-noenqueue-beforepublish.47]], [[present-pacing-drawchunk-limit.48]]
- The latest low-overhead refresh after capture-layer repair keeps the same
  current-head owner split without relying on Xcode or encoder breakdown. It
  renders a normal GT1 frame, records `1,812` presents, and reports
  `completion_wait_without_enqueue_ms=27.717ms/present` versus
  `0.199ms/present` overlapped wait. GPU command-buffer time is only
  `3.231ms/present`; the exposed CPU stages are still large:
  `commit_chunk_replay=8.519ms/present`, snapshot lookup
  `2.919ms/present`, `encode_chunk=11.348ms/present`, and
  `encode_draw=8.759ms/present`. It also repeats the before-publish shape:
  p50 `11` commit chunks before first publish and `93.0%` scanned chunks with
  draw records. [[present-pacing-current-lowoverhead.49]]
- A current-tree repeat after the state/copy cleanup keeps that verdict and
  closes the old broad F1/F2 state-copy critique as the next priority. The run
  records `1,823` presents at `16.666` sampled average FPS, with GPU command
  buffer time only `3.020ms/present` and
  `completion_wait_without_enqueue_ms=29.336ms/present` (`99.608%` of wait).
  The P2/P3 serial work is still exposed: `commit_chunk_replay=8.395ms/present`,
  snapshot lookup `2.925ms/present`, and `encode_chunk=11.110ms/present`.
  State materialization is already `46.86%` elided, same-generation/lane covers
  `98.52%` of compatible draw-run pairs, and discarded materialized states are
  only `0.44%` of submissions. Treat residual state-copy/compat scan as fixed
  unless those counters regress; the open FPS levers remain P4 overlap or the
  named replay/snapshot/encode stages. [[present-pacing-current-lowoverhead.52]]
- A matching direct-cbuf repeat proves that the largest remaining argbuf bucket
  is not the average-FPS owner. `DXMT9_ARGBUF_DIRECT_CBUF=1` removes argbuf
  setup/open/cbuf-update/table-bind counters and cuts encode
  `11.110 -> 8.500ms/present`, but `wait -> next enqueue` stays flat
  (`30.482 -> 30.703ms/present`) because `commit entry -> publish` grows
  `13.672 -> 16.260ms/present`. The run is also only visual-open, not a
  default-promotion candidate: HUD is present, but the screenshot is much darker
  than baseline and shows a large white band. This reinforces that the next
  FPS lever is replay/snapshot/publish cadence or true P4 overlap, not more
  local argbuf table cleanup. [[state-churn-encode-encode-phase.146]]
- A larger draw-count split confirms the mechanism but rejects threshold tuning.
  `DXMT9_DRAW_CHUNK_COMMAND_LIMIT=256` creates overlap
  (`completion_wait_with_enqueue_ms=0.199 -> 14.569ms/present`) and reduces
  no-enqueue wait (`27.717 -> 15.828ms/present`), but total completion wait
  rises (`27.916 -> 30.397ms/present`), GPU command-buffer time rises
  (`3.231 -> 4.646ms/present`), command buffers rise `7,247 -> 11,153`,
  render passes rise `21,367 -> 22,686`, tile preservation rises `+5.52%`,
  encode rises `11.348 -> 12.488ms/present`, and sampled FPS stays flat. The
  next P4 design must overlap replay/encode without using extra Metal
  command-buffer/render-pass fragmentation as the carrier.
  [[present-pacing-drawchunk-limit-sweep.50]]
- Overlap candidates now have explicit locality-preservation gates. A future
  P4 candidate can require recovered overlap while also rejecting increased
  command buffers per present, render passes per present, or tile-preservation
  MiB. This encodes the limit64/limit256 lesson into the toolchain instead of
  relying on review prose. [[present-pacing-overlap-locality-gates.51]]
- The first implemented run-ahead/coalescing path validates the mechanism but
  fails the promotion gate. `DXMT9_OFFSCREEN_RUN_AHEAD=1` plus ready-slot
  coalescing drops present wait `29.839 -> 0.202ms/present`, raises overlap
  `1.915 -> 20.855ms/present`, and lowers no-enqueue wait
  `27.924 -> 16.135ms/present`. The same run worsens total completion wait
  `29.839 -> 36.990ms/present`, raises command buffers per present
  `3.999 -> 19.156`, and raises GPU command-buffer time
  `3.718 -> 35.197ms/present`. Treat this as proof that P4 can be moved, not
  as an FPS fix. [[present-pacing-run-ahead-coalesce.69]]
- The R-BACK-2.40 CPU-ready staging follow-up restores much of the Metal
  carrier shape but still fails the FPS gate. Compared with the prior
  run-ahead/coalescing carrier, `command_buffers_per_present` falls
  `19.156 -> 5.741` and `sub_command_buffers_per_present` falls
  `10.394 -> 1.287`; present wait stays near zero (`0.116ms/present`).
  Against baseline, however, CBs are still above baseline (`3.999 -> 5.741`),
  total completion wait worsens `29.839 -> 40.347ms/present`,
  wait-to-next-enqueue worsens `31.632 -> 52.724ms/present`, and commit replay
  rises `8.363 -> 40.441ms/present`. The run's `actual.png` also contains a
  large black vertical scene artifact, so the clean error counters do not make
  it a visual smoke pass. This confirms that restoring CB locality is a
  necessary gate, not an FPS proof.
  [[present-pacing-run-ahead-cpu-ready.70]]
- The Wine `OnMainThread()` source audit is useful but not a current-owner
  proof. It proves a possible synchronous macdrv transmission path, not that
  the selected GT1 producer is blocked there. Later native-selector sidecars
  sample the producer thread running with `0` wait-keyword hits, so the
  stronger "previous present holds the Cocoa main thread and blocks next
  `Clear`" framing stays below the measured replay/snapshot/encode lane unless
  a future threshold log or System Trace contradicts it.
  [[present-pacing-winemac-onmainthread.44]]
- A short 2-second System Trace sidecar remains a valid fallback while
  `.gputrace`/Xcode attach is blocked by Developer Mode. It joins `306/306`
  Metal GPU interval rows to dxmt encoder telemetry and selects the native
  producer thread, but the producer is sampled running in `2519 / 2519` rows
  with `0` wait-keyword hits. The same run still has
  `completion_wait_with_enqueue_ms=0`, `completion_present_wait_ms=25.608ms`
  per present, `commit_chunk_replay_cpu_ms=8.613ms/present`, and
  `encode_chunk_cpu_ms=13.353ms/present`, while the captured GPU window is
  `93.07%` vertex-stage time. [[present-pacing-systemtrace-p4-smoke.34]]
- The current-code repeat keeps the same broad constraint but weakens the
  exact CPU verdict from strict negative to inconclusive: System Trace joins
  `386/386` rows over seq `1052..1087`, selects native producer `0x665ec1`,
  finds `producer_wait_keyword_hits=0`, but sees `1` blocked producer sample
  out of `2,429`. It is not positive `OnMainThread` evidence, and the pacing
  split is still no-overlap P2/P3 work:
  `completion_wait_with_enqueue_ms=0`, `completion_wait_ms=26.319ms/present`,
  `commit_chunk_replay_cpu_ms=8.510ms/present`, and
  `encode_chunk_cpu_ms=13.254ms/present`.
  [[present-pacing-systemtrace-p4-current.35]]
- The seq-range repeat is the preferred fallback shape. It limits encoder
  breakdown to `seq 1000:1125`, still joins `395/395` xctrace rows over
  `1037..1073`, and reduces probe output from `553MiB` to `130MiB`. The P4
  verdict returns to a strict negative scout: native producer `0x668652` is
  sampled running in `2515 / 2515` rows with `0` blocked rows and `0` wait
  keyword hits. The pacing owner is unchanged:
  `completion_wait_with_enqueue_ms=0`, `completion_wait_ms=27.606ms/present`,
  `commit_chunk_replay_cpu_ms=8.516ms/present`, and
  `encode_chunk_cpu_ms=10.874ms/present`.
  [[present-pacing-systemtrace-p4-range.36]]
- The P4 comparison gate is now explicit in
  `compare_3dmark05_perf_counters.py`. Reports split total present wait,
  wait-with-enqueue, and wait-without-enqueue into per-present metrics and
  overlap shares; optional gates can fail a candidate that lowers local CPU
  time while leaving `completion_wait_without_enqueue_ms` flat or reducing the
  already rare overlap bucket. [[present-pacing-compare-gates.37]]
- The P2/P3 serial-stage comparison gate is also explicit now. The same report
  exposes replay, queue draw-submission, snapshot, snapshot-cache lookup,
  encode, and same-cycle no-enqueue stages as per-present metrics. This lets a
  CPU-path A/B prove the targeted stage moved before reviewers check whether
  H43's P4 gates or frame sampling also moved. [[present-pacing-serial-stage-compare-gates.38]]
- Raising the mid-chunk sub-CB cap is not the missing overlap lever in the
  current shape. Cap=8 makes the mechanism move (`sub_command_buffers` roughly
  double and cap suppression largely disappears), but tail FPS is flat/worse and
  wait-end -> next-enqueue p50 gets longer. [[present-pacing-subcb-cap.25]]

**Open target**

- Recovering current GT1 direct fps requires reducing P2/P3 work that feeds
  present-bearing chunks, then proving that P4 completion/present wait and
  frame sampling move. The latest stage split makes that P2/P3 work concrete:
  same-sample p50/p95 is `6.172/28.101ms` for unix
  commit/replay/snapshot/submit before `CommitPublish`, `2.535/5.086ms` for
  queue publish-to-dequeue, and `11.384/22.232ms` for backend encode after
  `EncodeDequeue`;
  toggling
  `DXMT9_DISABLE_VSYNC` is no longer a valid lever unless
  `present_schedule_after_minimum_duration` is nonzero. The old
  "bind calls dominate the 73% unattributed encode remainder" attribution is
  rejected by [[present-pacing-bind-cache-work-a.01]]. Category identity cbuf
  reuse, plan-direct binding-packet cache lookup, snapshot component-hash reuse,
  usage-aware payload hashing, and FFP known-zero programmable constant usage
  are now accepted CPU wins with current visual smoke, but still need a
  vsync-on wallclock gate before they close this pacing hypothesis. The
  remaining narrowed CPU target is now named backend encode work first. In the
  latest low-overhead P2/P3 scout, the active children are
  `argbuf_setup=1.875ms/present`, `argbuf_cbuf_update=0.981ms/present`,
  `binding_packet=1.044ms/present`, `stream_bind=1.250ms/present`, and
  `encode_slot_pso_prefetch=1.180ms/present`, plus residual snapshot/cache
  lookup at `2.919ms/present`. Historical split runs further narrow those
  buckets to table reserve/repoint, dirty cbuf update, fragment texture/sampler
  planning/bind, index bind, shader stream, raster state, and miss-side PSO key
  resolve. [[present-pacing-direct-cbuf.45]] proves that removing the argbuf
  path is not enough by itself: the current-default comparison cuts encode
  by `-24.44%` while FPS stays flat and no-enqueue wait worsens. These are
  owned by
  [[state-churn-encode]] and [[snapshot-cache]].
- A larger producer-overlap architecture is still open, but the target is now
  concrete: the app-side interval between `SetRenderTarget` caller PC
  `0x0042AF4F` and `Clear` caller PC `0x0042B061` is command-dispatch cadence
  around `3DMark05.exe+0x88760`, and it is not waiting on dxmt9's completed
  seq/waterline publication. Frame sampling, child descriptor/subresource
  getters, lock/query/swapchain milestone coverage, broad runloop/macdrv sleep,
  hidden dxmt9 API-call duration, and dxmt9 completion-signal delay have been
  rejected or weakened as that owner. A lower actual Metal/CA completion
  dependency is still a separate open experiment because the current
  perturbation happens after `waitUntilCompleted()` has already returned. The
  simple post-`Clear` early-publish probe is now rejected: it publishes a
  2-record chunk earlier, but it still does not enqueue while completion waits
  and it increases chunk count. The current low-overhead refresh confirms this
  remains true after recent encode/copy cleanup. A larger producer-overlap
  design would have to
  publish useful work before the app's `Clear` dispatch gate, or reduce the
  pre-publish replay/snapshot and backend encode stages enough that the
  completion wait no longer exposes a full extra frame slot. Any earlier-flush
  design must still preserve D3D9 ordering, resource lifetime, render-pass
  coalescing, and dynamic-buffer snapshot correctness.

**Proposed (not yet built)**

- Run no-gputrace A/Bs that reduce a named encode bucket or residual snapshot
  rebuild before spending Xcode budget; dirty cbuf, binding-packet cache,
  snapshot hash reuse, usage-aware payload hashing, and FFP known-zero usage
  already have accepted CPU wins plus current visual smoke.
- Reduce draw-run breaks only where the taxonomy names a semantic-safe boundary;
  current average backend batch size remains ~1.9 records, but prior bind-cache
  work proved bind suppression alone is not enough.

**Rejected**

- `DXMT9_MAX_FRAME_LATENCY=3` plus `DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS=0`:
  wallclock unchanged (Δ +0.07% on 251 s scene), p95 +31%, max +12%.
  The compositor's vsync pacing dominates the per-CB wait regardless of
  how many frames are queued ahead.
  [[present-pacing-frame-latency.01]]
- Current direct-path boundary/latency A/B as a producer-overlap lever:
  `DXMT9_DISABLE_PRESENT_BOUNDARY=1` and
  `DXMT9_MAX_FRAME_LATENCY=6 DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS=0` leave
  `completion_wait_without_enqueue_ms≈44s` and sampled FPS p50 `17.8-18.0`.
  The disabled-boundary run proves env propagation (`present_boundary_skipped =
  1740`), but the explicit boundary was never sleeping (`present_boundary_waits
  = 0`) in baseline. [[present-pacing-boundary-latency-ab.06]]
- `DXMT9_PRESENT_ASYNC_ACQUIRE=1`: did exactly what its name says
  (`present_acquire_wait_ms` −37.5%) but the axis is < 0.5% of total
  wait budget; wallclock Δ +0.22% (noise); encode CPU +8.3% from
  added acquire work on the encode thread.
  [[present-pacing-async-acquire.01]]
- Broad bind-cache extension as the main fps lever: Work A covered five new
  bind classes and produced no wallclock change, while `encode_chunk_cpu_ms`
  rose. Keep the counters/helpers, but do not extend this family without a new
  hit-rate or sampling proof. [[present-pacing-bind-cache-work-a.01]]

**Rejected / out-of-scope**

- `DXMT9_LAYER_DISPLAY_SYNC=0` as a production fix — causes tearing and
  breaks the compositor pacing contract. Diagnostic value only.
- Tuning `DXMT9_PRESENT_BOUNDARY_*` policies in isolation — present
  boundary wait counter was 0 in the baseline run; the policy axis is not
  currently load-bearing.
