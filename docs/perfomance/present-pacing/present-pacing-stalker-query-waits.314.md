---
domain: present-pacing
workload: STALKER CoP Day
title: "STALKER Day: query completion waits on unchanged H312 images"
type: evidence
status: partial-attribution
updated: 2026-09-16
source: experiments/output/stalker-query-r635-20260916.noindex/excluded-attempt.json; experiments/output/stalker-query-r635-20260916.noindex/retry/{baseline,diagnostic,analysis,surface-qualification}.json; experiments/output/app-d3d9-stkcop-bench-query-r635b-day-{baseline,diagnostic}/result.json; experiments/output/app-d3d9-stkcop-bench-readme-dx9-high-day-20260831/result.json
related: present-pacing-app-thread-attribution.313.md; present-pacing-cpu-supply-critical-path.312.md; overview.md
---

# STALKER Day query-wait baseline

The repeated current-image Day run finishes at **12.298 FPS**. A separate
diagnostic measures **64.465 s inside 5,204 query GetData crossings**, alongside
62.378 s of queue sequence-completion waits. Query-entry replay drain is only
1.318 s at its last periodic report. These nested/overlapping observations
prioritize the synchronous query boundary over another small PE-only rewrite;
they are not additive costs or a measured removable wait budget.

The diagnostic app-thread CPU envelope uses 0.449 cores, unlike H312's
0.94–0.96 cores in 05/06 GT2. This is a different workload and a partial
diagnostic population, not a controlled cross-workload CPU comparison.
Both new runs miss the onscreen game capture, so **visual/focus qualification
remains open**. No query behavior, runtime defaults or README FPS are changed.

## Qualification and preserved evidence

Source images remain `b74a2023`, identical to H312/H313; repository HEAD is
`eed9cac9` after committing H313. The drivers preserve all five full image
hashes, environment overrides, host-start process lists, staged-image receipts,
benchmark-owned `.result` files and artifact hashes. They compare build images
before/after execution and actual staged Wine-root images against the frozen
manifest. `retry/analysis.json` rechecks the receipts, driver/artifact hashes,
health counters and staged Wine-root images after both runs.

Recipe: Sikarugir cx24.0.7, Day/High, `renderer_r2.5`, 1280x720 windowed,
perf profile, replay offload ON. CPU-ready Tape, segmented transport, inline
constant delta, precise uniform invalidation, lock-scope refinement and
opaque-depth optimization are OFF, matching H312. CPU-supply trace, PE
suspension sampler and uniform-dependency probe are OFF. No task-owned
build/test/other benchmark overlaps these runs; unrelated host activity is
not proved absent.

| Attempt | Disposition |
|---|---|
| `query-r635-day-baseline` | Excluded: user reported concurrent unrelated work; timeout, no benchmark result, late two-second native stack sample. Preserve it; do not infer a regression or deadlock. |
| `query-r635b-day-baseline` | Benchmark result and covered health pass. Warn logging, frame sampling and periodic bridge/perf/drain reports OFF. Onscreen capture fails at 90 s; bridge report covers only the startup factory probe. |
| `query-r635b-day-diagnostic` | Benchmark result and covered health pass. Info logging, per-frame counters, perf reports every 60 Presents, bridge reports every 1,000 commits, drain-site attribution, external native-thread CPU snapshots every two seconds. Not baseline FPS. |

Run paths have the prefix `experiments/output/app-d3d9-stkcop-bench-`.
Campaign drivers and receipts live in
`experiments/output/stalker-query-r635-20260916.noindex/` and `retry/`.
The baseline driver is preserved unchanged; the separate diagnostic driver
moves automatic capture to 150 s and retains benchmark supervision if the
external CPU observer fails. No observer failure occurred.

Eight checked rejection/GPU/pipeline counters are zero at both final endpoints.
The runner's `status=pass` does not include a successful visual oracle.
Both `actual.png` files are full-screen fallbacks after
`unable to find onscreen window matching title: S.T.A.L.K.E.R.`.
Computer-use observes diagnostic PID 62072 alive with an empty window list
during scene progress, and later reports `window_not_found`. No input/focus
action is sent. The missing onscreen surface is unresolved, not evidence of
either correct or incorrect rendering. Preserve these captures as failures,
not visual anchors.

## Whole-run results and elapsed scopes

The `.result` population and encoded-Present population differ because of
loading/warm-up/other application presentation. Normalize cumulative counters
by their own encoded Presents, not benchmark frame rows.

| Observation | Baseline | Diagnostic |
|---|---:|---:|
| Benchmark average FPS | 12.298422 | 11.595983 — diagnostic only |
| Benchmark min / max FPS | 3.408723 / 29.711130 | 3.973437 / 29.028803 |
| Benchmark frame rows | 1,708 | 1,657 |
| Encoded Presents | 1,797 | 1,746 |
| Draws/P | 2,904.92 | 2,924.04 |
| Render passes/P | 30.97 | 31.22 |
| Replay elapsed ms/P | 19.149 | 19.619 |
| Encode elapsed ms/P | 22.758 | 23.957 |
| GPU command-buffer time sum ms/P | 11.589 | 11.606 |
| Queue sequence-wait elapsed ms/P | 35.094 | 35.726 |
| Queue sequence waits/P | 1.405 | 1.411 |
| All replay-drain elapsed ms/P | 0.993 | 1.092 |

Replay/encode counters named `*_cpu_ms` are elapsed scopes, not native CPU
time. GPU command-buffer duration sums are not GPU critical-path occupancy.
Do not sum rows or subtract them from benchmark frame time to create an
exclusive budget. The lower diagnostic FPS is not an isolated measurement
of observer overhead or a runtime regression.

The successful README Day run from 2026-08-31 reports 12.494642 FPS;
the new baseline is 1.57% lower. Different-date single spots, no matched A/B,
and the new missing visual/focus evidence cannot attribute that difference
to code. Historical WineD3D/D9VK values are not rerun here.

### Query crossing versus replay drain

The baseline's bridge report has only five startup crossings and zero chunk
commits. Its zero query entries mean **missing game-process coverage**, not
zero query cost. The diagnostic recovers 259 game bridge reports including
a final report after frame 1745; it matches `result.json`.

| Diagnostic bridge scope | Calls | Total elapsed ms | ms/encoded Present |
|---|---:|---:|---:|
| Query GetData | 5,204 | 64,465.358 | 36.922 |
| Chunk commit | 258,565 | 6,747.473 | 3.865 |
| StateBlock category | 171,881 | 515.874 | 0.295 |

Query GetData has a 130.489 ms maximum crossing. The separate queue
sequence-wait total is 62,377.904 ms. Both query-call count and `submit_flush`
count are 5,204, consistent with frequent query-driven flushing, but this
aggregate equality does not establish per-call flags, type, result or identity.

The last query **replay drain** report is at 1,740 Presents: 2,450 blocking
entries, 1,317.645 ms total, 0.7573 ms/P. The final six Presents lack updated
site attribution; the overall perf endpoint reaches 1,746. Query drain is
inside the bridge scope and precedes the core query call. It is not the
subsequent queue/GPU completion wait. These observations strongly distinguish
the two wait mechanisms, but do not prove every sequence wait belongs to
GetData or that all of its latency can be removed.

The successful historical Day run also has a complete query report:
5,354 GetData calls / 64,016.058 ms. The candidate is therefore supported by
successful benchmark evidence, not just an older failed diagnostic.
StateBlock bridge cost is smaller, but excludes PE preparation and downstream
snapshot/cache rebuilding; it does not close that separate candidate.

### Partial native application CPU

The calibrated external observer identifies PID 62072 / native TID 77887687
using the provider's commit marker and process start identity. Stable adjacent
endpoints within the `draw_calls > 100` scene heuristic cover 1,536 frame
advances and 130.847 s. CPU is 57.613 s user + 1.201 s system:
**38.291 ms per covered frame / 0.449 average cores**. No observer exception
or CPU-query error is reported; startup/pending-selector/unstable endpoints
are excluded.

The log parser retains 1,736 frame rows out of 1,746 encoded Presents; the
scene heuristic is not benchmark-owned frame identity. The CPU result is a
partial application/game/Wine/PE/synchronous-Unix envelope, not PE-exclusive
or whole-scene time. Its non-CPU remainder cannot be assigned entirely to
queries. Unlike GT2's near-one-core envelope, it does not suggest a
continuously CPU-saturated application thread in this selected population.

## Code path and model boundary

The relevant production path on these images is:

```text
D3D9QueryImpl::GetData
  -> flushChildRecorder                         (also without FLUSH)
  -> dxmt9c_query_get_data
     -> drainDeferredReplay                     (CPU replay fence)
     -> core Device::getQueryData
        if FLUSH:
          -> upperDevice_->flush
          -> CommandQueue::submitFlush
          -> QueueLifecycleController::flushAndWait
             -> commitCurrentChunk
             -> waitForSequence(nextSeqId - 1)   (latest published suffix)
          -> completeUpTo(core submittedSequenceId)
        -> Query::getData                       (inner nonblocking predicate)
```

Source owners: `src/d3d9/d3d9_pe_device_child_misc.cpp`
(`D3D9QueryImpl::GetData`),
`src/d3d9/device_c_bridge_swapchain_query_stateblock.cpp`
(`dxmt9c_query_get_data`), `src/d3d9/core_resources.cpp`
(`Device::getQueryData`, `Device::issueQuery`, `Query::getData`),
`src/dxmt9/dxmt9_device.cpp` (`DeviceImpl::flush`),
`src/dxmt9/dxmt9_command_queue.cpp` (`CommandQueue::submitFlush`) and
`src/dxmt9/dxmt9_queue.cpp` (`flushAndWait`, `waitForSequence`).

The outer FLUSH path waits before asking whether the query is already
resolved. Its target is the queue's latest published sequence, not an
explicit query-END completion receipt. Core `submittedSequenceId_` advances
per core operation and must not be treated as the ChunkSlot sequence namespace.
Simply deleting the wait while retaining `completeUpTo()` could fabricate
completion; merely swapping sequence counters is not a safe implementation.

`specs/d3d9/queries/spec.md` and `QuerySeqId.tla` describe END-to-chunk
identity with separate submission and completion. The production pure
`queryGetDataPollSatisfied` predicate is nonblocking, but that does not prove
the entire public wrapper is nonblocking. The `ConcurrentProgressSignals`
gap already limits its native binding. Moreover,
`tests/native/core/core_device_lifecycle_spec.cpp` currently expects FLUSH
to return `S_OK` and advance the core cursor synchronously. A future rewrite
must change the production ownership and test contract together, not just
preserve those assertions under an asynchronous name.

## Next bounded investigation

1. Add cold, bounded query evidence: kind/generation/issue ordinal, FLUSH,
   entry/exit readiness and HRESULT; distinguish replay-drain, publication,
   and actual queue completion identities. Avoid per-poll full logging.
2. Bind query END to a typed backend publication/completion receipt, including
   reissue, destruction, reset/loss and stale-generation cases. Do not infer
   identity from the current Tape control's `seqId=0` placeholder.
3. Test a query-specific submit-without-wait path with pending `S_FALSE`,
   later `S_OK`, and actual completion-driven progression. Keep blocking
   readback/reset/teardown semantics separate. Require native delayed-completion
   counterexamples, model-code binding, Wine behavior and visible Day evidence.
4. Re-measure matched baseline/candidate on a visible, focus-qualified lane.
   Then investigate StateBlock restore/snapshot reconstruction if supply is
   still limited. Query data semantics (including real occlusion/timestamp
   backend data versus core placeholders) need a separate type-specific audit.

This task changes documentation only. No new TLA/native runtime validation is
claimed; no nonblocking query implementation or FPS improvement is claimed.
