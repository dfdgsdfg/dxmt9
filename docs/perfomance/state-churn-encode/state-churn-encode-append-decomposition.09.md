---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 09
title: 60% Of The Draw Entry Is An SWVP Probe That Reads The Index Buffer Before Asking Whether SWVP Applies
date: 2026-08-01
type: experiment-run
status: accepted-attribution; proposed fix corrected 2026-08-01 after review
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

**The cost is paid twice per indexed draw.** The FFP probe returns `S_FALSE`
after its full read, so `trySoftwareProgrammableDrawIndexedPrimitive` runs and
**repeats the identical read** before its own predicate rejects. The phase timer
wraps both, so the numbers above already include the doubling.

**The fix is a hoist, not a rewrite — but not the hoist first written here.**

> **Corrected 2026-08-01 after review.** This section originally said to lift
> "the `softwareVertexProcessing_ && !vs_` test … in both probes". **That would
> break the programmable fallback.** The two predicates are *complementary* on
> `vs_`:
>
> | probe | predicate | file |
> |---|---|---|
> | FFP | `!softwareVertexProcessing_ \|\| vs_ != nullptr` | `d3d9_pe_device.cpp:4518` |
> | programmable | `!softwareVertexProcessing_ \|\| !vs_` | `d3d9_pe_device.cpp:5124` |
>
> Hoisting the FFP test into the programmable probe returns `S_FALSE` for every
> draw with a bound vertex shader — on a genuine SWVP device that silently
> disables programmable software vertex processing entirely, and GT2 (hardware
> VP) could never catch it. The original safety argument — "a workload that
> genuinely uses SWVP would not reach the S_FALSE early-out, so the hoist cannot
> hurt it" — is exactly wrong for that case.
>
> **The correct hoist is the shared conjunct only:**
> `if (!softwareVertexProcessing_) return S_FALSE;` in both probes, or one check
> at the `DrawIndexedPrimitive` entry gating both calls — which is how the UP
> variants already work (`trySoftwareFfpDrawPrimitiveUP` calls describe first).
> Each probe keeping its *own* full predicate hoisted is equally correct.

**And it is not semantically inert.** `readSoftwareFfpAdjustedIndices` returns
`D3DERR_INVALIDCALL` on byte-count overflow, on an index range exceeding the
index buffer, and on a null mapping — and both probes forward that, so the entry
point drops the draw and returns `D3DERR_INVALIDCALL` to the app.
`appendDrawIndexedPrimitiveRecord` performs no equivalent bounds check. **Today,
on a hardware-VP device, an out-of-range `DrawIndexedPrimitive` fails at call
time as a side effect of a probe that cannot apply.** After the hoist it would
be recorded and forwarded. Retail D3D9 does not validate this, so removing it is
probably *desirable* — but it is a behaviour change on the path this document
first called inert, no native spec covers these probes, and whichever behaviour
is chosen should be pinned by one.

**Not implemented or measured here, deliberately.** The prediction is
`~12 ms/present` of producer CPU removed on GT2, which at the frame level is
`~23%` — but this day has established twice that CPU removed and wall clock are
different currencies, and that the conversion ratio is unidentified somewhere in
roughly `[0.3, 1.5]`
([.02 correction](state-churn-encode-append-decomposition.02.md)). A removal
this large is also the first candidate all year big enough to measure the
conversion ratio *properly*, which the underpowered `.07` attempt could not.
That A/B is the next step and it deserves its own leaf.

**The `7,264 ns` is fully accounted for by the index loop.** GT2 runs
`~1,660` indexed draws/present over `2.088e9` primitives — about `3,180` indices
per draw, read twice, so `~6,360` element iterations at **`1.14 ns/element`**.
That is exactly a compare/adjust loop plus one vector allocation per draw, and
it leaves little room for anything else hiding in the span.

That matters for a hypothesis worth recording and then bounding: the probe locks
the index buffer `READONLY` on every draw, and a lock that crosses the bridge
blocks on the commit-replay drain fence
([.206](../present-pacing/present-pacing-drain-fence-attribution.206.md) —
`dxmt9c_buffer_lock` is `99.8%` of it). If most probe locks crossed, part of the
`swvp` span would be *blocking*, not CPU. They do not: the PE readonly cache
serves MANAGED-pool readonly locks before the bridge, and the counters show
`61.53` readonly locks/present reaching unix against `1,660` indexed draws — a
`~96%` hit rate. The mechanism is real, the magnitude is small, and the
element-count arithmetic above independently leaves no room for it.

**Scope.** One run per decimation rate, GT2 only. A workload that genuinely uses
software vertex processing pays the probe legitimately and would see no gain
from the hoist — but with the corrected predicate it also loses nothing. The
`record` phase at `4,126 ns` against `appendRecordDirect`'s `2,121 ns` is
consistent: the remainder is `flushPendingConsts` and parameter construction
inside `appendDrawIndexedPrimitiveRecord`. The draw entry bucket is shared with
the UP entry points, which record no sub-phases, so the `rest` residual mixes
populations — UP and non-indexed draws are `~1.1%` of calls, so the `60/34/5`
split is unaffected.

**Related.**
[append-decomposition.08](state-churn-encode-append-decomposition.08.md) ·
[append-decomposition.07](state-churn-encode-append-decomposition.07.md) ·
[frame-lifecycle](../frame-lifecycle.md)
