---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 191
title: Offload Backpressure Attribution Closes The Mechanism Gate
date: 2026-07-06
type: no-gputrace
status: accepted-attribution-offload-lever-exhausted
source: experiments/output/app-d3d9-3dmark05-replay-offload-attrib-r6-20260706/result.json; docs/perfomance/present-pacing/present-pacing-commit-replay-offload.190.md; docs/superpowers/specs/2026-07-05-commit-replay-offload-design.md
related: docs/perfomance/present-pacing.md; specs/backend/design.md
---

# Present-Pacing H191 - Offload backpressure attribution

## Question

H190 left one residual against the offload spec's mechanism gate:
`bridge_commit_latency = 12.97 ms/present` looked like raw-queue push
backpressure throttling the app thread. Is it, and is queue-bound tuning the
next lever?

## Run

Single supervised 120 s attribution scout with four new counters
(`fdc3057f`): `offload_commit_app_cpu_ms` (app-thread offload-branch wall,
entry to return), `offload_push_backpressure_waits/_wait_ms` (counted only
when the bounded queue actually blocks a push),
`offload_worker_idle_wait_ms` (pop-idle), plus read-once queue-bound knobs
`DXMT9_OFFLOAD_QUEUE_CHUNKS` / `DXMT9_OFFLOAD_QUEUE_BYTES` (defaults 64 /
8 MiB) for the tuning lever that turned out to be unnecessary.

## Verdict

Accepted attribution; the backpressure hypothesis is dead and the offload
lever is exhausted.

- `offload_commit_app_cpu_ms = 1.083 ms/present` — the app thread's real
  commit cost after offload (raw enqueue `0.648`, retention/scan the rest).
  The inline path spent `~8.5 ms/present`; the offload removed `~87%` of it.
  The spec's mechanism gate (raw handoff `<= 2 ms/present`) **passes**
  against this counter; H190's `12.97` reading was `bridge_commit_latency`,
  which the deferred replay closes at worker-replay end and therefore
  measures commit-to-replay pipeline latency (queue residency), not
  app-thread blocking.
- `offload_push_backpressure_waits = 0` — the bounded queue never blocked a
  push in the whole run; the 64-chunk/8 MiB defaults are not a constraint.
- `offload_worker_idle_wait_ms = 44.792 ms/present` — the worker starves,
  waiting on the producer.
- `presents = 2022` (`+12.3%` vs the H190 baseline 1800; noise-equivalent to
  H190's 1996), `gpu_command_buffer_errors = 0`, ordinal waits `0`,
  drain fences `2`/run.

Frame accounting at `59.4 ms/present`: dxmt9's unix-side app-thread cost is
now `~1.1 ms` (1.8%). The remaining `~58 ms` is the producer itself — the
game's own CPU, PE-side d3d9.dll recording (`recordAppend`/`constFlush`
CPU, so far measured only under perturbing `DXMT9_PE_RECORDER_STATS`
instrumentation), and Wine/wow64 thunking. Further offload/queue tuning
cannot move average FPS by more than `~2%`.

## Next owner

The average-FPS frontier moves to the PE side: low-overhead measurement of
PE recording cost (a sampling profiler over the PE d3d9.dll paths or a
cheap cycle counter, not the perturbing stats mode), then recording-cost
reduction (const-flush shape, record append path) — or acceptance that the
residual wall is the game's own CPU on this SKU. Queue-side work (worker
replay cost, encode contention) affects only pipeline latency, not
throughput, while the worker idles 45 ms/present.
