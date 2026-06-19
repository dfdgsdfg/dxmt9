---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: overlap
order: 83
title: Completion Wait Commit Chunk Overlap Counters
date: 2026-06-19
type: instrumentation
status: accepted-tooling
source: src/dxmt9/dxmt9_queue.cpp, src/dxmt9/dxmt9_command_queue.cpp, src/d3d9/device_c_chunk_replay.cpp, src/dxmt9/dxmt9_perf_counters.cpp, scripts/tools/summarize_3dmark05_perf.py, scripts/tools/compare_3dmark05_perf_counters.py, tests/scripts/test_compare_3dmark05_perf_counters.py
related: docs/perfomance/present-pacing/present-pacing-noenqueue-compare-closure.80.md, docs/perfomance/present-pacing/present-pacing-ready-depth-compare.81.md, docs/perfomance/present-pacing/present-pacing-batch-carrier-current.82.md
---

# Present-Pacing 83 - Completion wait commit_chunk overlap counters

## Question

When `completion_wait_with_enqueue` stays near zero, is the producer completely
absent during the completion-thread `waitUntilCompleted()`, or is it running
`dxmt9c_device_commit_chunk()` but failing to publish/enqueue the next Metal
command buffer before the wait finishes?

## Instrumentation

The queue now records commit_chunk milestones observed while
`QueueLifecycleController::completionWaitActive_` is true:

| Counter | Meaning |
|---|---|
| `completion_wait_commit_chunk_entries` | `dxmt9c_device_commit_chunk()` entry observed during a completion wait |
| `completion_wait_commit_chunk_replay_starts` | validated chunk replay began during a completion wait |
| `completion_wait_commit_chunk_replay_ends` | chunk replay ended during a completion wait |
| `completion_wait_commit_chunk_replay_cpu_ms` | replay CPU time for chunks whose replay end was observed during a completion wait |

`summarize_3dmark05_perf.py` and `compare_3dmark05_perf_counters.py` expose the
matching per-present rows:

| Derived row | Use |
|---|---|
| `completion_wait_commit_chunk_entries_per_present` | proves whether the PE/unix producer reached commit_chunk while the watcher waited |
| `completion_wait_commit_chunk_replay_starts_per_present` | separates importer-only overlap from actual replay overlap |
| `completion_wait_commit_chunk_replay_ends_per_present` | shows replay completion before the wait boundary |
| `completion_wait_commit_chunk_replay_cpu_ms_per_present` | sizes replay CPU work that overlapped the wait |

## Interpretation

```mermaid
flowchart TD
  A["completion watcher\nwaitUntilCompleted active"] --> B{"commit_chunk entry during wait?"}
  B -- "No" --> C["producer/app cadence did not reach unix commit_chunk\nP4 fix needs producer run-ahead or app/Wine cadence evidence"]
  B -- "Yes" --> D{"replay start/end during wait?"}
  D -- "No / entry only" --> E["producer reached boundary but import/validation/publish timing is still suspect"]
  D -- "Yes" --> F{"Metal enqueue during wait?"}
  F -- "No" --> G["replay overlapped wait but did not produce ready/enqueue\nfocus publish/encode handoff or ready-slot staging"]
  F -- "Yes" --> H["overlap exists\nthen judge completion_wait_with_enqueue and locality gates"]
```

This complements H80/H81. The old no-enqueue counters measure the gap after a
completion wait ends. These counters measure producer activity while the wait is
still active.

## Gate

The next no-gputrace P4 run should include these rows before another Xcode
counter spend:

| Observation | Next owner |
|---|---|
| all `completion_wait_commit_chunk_*_per_present` rows stay zero | producer/app/Wine cadence; queue-side coalescing alone cannot create overlap |
| entries/replay starts are nonzero but replay ends and enqueue stay zero | import/replay duration or pre-publish handoff |
| replay ends are nonzero but `completion_wait_with_enqueue` stays zero | publish/ready-slot/encode handoff |
| replay ends and enqueue are nonzero | candidate has created overlap; then apply H57 locality and `v0.0.3` visual gates |

## Verification

- `python3 -m pytest tests/scripts/test_compare_3dmark05_perf_counters.py`
- `python3 -m py_compile scripts/tools/summarize_3dmark05_perf.py scripts/tools/compare_3dmark05_perf_counters.py`
- `meson compile -C build-arm64-nowine`
- `meson test -C build-arm64-nowine dxmt9-queue-completion-sources-spec dxmt9-verify-tla`
- `git diff --check`

**Related.** [[present-pacing-noenqueue-compare-closure.80]] ·
[[present-pacing-ready-depth-compare.81]] ·
[[present-pacing-batch-carrier-current.82]].
