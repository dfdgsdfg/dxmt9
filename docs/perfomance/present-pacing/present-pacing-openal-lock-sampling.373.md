---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2
title: "H373: Bounded OpenAL lock ownership sampling"
type: evidence
status: partial-wake-timeline
updated: 2026-09-26
source: traces/app-d3d9-stkcop-openal-timeline-h373/{poll.c,run.py,snapshot.txt,receipt.jsonl,analyze.py,analysis.json}; experiments/output/app-d3d9-stkcop-bench-openal-timeline-h373/result.json
related: present-pacing-guest-wait-ownership.372.md
---

# H373: Audio lock occupancy is substantial; precise wake timing stays open

A five-second external read-only poll starts after the supervisor observes
frame 2700. Unlike H372, it never suspends target threads. It locates the loaded
`wrap_oal.dll`, checks its lock helper instruction/relocated operand, and reads
the 24-byte x86 critical-section header at module RVA `0x106520`. Requested
sleep is 2ms; QPC brackets each observation. This is a diagnostic lane, not an
observer-OFF performance comparison. Runtime code and audio provider are unchanged.

The Day/high 1280x720 `-max-threads 2` catalogue run passes, exits zero and reports
zero GPU command-buffer errors. All five staged binary hashes match H372c.

| Measurement | Result |
|---|---:|
| Complete lock-header reads | 1,663 / 1,663 |
| Owned samples | 1,382 (83.10%) |
| Owner `0x13c` (WinMM thread in H372) | 1,379 samples |
| Owner `0x144` | 3 samples |
| No owner | 281 samples |
| Median / maximum sampling interval | 3.1002 / 4.4730ms |
| Total bracketed read/owner-query scope | 201.8263ms |
| Longest consecutive same-owner sampled span | 107.6589ms, 37 samples |

The last row is **not a continuous hold duration or even its lower bound**:
unobserved release/reacquire can occur between samples. The header itself is
read non-atomically; owner/recursion fields can be transitional. The ownership
fraction is a sample fraction, not a measured wait-time fraction. Polling is
not free: the bracketed scope occupies roughly 4% of the five-second observer
window and includes observer scheduling; it is not game CPU overhead.

`GetThreadTimes` succeeded but returned zero CPU deltas for both observed
owners. Treat CPU attribution as unavailable under this measurement/runtime,
not as evidence that the callback consumes no CPU. Owner handles are retained
while unlocked, so CPU intervals would not isolate lock-held work even with
useful counter values.

## Interpretation and next gate

H372's concrete waiter/owner chain is reinforced without stopped-thread
sampling: the audio lock is frequently observed owned. Short unlocked periods
and owner transfers exist, so these data do not show a permanently stuck lock.
Do not attribute all rear-scene slowdown or audible interruptions to audio.

This experiment does **not** complete recursive Enter/Leave event tracing,
Wine wake delivery, or game-thread resume timing. It deliberately avoids
patching third-party DLLs or repeating the failed debugger attachment.
Closing that edge needs an explicit entry/exit observer at the matched Wine
critical-section implementation (or a separately qualified reversible hook),
with PID/TID/object identity, recursion, timestamps and scheduler resume evidence.
Polling alone cannot supply that proof. Retain this result as occupancy evidence,
not as the requested end-to-end wake timeline or FPS improvement evidence.
