---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "L64 production wild ABBA: small FPS signal, visual gate open"
type: experiment
status: exploratory-no-promotion
updated: 2026-10-05
source: traces/stkcop-l64-h444/{run.py,analyze.py,summary.json}; traces/stkcop-l64-h444/{a1,b1,b2,a2}/{invocation.json,completion.json}; experiments/output/app-d3d9-stkcop-bench-l64-h444-{a1,b1,b2,a2}/{result.json,daybenchmark.result}
related: docs/perfomance/present-pacing/present-pacing-v4-final-grouping.443.md
---

# L64 production wild comparison

Follow-up [H445](present-pacing-l64-wild-diagnostic.445.md) observes persistent
missing geometry in a visible L64 diagnostic run. The FPS signal below is not
an accepted equivalent-rendering improvement; promotion is blocked pending
observer-OFF/foreground-controlled reproduction and correctness closure.

Committed V4 work at `8208639c`, rebuilt both PE architectures and the x86
Unix provider, and passed the provider install-name audit. Initial PE build
failed because llvm-mingw was absent from PATH; adding the installed toolchain
resolved it. Both PE builds retain the existing unused `g_ntAllocScanHint`
warning. No runtime code or default policy changed for this experiment.

## Policy correspondence and method

The V4 CPU CLI sets `DXMT9_DRAW_CHUNK_COMMAND_LIMIT` from its requested limit.
Production uses the same cached `drawChunkCommandLimit` resolver in
`src/dxmt9/dxmt9_command_queue.cpp`: compatibility batch segmentation checks
prospective command count, and `maybeCommitDrawChunkUnlocked` publishes with
reason `DrawLimit`. The unit is commands, not individual draws.

H443 fixed the Compatibility route; this wild comparison leaves production
route selection unchanged. Thus it tests applicability to the actual mixed
workload, not identical routing or source identities to the cold harness.
Query/control boundaries remain unchanged. No new runtime selector is needed.

Sequential A/B/B/A uses limit 0/64/64/0, Sikarugir CX24.0.7, Day/high,
1280x720 windowed, two game threads, and 16 sound targets. Perf profile,
runtime counters and frame observers OFF; no external profiler. All four
finish successfully without timeout and generate engine results. The five
staged binary receipts and requested/final/control configuration hashes match.
These receipts prove staged-image parity, not an independent loaded-module
trace. No concurrent build or heavy analysis; external load is uncontrolled.

## Engine results

| Run | Limit | Average FPS | p95 frame ms | p99 frame ms | Last 10s FPS |
|---|---:|---:|---:|---:|---:|
| A1 | 0 | 28.6023 | 75.49 | 100.80 | 13.09 |
| B1 | 64 | 29.2301 | 68.74 | 88.18 | 15.29 |
| B2 | 64 | 29.6683 | 68.88 | 96.63 | 15.89 |
| A2 | 0 | 29.1521 | 64.96 | 90.38 | 17.08 |

Forward/reverse average differences are +2.20%/+1.77%. Baseline itself rises
from 28.60 to 29.15 FPS. The tail is not consistently better: B2 loses to A2
in last-10s FPS and p95/p99. Maximum frame times are 414/596/262/253 ms;
frames over 100 ms are 35/23/31/26. Tail windows reconstruct time by summing
reciprocals of engine per-frame FPS; they are not matched camera/draw identities.

## Limits and decision

All automatic screenshots fall back to desktop capture because no onscreen
game window is found. Computer-use independently finds no visible game window.
Visual correctness and foreground/focus qualification therefore remain open.
The first log's `OA not initialized` initially looked like failure, but the
engine subsequently completed and produced a fresh result; that message alone
does not invalidate completion or establish its cause. It does not repair the
missing visual gate either.

This is a small positive average-FPS signal, not proven single-thread CPU
savings, GPU timing, tail repair, or promotion evidence. H443's CPU result
cannot be substituted for CPU attribution in this mixed production workload.
Keep defaults unchanged. Next obtain a visible, interference-free comparison
and a separate bounded diagnostic pair for effective DrawLimit publications,
replay/encode work and queue/completion waits before deciding on promotion.
