---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 31
title: Surface-Lock Shadow — A 1.5 ms Address Scan, A 93% Counter Win, And An FPS Null
date: 2026-08-21
type: experiment-run
status: accepted-mechanism
source: experiments/output/app-d3d9-3dmark05-slock-attr-r1; experiments/output/app-d3d9-3dmark05-slock-attr-r2; experiments/output/app-d3d9-3dmark05-slock-pool-r1; experiments/output/app-d3d9-3dmark05-pool-{a1,a2,a3,b1,b2,b3,b4}
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.26.md
---

# Surface-Lock Shadow — A 1.5 ms Address Scan, A 93% Counter Win, And An FPS Null

**Attribution chain** ([.26]'s ledger item ②, `aa116558`/`ba71def5` counters):
`dxmt9c_surface_lock_rect`'s 1.33 ms/call is **not** the core lock (0.2 µs/call)
and not DISCARD zero-fill (zero DISCARD surface locks on GT2) — it is the
wow64 pointer-shadow block, and inside that, **the allocation**:
`allocateLow4GB` at 1,528 µs/call firing on 92.7% of shadowed locks (712/768
per run; the per-surface reuse cache never warms because the app locks fresh
surfaces ~every 2 frames), while the copy is 50 µs/84 KB. The cost is the
`NtAllocateVirtualMemory` low-address scan — a syscall per failed attempt
from a fixed 0x10000000 start.

**Fix** (`7302fa32`): a bounded size-classed low-4GB block pool (64 KB–8 MB
buckets, 8/bucket, 64 MiB cap, own mutex — the replay worker can drive
releases) plus a persisted scan-start hint for the slow path.
**Counter result: shadow 0.665 → 0.050 ms/present (−93%), alloc 1,528 → 55
µs/call.** The honest twist: `pool_hits=7` — the pool cannot help THIS
workload because the app releases its surfaces in end-of-scene batches, so
there is no release↔re-acquire temporal overlap; **the scan-start hint did
the whole recovery**. The pool stays as bounded, tested insurance for
churn-shaped apps.

**FPS: null, honestly read.** B-first bracket (B×3 → A×3 → closing B):
the B arm showed a warm-up ramp (25.89 → 26.20 → 27.71, closing 27.54) after
the preceding conformance run; the naive full-arm delta (+2.8%) is that
drift, not the fix. On the stable samples, B(b3,b4)=27.63 vs
A=27.59±0.12 — **flat**. Conformance 234/235, zero GPU errors.

**Why a real 0.64 ms/present did not convert:** the cost was not a
per-frame tax but a ~1.5 ms burst on the ~every-other-frame new-surface
lock, and bursts of that size sit inside the producer's present run-ahead
slack. Method lesson for the ledger, extending [.27]'s noise-floor rule:
**a per-present average alone does not predict fps conversion — check the
burst structure**. A cost that lands as occasional multi-ms spikes may be
entirely absorbed by frame-latency slack, where the same average spread
per-frame (like the [.25] crossing tax) converts near 1:1. The change stays
merged on its merits: real work removed, worst-case lock spike cut from
~1.5 ms to ~55 µs (hitch-tail relief), no cost anywhere.

**Ledger update.** [.26] item ② is closed (mechanism). Remaining GT2
producer items: linear-scan batch (~0.45 ms, spread per-call — should
convert), T2b/T2c producer acquire elimination, lock/unlock unix work
(2.6 ms — now with the caveat that its conversion also needs burst-structure
analysis first), commit mark residuals.
