---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: overlap
order: 73
title: Current HEAD Has No Run-Ahead Runtime Knob
date: 2026-06-18
type: code-audit
status: accepted-current-code
source: src/dxmt9/dxmt9_command_queue.cpp; src/dxmt9/dxmt9_queue.cpp; src/dxmt9/dxmt9_queue.hpp; agents/rules/environment_variables_present.rules.md; specs/backend/spec.md; specs/backend/gap.md; docs/perfomance/present-pacing/index.md
related: docs/perfomance/present-pacing/present-pacing-run-ahead-design.68.md; docs/perfomance/present-pacing/present-pacing-run-ahead-coalesce.69.md; docs/perfomance/present-pacing/present-pacing-run-ahead-cpu-ready.70.md
---

# Present Pacing 73 - Current HEAD has no run-ahead runtime knob

## Question

Can the current HEAD still run the historical `DXMT9_OFFSCREEN_RUN_AHEAD` /
ready-slot coalescing experiments described by H74/H75?

## Method

- Checked the current git history for the queue files. The current branch head
  is after `Revert run-ahead coalescing code changes`, following
  `Revert CPU-ready run-ahead code changes`.
- Searched source for `DXMT9_OFFSCREEN_RUN_AHEAD`,
  `DXMT9_ENCODE_COALESCE_READY_SLOTS`, `DXMT9_ENCODE_COALESCE_READY_SLOT_LIMIT`,
  `offscreenRunAhead`, and ready-slot coalescing helpers.
- Re-read `QueueLifecycleController::runEncodeIteration()`,
  `QueueLifecycleController::processOnePendingCompletion()`, and
  `CommandQueue::runEncodeLoop()`.

## Observation

The current source no longer honors the historical run-ahead envs. The present
rules file and `specs/backend/gap.md` still described them as if they were available,
but the implementation has been reverted. The queue now has a small
future-facing completion carrier for R-BACK-2.41, but no code path fills it from
multiple ready slots yet:

| Area | Current code shape |
|---|---|
| Env parsing | no source reads for `DXMT9_OFFSCREEN_RUN_AHEAD`, `DXMT9_ENCODE_COALESCE_READY_SLOTS`, or `DXMT9_ENCODE_COALESCE_READY_SLOT_LIMIT` |
| Queue ready slots | `dequeueReadySlot()` pops one slot for the current path; `dequeueReadySlotBatch()` can move several ready slots into `Encoding` and `runEncodeBatchIteration()` can hand a caller-owned source span to a future coalesced backend path |
| Encode loop | `runEncodeIteration()` still calls the backend once for one slot |
| Submission record | legacy path still submits one `slotIndex` and one `seqId`; optional `completionSources` can carry several strict-order source seqIds for future coalesced tails |
| Pending completion | legacy path still completes one seqId; `processOnePendingCompletion()` can expand a coalesced tail into strict-order per-source completion queue entries |
| Carrier invariant | debug builds require every listed source slot to already be in `Encoding` with the matching `seqId` before `submit()` moves it to GPU |
| Diagnostics | when `completionSources` is present, `submit()` aggregates draw/present/blit diagnostics from every source slot so completion-wait counters are not attributed only to the tail slot |

```mermaid
sequenceDiagram
  participant Q as QueueLifecycleController
  participant E as encode thread
  participant B as backend
  participant M as Metal

  Q->>Q: readySlots.pop_front()
  Q-->>E: one ChunkSlot copy
  E->>B: onChunkReady(slot)
  B-->>E: one QueueSubmissionRecord
  E->>M: submit tail command buffer
  M-->>Q: complete one seqId
```

```mermaid
sequenceDiagram
  participant E as encode thread
  participant M as Metal tail CB
  participant C as completion watcher
  participant F as finish thread

  Note over E,C: future carrier only - no current multi-slot producer fills it
  E->>M: submit tail CB with completionSources [seq N..N+k]
  M-->>C: tail complete
  loop each source seqId in order
    C->>F: append completedSeqQueue source seqId
    C->>F: append completedPresentSeqQueue only if source has Present
  end
```

## Verdict

Accepted current-code audit: H74/H75 remain valid historical experiments, but
they are not runnable knobs in current HEAD. New average-FPS work should not
schedule runs with those envs unless the run-ahead implementation is
intentionally reintroduced in the same change.

The open design target remains R-BACK-2.35-R-BACK-2.41: overlap the producer
and encode path with present completion while preserving command-buffer,
render-pass, tile-preservation, present-token, and resource-lifetime gates. The
next implementation must still build CPU-ready staging and encode-side
multi-slot selection. The completion carrier alone does not create run-ahead,
overlap, or a runtime knob; the batch dequeue and completion carrier only make
the eventual coalesced-tail path strict-order and assertion-guarded.

```mermaid
flowchart TD
  H74["H74 historical prototype\nP4 can move\nlocality failed"]
  H75["H75 historical CPU-ready follow-up\nlocality improved\nFPS + visual failed"]
  Revert["current HEAD\nprototypes reverted"]
  Current["current code\none ready slot -> one backend call\nlegacy one seqId path"]
  Carrier["completionSources carrier\ncan expand one tail CB\nto N ordered seqIds"]
  Next["next work\nfresh R-BACK-2.35..2.41 design\nwith H57 gates"]

  H74 --> H75 --> Revert --> Current --> Carrier --> Next
```
