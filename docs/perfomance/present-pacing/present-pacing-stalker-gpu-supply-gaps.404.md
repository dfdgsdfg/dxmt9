---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "H404: Middle and late GPU execution gaps precede submission"
type: evidence
status: gpu-saturation-weakened-pre-submit-owner-open
updated: 2026-09-27
source: traces/app-d3d9-stkcop-bench-critical-h404/{run.py,export.py,analyze.py,invocation.json,summary.json}; traces/app-d3d9-stkcop-bench-critical-h404/{middle,late}-metal-gpu-intervals.xml; traces/app-d3d9-stkcop-bench-critical-h404/{middle,late}-metal-application-command-buffer-submissions.xml; traces/app-d3d9-stkcop-bench-critical-h404/{middle,late}-time-sample.xml; experiments/output/app-d3d9-stkcop-bench-critical-h404/result.json
related: present-pacing-stalker-covered-pending-validation.403.md; present-pacing-stalker-renderer-comparison.399.md
---

# H404: The sampled late collapse is not continuous GPU saturation

## Method and qualification

HEAD `dbc216d5`, same five staged images and requested/final/control hashes
as H403. Sikarugir CX24.0.7, Day/high, 1280x720 window, two game threads,
16 sound targets. Hold both actual Metal occlusion and covered-Pending OFF.
Runtime counters, frame sampling and query observers are OFF. Xcode 27
`xctrace` Metal System Trace attaches to `xrEngine.exe` PID 52712 for two
8-second recordings, requested 100 and 165 seconds after PID discovery.
Apple M1 eight-core GPU, 16 GiB, macOS 27.0; toolchain doctor passed.

Analyze trace-relative `[1,7)` seconds in each recording, excluding attach
edges. Names `middle` and `late` are process-relative sampling labels, not
matched camera identities or exact benchmark-relative timestamps. There is
one diagnostic run, no concurrent benchmark/build. It exits normally and
passes the runner. The saved scene screenshot contains rendered geometry;
this is not a pixel oracle. Engine 28.289516 FPS is diagnostic only and is
not compared to observer-OFF reference FPS.

Export table schemas from each TOC, resolve XML id/ref values, use raw
nanosecond fields, and union overlapping Active GPU intervals across Vertex
and Fragment channels. Filter app ownership by process, not label alone.
This avoids summing overlapping stage/CB times and calling that wall time.
The analyzer checks clipping/overlap examples and interval bounds. Raw traces
and exports remain under the ignored run directory; no capture-layer patch
or runtime implementation change was required.

## GPU execution versus elapsed time

| Six-second window | Middle | Late |
|---|---:|---:|
| App GPU Active interval union | 2.77848 s (46.31%) | 0.95355 s (15.89%) |
| All reported GPU Active interval union | 2.97535 s (49.59%) | 1.01258 s (16.88%) |
| App Vertex interval union | 0.51455 s | 0.25874 s |
| App Fragment interval union | 2.38857 s | 0.73223 s |
| Present requests | 231 (38.5/s) | 72 (12.0/s) |
| Longest internal app GPU gap | 25.708 ms | 176.047 ms |
| App GPU gaps over 1 ms | 464 | 163 |
| Exported command-buffer error rows | 0 | 0 |

Channel unions overlap and must not be added. These are reported execution
intervals, not ALU occupancy, bandwidth saturation or a full-workload GPU
optimization upper bound. Present request rate is diagnostic and not a
replacement for benchmark-owned FPS. Both windows contain game RenderPass
and Present labels. WindowServer accounts for nearly all additional reported
GPU activity; the selected windows have no unattributed Active duration.

## Large gaps occur before the next commit

Join each app GPU CB intersecting the window to the application's
CommandBufferSubmission row by command-buffer ID: 1,158 middle / 384 late,
with zero missing joins. Infer commit at the end of that submission interval
(`start + duration`); every joined GPU execution starts after it.
Creation-to-commit duration is not CPU running time.

| Late gap | Next CB commit after gap begins | Commit to next GPU execution |
|---|---:|---:|
| 176.047 ms | 175.618 ms | 0.430 ms |
| 162.617 ms | 162.137 ms | 0.480 ms |
| 151.646 ms | 151.163 ms | 0.482 ms |
| 128.124 ms | 127.693 ms | 0.431 ms |

The largest gap is trace-relative 2.9271345–3.103181833 s; the following
execution is `cb_seq_16361:Clear[...]`. The middle maximum similarly has
25.253 ms before commit out of a 25.708 ms GPU gap.

Union of inferred commit-to-last-GPU intervals for these joined CBs covers
3.03085 s middle / 1.06769 s late. This is evidence about CBs with GPU work
in the window, not a complete queue-empty proof: it does not inventory every
empty, future or blocked CB. Nevertheless the longest observed gaps are not
explained by the following work already being submitted and then spending
the entire gap executing or queued on the GPU.

## CPU ownership: useful lead, incomplete attribution

The raw `time-sample` exports succeed. Within both selected windows they
contain only Timer Fired / Running samples. The template does not record
waiting threads continuously, so missing samples must not be called blocked
time and running sample counts must not be summed into elapsed wall time.

Inside the late 176.047 ms GPU gap:

| Thread | Running samples |
|---|---:|
| unnamed `xrEngine.exe`, native TID `0x103d22e` | 167 |
| X-RAY Secondary, TID `0x103d35e` | 144 |
| unnamed TID `0x103da2c` | 76 |
| unnamed TID `0x103d343` | 74 |
| X-RAY Primary, TID `0x103d0cc` | 13 |
| dxmt9-encode, TID `0x103d34e` | 0 |

This does not resemble a continuously busy encoder in that particular gap.
It motivates following the game Secondary and the unnamed busy thread before
assuming more encode parallelism is the answer. Roles of unnamed threads and
the Primary's wait/runnable state remain unproven.

Both `time-profile` symbolicated exports terminate with return code -5;
raw CPU samples and GPU tables are valid and retained. `dyld-library-load`
exports no rows for the attach interval, so it cannot supply the missing
address map. Do not manufacture function attribution from opaque Rosetta
addresses. No TLA/native test rerun is needed to claim a runtime change:
there was no runtime change.

## Decision and next gate

- Continuous GPU saturation is not supported in these sampled windows,
  especially the late one. Pre-submit supply/progress is now a stronger
  target than GPU shader throughput or repeated Pending mutex checks.
- The result does not assign the delay to PE, replay, encode, Wine or game
  code; CPU scheduling, dependencies and query synchronization remain possible.
- Next capture native/guest stacks plus thread identity and wait/waker evidence
  around late no-submit gaps, focusing on Secondary and TID `0x103d22e`'s role.
  Recover a reliable symbolication path before adding another runtime knob.
- Actual visibility correctness and D9VK matched-scene workload/critical-path
  comparison remain open; this single dxmt9 trace does not explain the entire
  1.825x renderer gap.

No runtime defaults, README, commit or push changed.
