---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 09
title: 62% Of The Draw Entry Is An SWVP Probe That Reads The Index Buffer Before Asking Whether SWVP Applies
date: 2026-08-01
type: experiment-run
status: accepted-attribution; proposed fix corrected 2026-08-01 after review
source: experiments/output/app-d3d9-3dmark05-gt2-drawphase-n64; experiments/output/app-d3d9-3dmark05-gt2-drawphase-n16
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.08.md
---

# 62% Of The Draw Entry Is An SWVP Probe That Reads The Index Buffer Before Asking Whether SWVP Applies

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

**Result.** The two rates agree within `2.2%` on the entry and the swvp phase
(the record phase disagrees `6.3%` — see Scope).

| | `N=64` | `N=16` | share of entry |
|---|---:|---:|---:|
| draw entry | `12,164 ns/call` = `20.61 ms/present` | `12,362` = `20.80` | |
| **swvp probes** | **`7,264 ns`** = **`12.31 ms/present`** | **`7,421`** = **`12.48`** | **`60%`** |
| record append | `4,126 ns` = `6.99` | `4,388` = `7.38` | `34%` |
| rest of call | `773 ns` = `1.31` | `552` = `0.93` | `5%` |

> **Corrected 2026-08-01 after review — the denominator above is inflated.** The
> two phase timers cost the draw entry `~671 ns` of their own clock reads
> ([.10](state-churn-encode-append-decomposition.10.md)), so `12,164` is the
> instrumented entry and the true one is `11,493` (measured on the build without
> them). Each phase span holds one of those reads (`~168 ns`); the other two land
> in the residual. Corrected, the decomposition closes **exactly**:
>
> | | true ns/call | share of a clean entry |
> |---|---:|---:|
> | swvp probes | `7,097` | **`61.7%`** |
> | record append | `3,959` | `34.4%` |
> | rest of call | `437` | `3.8%` |
> | sum | `11,493` | `100.0%` |
>
> The `5%` "rest" was `~77%` instrument. The swvp headline is unaffected:
> `7,097 × 1,694` = **`12.02 ms/present`, `22.6%` of the frame**.

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

**The `7,097 ns` is fully accounted for by the index loop.** GT2 runs
`~1,660` indexed draws/present over `2.088e9` primitives — about `3,180` indices
per draw, read twice, so `~6,360` element iterations at **`1.12 ns/element`**.
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
`~96%` hit rate (`~98%` per lock, since each draw locks twice). The mechanism is
real, the magnitude is small, and the element-count arithmetic above
independently leaves no room for it. A hard ceiling settles it regardless:
`offload_drain_fence_wait_ms` is `2,667.5 ms / 1,194` presents =
**`2.23 ms/present` of drain-fence blocking in the whole process**, so even
attributing every microsecond of it to this span — which
[.207](../present-pacing/present-pacing-drain-fence-attribution.207.md)
contradicts, having classified the blocked subset as DISCARD-skewed writes —
leaves `>= 10 ms/present` of real CPU in the probe.

**Scope.** One run per decimation rate, GT2 only. A workload that genuinely uses
software vertex processing pays the probe legitimately and would see no gain
from the hoist — but with the corrected predicate it also loses nothing.

The `record` phase closes against the append scope, which is the strongest
available check that the draw-side instrument is coherent — but not the way this
document first said. It read `4,126 ns` against `appendRecordDirect`'s
`2,121 ns` and called the `~2 us` difference "`flushPendingConsts` and parameter
construction". **A draw's record phase contains more than one append**:
`3,104,430` appends against `1,933,451` draws is `1.606` appends per draw,
because `flushPendingConsts` emits the pending const and state records inside
the same phase. Correcting both figures for instrument cost — the phase pays one
clock pair (`~183 ns`) out of its parent's span, and `appendRecordDirect` itself
nests two parent-gated phase timers worth `~361 ns` that its own null read does
not remove — gives `1.606 × 1,760 = 2,827 ns` of append inside a true
`~3,943 ns` phase. The non-append remainder is **`~1.1 us`**, not `~2 us`, and
it closes to within `~28%` rather than being half the phase.

Two riders on the append figure itself. `2,121 ns` is a mean over a **bimodal**
population: one append in 64 seals the chunk and pays the `65.7 us` bridge
crossing, so `~0.8-1.1 us` of that mean is amortized tail — anyone optimizing the
typical append should use the flush-free mean, `~1.30 us`, which is the one
figure here that replicates tightly (`1,296-1,305 ns` across all four locked
runs). And the mean itself does not replicate: `append` disagrees `12.8%` across
rates **and `17.7%` between two `N=64` runs** (`2,121` vs `2,496`), with sampled
flush share wandering `1.27-1.88%` at fixed `N`. Chunk-period aliasing — both
`64` and `16` divide the 64-record period — is a real mechanism and was first
written here as if it explained the whole gap; it cannot, because it is
`N`-dependent and the spread is not. Sizing it properly needs the flush share
taken from `bridge_commit_chunk` counters, or an `N` coprime to 64. The draw
entry scope counts draws, not records, so it escapes this entirely.

The draw entry bucket is shared with
the UP entry points, which record no sub-phases, so the `rest` residual mixes
populations — UP and non-indexed draws are `~1.1%` of calls, so the `60/34/5`
split is unaffected.

**Related.**
[append-decomposition.08](state-churn-encode-append-decomposition.08.md) ·
[append-decomposition.07](state-churn-encode-append-decomposition.07.md) ·
[frame-lifecycle](../frame-lifecycle.md)
