---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Native application CPU and synchronous wait stacks"
type: evidence
status: measured-runtime
updated: 2026-09-09
source: experiments/output/app-thread-waits-H305-20260909/analysis; experiments/output/sync-wait-audit-H304-20260909/analysis
related: present-pacing-replay-baseline.303.md
---

# Native application CPU and synchronous wait stacks

On unchanged HEAD `1f3f0358`, the application/commit thread uses approximately
0.924/0.954 CPU cores in 05/06 GT2. This supports a CPU-heavy upstream supply
path, not the hypothesis that the replay worker's long input wait is itself
an application GPU-completion wait. It does **not** isolate PE-exclusive cost
or prove the FPS benefit of changing replay or synchronization.

## Measurement boundaries

Four sequential runs: 05 CPU+stacks, 05 CPU-only, 06 CPU-only, 06 CPU+stacks.
The H303 runtime, resolutions and feature policy are retained: Sikarugir
cx-24.0.7, PE32/WoW64, 1024x768 / 1280x720, offload on, Tape/segmented/inline
constant delta/opaque-depth optimization off. These are Info/perf diagnostics,
not uninstrumented best-FPS runs. No README update or product change follows.
Three canonical runtime builds reported no work before measurement; no build,
test or second benchmark was launched alongside the scenes.

The existing `scripts/tools/observe_darwin_thread_cpu.py` observer selects the
Unix commit marker's exact native TID, not the AppKit main thread. Cumulative
native user+system CPU is queried every two seconds with the calibrated
`build/tools/dxmt9-thread-cpu` helper. Identity, log inode, intermediate sample
validity and query bounds qualify a single partial scene envelope. Frame-log
endpoints are approximate, not exact Present timestamps. The thread includes
game, Wine, PE and synchronous Unix work; it is not the replay worker.

CPU+stacks additionally enables drain-site attribution and runs three
five-second `sample` bursts at approximately scene seconds 10/30/45, with a
10 ms sampling interval. Sampling/symbolization is intrusive and may overlap
later work. Its FPS and off-CPU remainder are not ordinary-runtime estimates.
CPU-only disables these stack captures and the drain-site knob; neither mode
enables replay-plan or queue-mutex split timers. Host snapshots do not prove
continuous host isolation, and separate modes are not a controlled A/B.

| Mode | Scene FPS (diagnostic only) | Native envelope frames | Scene transition coverage | Native CPU ms/frame | Average cores | Non-CPU remainder ms/frame |
|---|---:|---|---:|---:|---:|---:|
| 05 CPU-only | 26.934 | (2, 1695] | 96.91% | 33.511 | 0.924 | 2.770 |
| 06 CPU-only | 16.901 | (740, 1787] | 96.50% | 55.889 | 0.954 | 2.698 |
| 05 CPU+stacks | 22.116 | (2, 1378] | 96.16% | 33.730 | 0.755 | 10.956 |
| 06 CPU+stacks | 14.025 | (748, 1613] | 96.22% | 55.630 | 0.785 | 15.274 |

Non-CPU remainder is `(wall - native CPU) / frame transitions`, including
blocking, scheduling and preemption. It is **not** a removable lock budget.
Native accounting does not identify on-CPU spinning or divide game/PE/Wine.
All eight reported error/reject counters are zero. 05 has complete reported
health coverage; 06 CPU-only/combined leave 19/21 final frames uncovered and
terminate at the expected post-scene GUI timeout. CPU-only screenshots were
inspected; neither screenshots nor counters prove temporal pixel correctness.

## Relation to replay supply

These CPU-only cumulative-counter subwindows lie inside, but do not share the
exact endpoints of, the native envelopes. Values are ms/Present, not additive
exclusive CPU categories or source-causal timelines.

| Scope | 05 (60, 1680] | 06 (780, 1740] |
|---|---:|---:|
| Replay elapsed service | 20.574 | 22.422 |
| Worker no-consumable-head wait | 15.004 | 36.320 |
| App commit elapsed work | 2.070 | 2.257 |
| Deferred replay drain | 0.101 | 0.077 |
| Present ordinal wait | 0.042 | 0.040 |
| Sequence / synchronous GPU / buffer-map waits | 0 / 0 / 0 | 0 / 0 / 0 |

`ReplayOffloadQueue::pop` can wait for an empty FIFO **or an uncommitted
mutation placeholder at its head**; wake/reacquisition latency is also inside
the interval. H303's original "empty-input" wording was too narrow. A worker
waiting for consumable input can coexist with a CPU-busy application thread.
The H304 code audit also distinguishes the dedicated GPU completion watcher's
wait from application waits. These observations do not exclude all GPU-related
effects or close the Empty-versus-ReservedHead attribution gap.

## Blocking stacks and uncovered lock sites

The analyzer conserves each exact-TID tree by subtracting child counts from
parents: nested pthread/CV ancestors are not added repeatedly. Root sample
counts are 438/438/438 for 05 and 415/414/415 for 06. Recognized wait counts
are respectively 41/37/20 and 15/13/7. They are wall snapshots, **not unique
waits, milliseconds, CPU percentages or FPS shares**. Unknown Rosetta/Wine
frames and repeated dispatcher ancestors prevent exhaustive attribution.

Repeated, symbol-qualified shapes:

- `Buffer::unlock` -> `DeviceImpl::uploadBufferData[Range]` ->
  `std::mutex::lock` -> pthread mutex wait. In
  `src/dxmt9/dxmt9_device.cpp`, these functions directly acquire
  `queue_.mutex_`; they do not use the command-queue mutex split probes.
  The delegated pool upload also takes an arena lock, so an upload ancestor
  alone would not identify which mutex is contended.
- `Pool::markBufferUse` and `Pool::finalizeBufferMap` ->
  `std::__shared_mutex_base::lock` -> condition-variable wait. These are
  `bufferArena_.update` exclusive-lock contexts in
  `src/dxmt9/dxmt9_resource_pool.cpp` and `dxmt9_resource_pool.hpp`.
  05 also samples `Pool::bufferHasVersionedBacking` taking the shared side.
- `CommandQueue::markChunkResourcesAndCaptureBufferBindings` ->
  `std::mutex::lock` is observed in all six bursts. Its implementation in
  `src/dxmt9/dxmt9_command_queue.cpp` also marks before and captures after
  the queue-mutex scope. Classification requires the nearest observed lock
  frame, not just this enclosing function name.
- Deferred buffer replay drain appears in five bursts. No selected app stack
  identifies a Present-ordinal or GPU-sequence wait; absence is not proof of
  zero waits outside these samples.

The next bounded attribution target is upload queue acquisition/hold versus
buffer-arena acquisition/hold, with the competing owner and resource identity
kept distinct. Preserve sequence tickets, mutation visibility and reclaim
contracts before considering lock narrowing or batching. The current evidence
does not justify removing locks or promising a large synchronization speedup.
App CPU symbol attribution and replay Empty/ReservedHead timing remain open.

## Evidence

The H305 `analysis/` directory retains the guarded driver, observer, analyzer,
four recipe/measurement receipts and `app-thread-summary.json`. Receipts pin
all five products, source/build/runtime identities and staged images; each run
retains raw CPU JSONL, result/log/CSV, screenshot and any sample reports.
The initial 05 combined driver/observer are preserved under `snapshots/`, with
hashes matching that run's frozen recipe; later runs use the current scripts.
`final-audit.json` rechecks identities, native-envelope reconstruction and
sample-count conservation. H304's `report.md` retains the preceding read-only
call-path audit. No native/TLA suite is rerun for these documentation-only
changes; no synchronization implementation or formal contract is changed.
