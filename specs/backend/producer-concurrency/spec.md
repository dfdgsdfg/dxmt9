---
type: "Spec"
title: "Producer Concurrency Spec"
description: "Actor/lock-domain architecture, the bridge entry classification table, ownership assignments, and the verification mapping for producer–queue concurrency."
tags: [specs, backend, producer-concurrency]
---

# Producer Concurrency Spec

Companion to [requirements.md](requirements.md). The design history and
measured motivation live in
`docs/superpowers/specs/2026-08-20-producer-queue-concurrency-design.md`; this
document is the durable statement of the resulting architecture.

## 1. Actors and lock domains

Four actors touch queue/pool state:

| Actor | Runs | Enters via |
|---|---|---|
| Producer | game thread | `dxmt9c_*` bridge entries (synchronous, in-process calls) |
| Replay worker | `ReplayOffloadWorker` thread | chunk replay → `submit*` paths; last-ref drops → destroy/reclaim |
| Encode thread | queue encode loop | slot dequeue/encode/submit |
| Completion | queue finish loop / GPU callbacks | watermark advance → reclaim |

Lock domains, from narrowest to widest:

| Domain | Protects | Notes |
|---|---|---|
| `HandleArena` internal mutex | slot metadata, `lastUsedSeqId` stamps | The **arena-stamp exception**: publishers may stamp retained objects without the queue mutex (pool header contract; production callers: producer bulk mark, worker draw mark, both via `restampIfTicketAdvancedLocked`'s frozen-ticket protocol). |
| low-4GB shadow pool mutex | wow64 shadow block pool | `7302fa32`; release reachable from the worker. |
| wire-object cache mutex | PE wire-identity map | create/destroy frequency only. |
| `CommandQueue::mutex_` | slot lifecycle, watermarks, capture, map waits, pool structure mutation, reclaim | The residual `queue-shared` domain; per-site contention is observable via `DXMT9_PERF_QUEUE_MUTEX_SPLIT` (42 tagged sites + lifecycle segment holds). |

## 2. Established ownership assignments

Evidence: the four source audits in the design doc §7 and the
`ProducerMarkReclaim` model.

| State | Class (R-BACK-43.4) | Evidence / mechanism |
|---|---|---|
| Buffer rename ring (`renameRing`, `renameActiveIndex`) | `producer-owned` | Referenced only inside the pool; worker/encode consume the commit-time snapshot. |
| Chunk buffer-binding capture read-set (`contents`, `contentRevision`, `desc.size`, flavor flags) | `producer-owned` (writes) + `owner-published` via commit-time snapshot | Every writer is a game-thread path (create/Lock/Unlock/finalize). |
| `lastUsedSeqId` per record | `arena-protected` | Stamped via `markStampUpper` under the arena mutex; monotone max. |
| `nextSeqId_` | `owner-published` | Writers under the queue mutex with release stores; lock-free acquire reads via `markTicketAcquire()` paired with the re-stamp protocol. |
| `completedSeqId_` | `queue-shared` today; T2c targets `owner-published` (atomic) | Written by completion, read by map fast path. |
| Writing slot contents | `worker-owned` between `ensureWritingSlot` and publish, EXCEPT the producer's map-wait force-publish | The exception is why unlocked appending is unsafe without the reserve-copy-commit protocol (T2d, model required). |
| Retainer pins / warm epochs | `producer-owned` (PE side) | Program-ordered release strictly after same-chunk marking; release can synchronously drive reclaim on the releasing thread. |
| Reclaim gate (`destroyPending` + watermark) | `queue-shared` | Three driving actors (producer, worker, completion), all under the queue mutex; the pin premise keeps marked records out of `destroyPending`. |

## 3. Bridge entry synchronicity classification

Taxonomy per R-BACK-43.1. The full per-entry table is maintained below;
class totals summarize it. (Initial classification 2026-08-21; a new entry
must be added here in the change that introduces it.)

The wire surface is the five `device_c_bridge_*.cpp` forwarder files plus
`dxmt9c_device_commit_chunk` (the one hot entry that IS its own wire symbol,
`device_c_chunk_replay.cpp:1548`); the `dxmt9p_*` bodies are the same
implementations compiled under macro renaming. ~89 distinct symbols.
Drain mechanisms: global `DXMT9_DRAIN_OR_RETURN` (cv-wait on offload queue
depth), resource-scoped buffer-lock/unlock ledger waits with
NOOVERWRITE/DISCARD bypass classes, and the fail-fast
`DXMT9_TERMINAL_OR_RETURN` poison check (shader/vdecl family only).

| Family (entries) | Class | Blocking today | Freq (GT2) |
|---|---|---|---|
| Factory enumeration/caps/create (~12) | `app-return-value` / `state-mutation-ack` (create_device2) | never-blocks (no drain — precedes the chunk pipeline) | adapter_count high; rest cold |
| Desc/count getters: texture level count/desc, buffer/surface get_desc, swapchain params, query size/type, shader bytecode, vdecl decl (~9) | `app-return-value` | never-blocks (no drain observed; see open item G1) | mid |
| Device-state getters: viewport/scissor/transform/material/render_state/TSS/sampler/clip/fvf/vs+ps consts/RT/DS/frame-latency (~14) | `app-return-value` by contract, **executed as `ordering-fence`** (full global drain) | may-drain | mid |
| Buffer lock/unlock | `visibility-wait` | resource-scoped cv (measured 0.105 ms/present blocked total, all plain-MANAGED; NOOVERWRITE/DISCARD bypass) | high (21.7 pairs/present) |
| Surface/texture lock/unlock, GetRenderTargetData, StretchRect/ColorFill/UpdateSurface/UpdateTexture, check_device_state/test_cooperative_level (~10) | `visibility-wait` | global drain (surface lock's measured cost is unix CPU, not the drain) | low-mid |
| State setters + draws + present + reset + creates + texture ops + swapchain/query/stateblock ops (~48) | `ordering-fence` | global drain (R-BACK-2.51 single-FIFO invariant) | setters/draws high |
| addref/release pairs (~20) | `state-mutation-ack` | mostly never-blocks; device addref/release pay the global drain wait with the result discarded | buffer pairs were 663×2/present pre-warm-epochs |
| Shader/vdecl create (3) | `state-mutation-ack` | terminal-check only (no queue-depth wait) — the one create family that does not drain | low-mid |
| `commit_chunk` (offload path) | `record-only` | enqueue + sync mark/capture, no drain unless the chunk carries a READBACK record (inline lane) | high (15.9/present) |

**Open classification items** (from the 2026-08-21 inventory; each is a
review obligation, not a resolved fact):

- **G1** — the desc-getter group's no-drain status was read from a grep hit
  list, not a full-file pass; confirm before treating as final.
- **G2** — the device-state getters carry the full global drain although the
  D3D9 contract alone would make them `app-return-value`; whether the drain
  is required (unix `DeviceState` reads must observe replay) or uniform macro
  application is THE first `record-only`-direction review target
  (R-BACK-43.2), sized by the `get_swap_chain` precedent (0.64 ms/present for
  one drained getter before its cache).
- **G3** — shader/vdecl creation's terminal-check-only asymmetry vs every
  other create's full drain: by-design (non-GPU-resident inputs) or gap.
- **G4** — device addref/release drain with ignored result: paying an
  ordering fence they cannot act on.
- **G5** — the state-setter family's `high` frequency is inferred from class
  totals, not per-opcode measurement.

## 4. Ordering protocols

- **Arena-stamp + frozen-ticket re-stamp** (`R-BACK-43.6` reference): a mark
  ticket read lock-free may be stale against a concurrent slot advance;
  under the re-acquired queue mutex the ticket is frozen, so one re-read plus
  a conditional monotone re-stamp is a fixed point. TLA+:
  `ProducerMarkReclaim!WorkerRestamp`; Buggy axis `RestampDiscipline`.
- **Pin-ordering**: retained refs strictly contain the same chunk's marking
  window (program order on the committing thread); pins prevent
  `destroyPending`, the watermark gates reclaim after pins drop. TLA+:
  `PinDiscipline` axis.
- **Stamps-before-capture**: a low stamp is repairable (monotone max); an
  early capture is not — capture never precedes the marking of the same
  commit.

## 5. Verification mapping

| Contract | Evidence |
|---|---|
| R-BACK-43.4/43.6 mark/reclaim ordering | `ProducerMarkReclaim.tla` (+ 2 counterexample cfgs), shared predicates `canReclaimRecord`/`markStampUpper`, `dxmt9-producer-mark-reclaim-spec` |
| R-BACK-43.5 thread-affinity asserts | `assertRecorderThreadConfined` (reference shape); shared helper adoption tracked in gap.md |
| Queue-mutex contention observability | `DXMT9_PERF_QUEUE_MUTEX_SPLIT` per-site acquire/hold/segment rows |
| C++ memory-order obligation | OPEN — deterministic interleaving harness (R-VERIF-7.3 direction), gap.md |
