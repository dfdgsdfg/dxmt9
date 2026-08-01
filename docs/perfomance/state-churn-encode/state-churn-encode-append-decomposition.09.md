---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 09
title: 60% Of The Draw Entry Is An SWVP Probe That Reads The Index Buffer Before Asking Whether SWVP Applies
date: 2026-08-01
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05-gt2-drawphase-n64; experiments/output/app-d3d9-3dmark05-gt2-drawphase-n16
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.08.md
---

# 60% Of The Draw Entry Is An SWVP Probe That Reads The Index Buffer Before Asking Whether SWVP Applies

**Question / hypothesis.**
[append-decomposition.08](state-churn-encode-append-decomposition.08.md) found
the draw entry point at `11,493 ns/call` — `19.5 ms/present`, `37%` of the GT2
frame on its own, against `2,121 ns` for the `appendRecordDirect` inside it.
What is the other `9.4 us`?

**Method.** Two sub-phases inside the draw entry, timed only when the parent
entry scope was sampled so the phases are comparable to each other and to their
parent rather than being independent populations: `swvp` (the two SWVP fallback
probes plus the containers they fill) and `record` (the actual draw-record
append). `entry - swvp - record` is the rest. `DrawPrimitive` and
`DrawIndexedPrimitive`, GT2, `perf`, two decimation rates.

**Result.** The two rates agree within `2.2%`.

| | `N=64` | `N=16` | share of entry |
|---|---:|---:|---:|
| draw entry | `12,164 ns/call` = `20.61 ms/present` | `12,362` = `20.80` | |
| **swvp probes** | **`7,264 ns`** = **`12.31 ms/present`** | **`7,421`** = **`12.48`** | **`60%`** |
| record append | `4,126 ns` = `6.99` | `4,388` = `7.38` | `34%` |
| rest of call | `773 ns` = `1.31` | `552` = `0.93` | `5%` |

**`12.3-12.5 ms/present` — `~23%` of the GT2 frame — is spent probing whether
software vertex processing applies.** It never does: GT2 is a hardware-VP
workload.

**Verdict — the probe does its work before its own predicate.**
`trySoftwareFfpDrawIndexedPrimitive` runs, in order:

```
out = {}; indices.clear();                       // clear the containers
indexCount = primitiveVertexCount(...)           // cheap
readSoftwareFfpAdjustedIndices(...)              // READS AND ADJUSTS EVERY INDEX
  -> if that fails, return
trySoftwareFfpTransformBoundVertices(...)
  -> describeSoftwareFfpDrawTarget(...)
       if (!softwareVertexProcessing_ || vs_ != nullptr) return S_FALSE;   // <-- the answer
```

The applicability test is **two pointer/bool comparisons on device state**, and
it sits two calls below a full index-buffer read that the answer makes
pointless. On a device that never created with software vertex processing, or
that has any vertex shader bound, every draw pays the index read to be told
`S_FALSE`.

This is the same shape as the ungated clock reads in
[.07](state-churn-encode-append-decomposition.07.md) — work performed and then
discarded — but an order of magnitude larger: `~23%` of the frame against
`~1.9%`.

**The fix is a hoist, not a rewrite.** Lift the `softwareVertexProcessing_ &&
!vs_` test (or `describeSoftwareFfpDrawTarget` whole, which is pure state
inspection and fills only out-parameters) above `readSoftwareFfpAdjustedIndices`
in both probes. It reads no indices and has no dependency on them, so the
reorder is semantically inert for the S_FALSE path and unchanged for the SWVP
path.

**Not implemented or measured here, deliberately.** The prediction is
`~12 ms/present` of producer CPU removed on GT2, which at the frame level is
`~23%` — but this day has established twice that CPU removed and wall clock are
different currencies, and that the conversion ratio is unidentified somewhere in
roughly `[0.3, 1.5]`
([.02 correction](state-churn-encode-append-decomposition.02.md)). A removal
this large is also the first candidate all year big enough to measure the
conversion ratio *properly*, which the underpowered `.07` attempt could not.
That A/B is the next step and it deserves its own leaf.

**Scope.** One run per decimation rate, GT2 only. A workload that genuinely uses
software vertex processing pays the probe legitimately and would see no gain —
but it would also not reach the `S_FALSE` early-out, so the hoist cannot hurt
it. The `record` phase at `4,126 ns` against `appendRecordDirect`'s `2,121 ns`
is consistent: the remainder is `flushPendingConsts` and parameter construction
inside `appendDrawIndexedPrimitiveRecord`.

**Related.**
[append-decomposition.08](state-churn-encode-append-decomposition.08.md) ·
[append-decomposition.07](state-churn-encode-append-decomposition.07.md) ·
[frame-lifecycle](../frame-lifecycle.md)
