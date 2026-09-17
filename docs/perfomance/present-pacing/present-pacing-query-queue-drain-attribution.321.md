---
domain: present-pacing
workload: STALKER CoP Day
title: "Observer-off visibility control and separate query queue/drain attribution"
type: evidence
status: measured
updated: 2026-09-17
source: experiments/output/query-phase-r645-r646.noindex/analysis.json; experiments/output/query-phase-r645-r646.noindex/occlusion-off-r645-day.guard.json; experiments/output/query-phase-r645-r646.noindex/occlusion-phase-r646-day.guard.json; experiments/output/app-d3d9-stkcop-bench-occlusion-off-r645-day/result.json; experiments/output/app-d3d9-stkcop-bench-occlusion-phase-r646-day/result.json
related: present-pacing-occlusion-timeline-stateblock.320.md; present-pacing-query-end-coalescing.317.md; overview.md
---

# Selected query delay is before dequeue; bridge polling is mostly replay drain

The attachment-preserving real occlusion candidate completes an observer-OFF
STALKER Day visual control and a separate observer-ON attribution run on the
same five staged images. No source/build changes, native tests, TLC, profiler,
or subagent work overlap either runtime. Both runs exit normally, produce
actual scene captures and have all eight required health counters present and
zero. This is bounded spot visual evidence, not whole-sequence pixel equality.

The diagnostic separates two previously conflated costs:

- In the selected occlusion window, publication-to-submit delay is mostly
  Ready/FIFO residence before dequeue, not encoding the queried source itself.
- Across all returned GetData calls, most outer bridge time is inside the
  Unix replay-drain prologue, not core query polling or Wine crossing alone.

These are mechanism results, not proof of a removable critical path or FPS gain.

## Run qualification

Both lanes use Sikarugir cx24.0.7, Day High, `renderer_r2.5`, 1280×720 windowed,
perf profile, Info logging, offloaded replay ON and Metal validation OFF.
Actual occlusion and asynchronous fences are ON; END coalescing is OFF.
StateBlock, query-split and per-frame observers are OFF in both. Global perf
health counters and their 60-Present reporting remain ON: "observer-OFF" here
means the query/timeline/bridge-split and drain-site observers, not a build with
all performance counters disabled.

| Run | Query / drain-site observers | Benchmark FPS | Benchmark rows | Encoded Presents |
|---|---|---:|---:|---:|
| R645 | OFF / OFF | 21.457247 | 2,620 | 2,709 |
| R646 | ON / ON | 20.149834 | 2,534 | 2,623 |

R645's `scene-mid.png` and `actual.png` show the cafe, ground, foliage and
surrounding geometry. `loading.png` is explicitly not scene evidence. R646's
`scene-early.png` and later `actual.png` also show scene geometry. Inspection
used Computer Use; no gputrace or extra GPU readback was injected. The images
are not frame-aligned and cannot establish exact OFF/ON pixel equivalence or
exclude transient artifacts between captures.

All five staged SHA-256 values match the canonical build outputs and each
other across the pair. The retained guard records environment, invocation,
start/finish and five-second task-relevant process samples: 45 samples per
run, 43 with the Wine engine launch active, zero sampled competing build/test/
TLC/profiler processes. Sampling does not exclude short unseen work or OS/user
activity. No such work was scheduled by this session during the runtimes.

The single OFF/ON pair is not a repeated observer-cost estimate, provider
ON/OFF comparison or non-regression gate. The diagnostic FPS must not replace
the visual control; neither value updates README or promotes the provider.

## Publication-to-submit decomposition

The existing `DXMT9_QUERY_OBSERVER` now records ordinary single-source encode
start/end at `QueueLifecycleController::runEncodeIteration`. Start follows
dequeue; end follows source resolution, unlocked encoding and queue relock.
The interval is elapsed phase time, not thread CPU or pure Metal encoder work.
DCE/lookahead and multi-source session attribution are not fabricated; absent
hooks remain explicit missing stages. Actual submit is the existing tail-CB
commit/completion-handoff observation, not a per-sub-CB timeline.

R646 skips 5,000 ENDs and retains 128 summaries: 56 OCCLUSION and 72 EVENT.
All retained rows have all six stages and ordered publication/encode/submit/
completion timestamps. The occlusion rows include 31 zero and 25 positive
results. There are 19,749 dropped summaries, 29,715 recent-evidence evictions
and 287,818 polls outside the retained identity set; this is not whole-run
per-query coverage.

| Occlusion interval, 56 retained rows | Median | p95 |
|---|---:|---:|
| Publication → dequeue/start | 14.340 ms | 30.343 ms |
| Dequeue/start → post-encode relock | 0.268 ms | 0.656 ms |
| Post-encode relock → actual submit observation | 0.021 ms | 0.033 ms |
| Publication → actual submit observation | 14.573 ms | 30.492 ms |
| Submit → successful completion observation | 3.612 ms | 8.481 ms |
| Completion observation → first S_OK | 0.206 ms | 6.507 ms |

Do not add the component medians. The selected rows' publication-to-submit
interval sum is 987.367 ms, of which 971.065 ms is before dequeue. This is
98.35% of that **selected interval population**, not CPU utilization or an
FPS bound. Concurrent query intervals can overlap; their sum is not unique
wall time. The 72 EVENT rows similarly have a median 11.556 ms before dequeue
versus 0.114 ms in the encode phase.

Ready residence can include older source execution, backpressure, submission
dependencies and thread scheduling. It does not show an idle encode worker or
prove a direct GPU wait. Submit-to-completion also includes finish-thread
observation scheduling and must not be called pure GPU execution time.

## Matching bridge polling populations

`device_c_query_poll_observer.hpp` adds an enabled-only Unix shim observation
around the existing drain and provider calls. It does not alter submission,
drain, retry, HRESULT or output-byte behavior. The gate creates no scope,
clock sample or report when disabled. Fixed aggregate storage records matching
call/result populations; opaque query pointers are never used as identities.
The counts match exactly across PE bridge, Unix split and core: **309,583**
GetData calls, all with FLUSH, zero drain failures or invalid clocks.

| Whole-run interval | Cumulative elapsed time | Scope |
|---|---:|---|
| PE bridge GetData | 11,070.769 ms | Outer bridge call, excluding PE recorder flush |
| Unix shim measured total | 10,636.000 ms | Drain plus provider; excludes its own reporting |
| Replay-drain prologue | 10,071.073 ms | All calls, including nonblocking checks/overhead |
| Provider call | 564.927 ms | Core plus provider/observer overhead |
| Core polling | 391.113 ms | Nested core-only timer |

The drain is 94.69% of the Unix measured interval and 90.97% of the PE bridge
interval. The outer remainder is 434.769 ms, but includes measurement/reporting,
marshalling/crossing and other wrapper work; it is not solely Wine overhead.
The provider-minus-core remainder is 173.814 ms and includes the core observer.

Result buckets preserve the same intervals: 24,849 S_OK calls contain
7,928.429 ms of drain time; 284,734 S_FALSE calls contain 2,142.645 ms. No
other HRESULT is returned. This does not mean GetData performs an internal
GPU-completion wait: the measured drain catches up the replay worker before
core polling. PE `flushChildRecorder` still precedes, and is outside, all the
intervals in this table.

The independent blocked-drain site counter's last periodic endpoint covers
2,580 Presents: 3,906 waits, 9,349.161 ms. It excludes the final 43 Presents
and the nonblocking drain calls, so it must not be subtracted from the final
all-call total as if both covered the same population. Its late interval from
2,280 to 2,580 Presents adds 5,433.941 ms across 630 waits, supporting a
late-scene drain-tail target without assigning that cost to an exact camera
position. These aggregates cannot be joined directly to the 56 query rows.

## Verification and next decision

Native focused gates pass 12/12; x86_64 gates pass 5/5. The unchanged full
214-configuration TLC inventory passes in 189.41 s before either run. Native,
Unix and both PE builds pass; install-name and bridge-entry audits pass.
New deterministic observer tests pin matching timing populations, drain-failure
provider exclusion, HRESULT preservation, invalid clock rejection, bounded
late-END/encode joins and missing-stage reporting. No new scheduling or
completion policy is introduced; the earlier lifecycle/attachment proof limits
in H320 remain.

The next investigation should qualify the earlier-source/frontier responsible
for Ready residence and the work covered by each blocked replay drain. A
query-END-qualified replay receipt might avoid waiting on unrelated later
work, but requires ordered Issue/GetData, reissue, reset and lifetime evidence;
do not simply remove the global drain. Keep the actual occlusion provider
default OFF while public PE/MSAA/fault breadth and full-sequence qualification
remain open. StateBlock whole-snapshot bypass is still unsupported by H320.
