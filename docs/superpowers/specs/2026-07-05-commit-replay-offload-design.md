# Commit-Chunk Replay Offload (Producer Serial Reduction) — Design

Date: 2026-07-05
Status: approved (architecture A — full deferral + pacing re-anchor)
Owner axis: 3DMark05 GT1 average FPS (producer serial time)
Predecessor: `docs/superpowers/specs/2026-07-04-gt1-p4-deferred-boundary-design.md`
(Phase A closed: the present boundary never waits on the baseline shape;
the wall is the producer/replay serial path —
`docs/perfomance/present-pacing/present-pacing-deferred-boundary-isolated.189.md`.)

## Problem and Opportunity

`dxmt9c_device_commit_chunk` (`src/d3d9/device_c_chunk_replay.cpp:2035`) is a
synchronous unix call on the app thread. It validates the wire chunk, then
replays every record — mutating the unix-side `D9CDevice` state, building
snapshot/draw submissions, and appending commands into the queue's writing
`ChunkSlot` — before returning.

Measured on the R0 baseline
(`experiments/output/app-d3d9-3dmark05-p4-deferred-iso-baseline-r0-20260704`,
1800 presents, ~61.1 ms/frame):

| Item | ms/present |
|---|---|
| `bridge_commit_latency` (app-thread unix-call total) | 8.919 |
| `commit_chunk_replay_cpu_ms` | 8.487 |
| — of which draw-record replay | 6.685 (snapshot 3.168, batch submit 1.762, run submit 1.183) |
| — pending flush | 1.776 |
| — present record | 0.529 |
| import + handle marking | 0.424 |
| encode-side `encode_chunk_cpu_ms` (for headroom) | 10.992 |

Volume: ~14.1 chunks/present, ~199 KB payload/present, ~708 records/present.
Direct (non-chunk) bridge calls in steady state: `bridge_total = 0.030/present`
— effectively zero. Boundary/ring waits on the app thread are ~0
(`present_boundary_waits=0`, `queue_sequence_wait_ms` negligible), so the app
thread is purely serial-time bound. Removing ~8.9 ms/present raises the FPS
ceiling from ~16.3 to ~19 (+~17%).

## Architecture (Approach A)

Move replay execution to a dedicated replay worker; the app-thread commit
becomes a validated raw handoff into a bounded raw-chunk queue. Present
pacing is re-anchored on the app thread with a present-ordinal wait.

Transport correction (2026-07-05, pre-plan): PE chunks and queue `ChunkSlot`s
are not 1:1 — R0 counters show ~14.1 `commit_chunk` entries coalesce into one
writing slot that publishes once per present
(`completion_no_enqueue_commit_chunk_entries_before_publish=14.127/present`,
`chunk_publish_slot_residency_samples=1.000/present`). A `Recorded` slot state
therefore does not fit; the deferral point sits upstream of the slot ring.
A dedicated worker (rather than interleaving replay on the encode thread) is
chosen because the worker inherits the app thread's existing blocking
semantics (`ensureWriterSlot` ring waits, publish policy) verbatim, while an
encode-thread replayer would need a non-blocking scheduling policy to avoid
ring-full self-deadlock (the encode thread must keep draining `Pending` slots
to free the ring). Approach A's pacing re-anchor is unchanged.

### Raw queue and replay worker

```
app thread:    commit_chunk = validate → copy → rawQueue.push → [ordinal wait] → return
replay worker: rawQueue.pop → existing replay dispatch → writing slot → publish (existing policy)
encode thread: unchanged (consumes Pending slots)
```

- **commit_chunk, app thread**: wire header/range validation stays synchronous
  (early failure preserved, same HRESULTs as today for malformed chunks) →
  copy record table, handle table, and payload arena into a unix-owned
  `RawChunk` buffer (~14 KB/chunk; the PE buffer is reused by the recorder
  after return, so the copy is required) → push onto the bounded raw queue
  (blocking when full — producer backpressure, bound sized in records/bytes
  to cover ~2 frames of chunks) → present-bearing chunks (detected by a
  record-type scan during validation) run the §Pacing wait → return.
- **replay worker (new thread)**: pops raw chunks FIFO and runs the existing
  replay logic from `device_c_chunk_replay.cpp` unchanged in content — the
  worker is the sole owner of `D9CDevice` replay state, the writing slot, and
  the existing publish path, inheriting today's blocking semantics
  (`ensureWriterSlot` ring waits) without new scheduling policy. No
  reordering is introduced.
- **encode/completion/finish threads**: unchanged.
- **Shutdown/Reset**: drain the raw queue, join the worker, then proceed —
  part of the drain-fence surface.

### Pacing re-anchor

Today the frame-latency boundary wait runs inside the replay of
`D9C_COMMAND_RECORD_PRESENT` (`device_c_chunk_replay.cpp:2792` →
`CommandQueue::submitPresent`) on the app thread. With offload:

- The app side keeps a **present ordinal** `N` (incremented per
  present-bearing commit). The queue exposes `completedPresentOrdinal`
  (incremented by the completion path where it already distinguishes
  present-bearing completions via `completedPresentSeqQueue`).
- Present-bearing `commit_chunk` calls wait, after enqueueing the raw chunk,
  until `completedPresentOrdinal >= N - clamp(maxFrameLatency, 1,
  kMaxQueuedChunks)`.
- The wait honors `resolveBoundaryPolicy`: `Disabled` skips it;
  `DeferredPresentCompletion` defers the wait to the next present-bearing
  commit with the `N+1`-shifted target (isomorphic to the tightened
  `9c0960f5` semantics); the default (`PresentCompletion`-class) waits
  immediately. `Completion`/`AfterAcquire` map to the default ordinal wait
  (their seqId-level distinctions are not observable at ordinal granularity;
  documented as a behavior note).
- In offload mode `submitPresent` (now running on the encode thread inside
  deferred replay) must NOT execute its boundary wait — the encode thread is
  never parked on pacing.
- Equivalence claim: the wait sits at the same wall-clock point
  (before `commit_chunk` returns for the present-bearing chunk) and the
  target is order-isomorphic (present ordinals and present seqIds are both
  monotonically assigned to the same events). This is proven by extending
  `specs/verification/tla/PresentFrameLatency.tla` with the ordinal variant
  and checking the same latency-bound invariants.

### Synchronization surface

- Every direct (non-chunk) device call gets a **drain-fence prologue**: wait
  until the raw queue is empty and the in-flight deferred replay (if any)
  has completed. At `0.030` direct calls/present in GT1 this costs nothing,
  and it is correct by construction for server-state reads (cap-check-class
  `chunkBarrierFlush` users), resource destroy/`Reset`, query paths, and any
  wire-handle generation race (`WireHandleGeneration.tla` invariants keep
  holding because no handle is resolved after a drain-fenced release).
- PE side is unchanged: no ABI change, no abi-hash regen, both x86/x64 lanes
  covered by the same unix-side change.

### Failure semantics

Deferred replay failure (record validation, handle resolution) fail-stops the
queue exactly like the batch-suffix-restore precedent (`78969a2b`):
`DXMT_ASSERT` in debug; in release set the stop flag and notify all queue CVs.
`commit_chunk` returns synchronous failure only for header/range validation.
The historical per-record failure short-circuit HRESULT contract
(`include/dxmt9/device_c.h:845`) does not hold in offload mode; this is
documented in `specs/backend/design.md` when the flag is introduced.

### Gating and naming

- Env: `DXMT9_OFFLOAD_COMMIT_REPLAY` — default off, read once at first use.
  Documented in `agents/rules/environment_variables_bridge.rules.md`.
- Off path must stay byte-identical in behavior (replay inline on the app
  thread, boundary inside `submitPresent`) so paired scouts are a clean A/B.

## Observability

New counters (normal counter-table + callsite audit discipline):

- `commit_chunk_raw_enqueue_cpu_ms` — app-thread cost of the raw handoff.
- `offload_replay_cpu_ms` — encode-thread deferred replay time.
- `offload_replay_queue_depth` (+p50/p95 ring) — raw-queue backlog.
- `present_ordinal_boundary_waits` / `present_ordinal_boundary_wait_ms`.
- `offload_drain_fence_waits` / `offload_drain_fence_wait_ms`.

## Proof Protocol

Paired 120 s no-gputrace scouts on the same day/HEAD, identical flags except
`DXMT9_OFFLOAD_COMMIT_REPLAY=1`, judged by an extended
`scripts/tools/compare_3dmark05_p4_pair.py` (or a sibling mode):

1. **FPS**: presents above baseline beyond the ±5% noise band.
2. **Mechanism**: candidate `bridge_commit_latency` minus
   `present_ordinal_boundary_wait_ms` ≤ 2 ms/present (raw handoff only) and
   `offload_replay_cpu_ms` ≈ the removed replay time appearing on the encode
   side.
3. **Locality (non-increase)**: CB, sub-CB, `render_pass_begin`, tile
   preservation per present — all at or below baseline (+2% slack).
4. **Correctness**: `status=pass`, `gpu_command_buffer_errors=0`,
   `completion_dequeue_status_error=0`, no INVALIDCALL-class strings,
   non-black captures passing the `v0.0.3` anchor class check.
5. **Pacing semantics**: `present_ordinal_boundary_*` counters present;
   effective frame-latency behavior unchanged (no run-away present depth:
   `completion_pending_depth_max` at or below baseline + latency).

Promotion to default remains a separate decision after the gates pass and a
longer confirm run, per the R-BACK-2.34 default-flip precedent.

## Correctness Verification

- Native specs (red-green, `build-arm64-nowine`): raw-queue push/pop/bound/
  drain-fence predicate rules (pure fixture, no Wine/Metal), present-ordinal
  boundary target math (clamp/underflow/deferred-shift) mirroring the
  `presentBoundaryTargetSeqId` test style, and RawChunk copy validation
  (reusing the `d9c_command_chunk_wire_*` range validators).
- TLA: extend `PresentFrameLatency.tla` with the ordinal-wait variant;
  `QueueLifecycleRefinement.tla` is unchanged (the slot lifecycle is
  untouched; the raw queue sits upstream and the writer role is
  thread-agnostic in the model). `dxmt9-verify-tla` green is a merge
  requirement (queue-change convention).
- Full backend native suite green; `git diff --check` clean.
- Visual: paired-scout captures (`--capture-range 880:960:10`) compared for
  artifact classes per the `v0.0.3` anchor discipline.

## Out of Scope / Follow-ups

- PE-side recording CPU (`recordAppend`/`constFlush` budgets) — separate
  track; measured only under perturbing instrumentation so far.
- Dedicated replay worker thread (approach C) — extension slot if encode-side
  budget (replay + encode) ever approaches the frame target.
- B′ per-seqId replay-complete fence — recorded as the fallback if the
  ordinal pacing gate fails equivalence in practice.
- Reducing replay cost itself (snapshot/batch internals) — continues on the
  state-churn-encode track independently.
