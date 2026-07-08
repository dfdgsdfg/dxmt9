---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 190
title: Commit-Replay Offload First Runtime Proof
date: 2026-07-06
type: no-gputrace
status: accepted-offload-fps-win
source: experiments/output/app-d3d9-3dmark05-replay-offload-baseline-r0c-20260706/result.json; experiments/output/app-d3d9-3dmark05-replay-offload-candidate-r5b-20260706/result.json; experiments/output/app-d3d9-3dmark05-replay-offload-candidate-r4-20260706/result.json; experiments/output/app-d3d9-3dmark05-replay-offload-fullcbuf-diag-20260706/result.json; experiments/output/app-d3d9-3dmark05-replay-offload-timephase-diag-20260706/result.json; traces/app-d3d9-3dmark05-replay-offload-baseline-r0c-20260706/analysis/captures/frame000920.bmp; traces/app-d3d9-3dmark05-replay-offload-candidate-r5b-20260706/analysis/captures/frame000920.bmp; traces/app-d3d9-3dmark05-replay-offload-timephase-diag-20260706/analysis/captures/frame001040.bmp; docs/superpowers/specs/2026-07-05-commit-replay-offload-design.md
related: docs/perfomance/present-pacing/index.md; docs/perfomance/present-pacing/present-pacing-deferred-boundary-isolated.189.md; specs/backend/design.md
---

# Present-Pacing H190 - Commit-replay offload first runtime proof

## Question

H189 closed pacing knobs as isolated levers: the GT1 wall is the
producer/replay serial path (`commit_chunk` replay ~8.5 ms/present on the app
thread). Does `DXMT9_OFFLOAD_COMMIT_REPLAY=1` (raw-chunk queue + device-owned
replay worker + present-ordinal pacing, `specs/backend/design.md`
§Commit-Replay Offload) raise GT1 FPS on the real Wine/wow64 path?

## Runs

Paired 120 s supervised no-gputrace scouts, `--capture-range 880:960:10`.
Three integration defects were found and fixed en route:

1. **wow64 thread context** (`385b76ea`): the worker lacked the committing
   thread's `g_wow64ClientCallDepth` thread_local, so `wireValuePtr` fell
   through to `reinterpret_cast` of unregistered 32-bit wire values — jump to
   address 0, which wedges Wine's signal handling on a non-Wine thread
   (black-screen, `presents=1`). Fixed by carrying `wow64ClientCall` in
   `RawCommandChunk` and reproducing `ScopedWow64ClientCall` in
   `replayRawChunk`. Root-caused via `sample`-based stack capture of the
   wedged process plus disassembly — the native unix-chunk-injection probe
   cannot cover this layer (no Wine, no wow64).
2. **Per-frame drain-fence serialization** (`1865d7da`): `BeginScene`/
   `EndScene` are direct bridge calls arriving once per frame; fencing them
   drained the whole raw queue every frame (`offload_drain_fence_waits`
   670/478 presents), killing the producer/worker overlap (12 fps, early
   exit). They are pure scene-flag toggles (`core_state.cpp`
   `Device::beginScene/endScene`, no replay-dependent reads), so they are
   exempted from the fence; remaining fences dropped to `3` per run.
3. **Frame-index captures are not same-scene across fps** (diagnostic
   lesson): at 22 fps frame 912 lands at demo time `t≈45 s` where the
   firefight burst has not started; the baseline at 17 fps reaches it at
   `t≈48 s` mid-burst. The apparent "missing particles/flash" was demo-time
   phase; the `frame001040` capture (`t≈60 s`) shows full muzzle-flash/flare/
   volumetric lighting under offload, and the full-cbuf oracle diagnostic was
   negative (not a cbuf-prefix artifact).

## Verdict

Accepted first runtime FPS win; promotion still gated.

| Gate | R0c baseline | R5b offload | Result |
|---|---|---|---|
| presents / 120 s | `1800` | `1996` | **`+10.9%`**, beyond the `±5%` band |
| correctness | `status=pass`, `gpu_command_buffer_errors=0` | same | PASS |
| visual | burst effects at `t≈48s` | same effects (time-phase shifted; `frame001040` proof) | PASS |
| locality CB / sub-CB per present | `4.009 / 2.998` | `4.009 / 2.998` | PASS (flat) |
| mechanism: worker replay | — | `offload_replay_cpu_ms=9.333`/present, raw enqueue `0.671` | PASS |
| pacing | inline boundary | `completed_present_ordinal=1996`, ordinal waits `0` (GPU ahead) | PASS |
| spec gate `bridge_commit ≤ 2ms/present` | `8.541` (inline replay) | `12.967` | **FAIL as written** — dominated by raw-queue push backpressure, not handoff cost |

The backpressure reading is the honest residual: the app thread no longer
replays, but it now waits for queue space whenever it outruns the worker
(worker throughput ≈ `9.3 ms/present` replay + contention). The `+10.9%` win
is the overlap of that wait with useful worker/encode/GPU progress. The next
levers are worker replay cost (the state-churn-encode track now runs off the
app thread) and queue-bound tuning; the spec's mechanism gate should be
restated against `commit_chunk_raw_enqueue_cpu_ms` (`0.671`) plus an explicit
backpressure counter.

Known startup flake (pre-existing class): two candidate attempts exited at
device init with `presents<=1` and zero commits before a clean retry; same
signature as the Phase A candidate flake, independent of the offload env.

## Next gates before promotion

- Restate the mechanism gate (raw-enqueue vs backpressure split) and add a
  `offload_push_backpressure_wait_ms` counter.
- Longer confirm runs plus a `v0.0.3`-anchor visual pass using time-aligned
  captures (frame-index windows chosen per measured fps, or same-input mini
  replay) — frame-index pairs across different-fps runs are not same-scene.
- The gap.md offload row follow-ups (R-BACK id, abort regression test).
