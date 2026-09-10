---
domain: mini-replay-bisection
workload: 3DMark05 GT1
title: "Mini-Replay Bisection — the apparatus that isolated per-draw vertex-stage write amplification - Current Overview"
type: domain-overview
status: current
updated: 2026-09-10
source: docs/perfomance/mini-replay-bisection/log.md; docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/mini-replay-bisection/index.md; docs/perfomance/mini-replay-bisection/log.md
---

# Mini-Replay Bisection — the apparatus that isolated per-draw vertex-stage write amplification - Current Overview

> Current, compact view for this performance domain. Historical detail from the former
> top-level `mini-replay-bisection.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

## Scope

This domain owns the **methodology**: build a standalone, row-local mini-replay
harness that consumes real captured index/stream/cbuf/depth payloads plus dumped
shaders, reproduce the original hot-encoder `VS Buffer Device Memory Bytes Written`
pressure at encoder scale outside Wine/D3D9, then bisect it down to individual
shader-pair / draw windows. It answers: *which concrete inputs reproduce the GT1
hidden vertex-stage write bucket, and at what granularity is it owned?* This is
the apparatus that made [tvb-mechanism-proof](../tvb-mechanism-proof/index.md) possible and that backs the
[index-cache-locality](../index-cache-locality/index.md) win.

## Latest Conclusions

The [qualified low-observer CPU policy interface](cpu-policy-measurement.02.md)
now compares unchanged GT2 input with separate semantic qualification and
measurement processes. A/A, compatibility command-limit 0/4 and Direct
exact/headroom comparisons pass with 1,420 identical ordered draws. Actual
source-count and provisioning changes are observed; iteration service timing
omits detailed proof work but retains runtime counters. This is a bounded
replay/Slot experiment interface, not an encode/GPU or wild FPS result.

The [fixed-input replay experiment](fixed-input-replay.01.md) adds a separate
production replay/Slot policy loop: authenticated PE Tape, same-process warm
Metal repetition and bounded completion fixtures with actual Metal-buffer
bytes but no GPU submissions. Direct/compatibility, external completion
schedules, a real Managed allocation-failure/retry case, and the grouped
seven-Direct/two-compatibility capacity fallback pass; small captured
Metal paths also pass. A fresh full-frame GT2 identity-v2 capture now
validates at the explicit 64-record cadence. The unchanged full frame now
passes the CPU comparator 16 times per route: 1420 draws, including 1410 Direct
and 10 compatibility draws in the candidate, with matching ordered semantics
and actual buffer-byte observations. An explicit source-projection oracle now
qualifies a one-source/one-mutation completion window: GT2 r144 passes 16
repetitions per route, with three deferred DISCARDs and 20 retained-buffer byte
checks per replay. Every draw also matches the immediate-completion reference;
the production visitor rechecks all 39 source closures. Actual A-to-B-to-A
reset and surviving-surface/retired-parent attribution controls now pass, with
decisive native faults; validation-enabled CPU runs are diagnostic only.
Source-owned pending proofs now reject six stale identity dimensions. Native
two-source FIFO lifetime and real 31-source capacity saturation/reclaim pass
on both routes. Public two-source scheduling now passes GT2 r102 at actual
pending peak two: all 16 repeats per route conserve 39 sources and match every
draw against a separate immediate-completion control. Input-derived whole-source
eligibility, per-owner FIFO proofs and actual isolated-runtime admission bound
this CPU-only window; noneligible sources drain before a later eligible source.
CPU input staging now requests independent copy-on-write copies on macOS,
allowing the fresh GT2 comparison without deleting prior evidence. Canonical
mipmap command/lifetime tests also pass. Active negotiated explicit mip capture
now records a canonical source and uses its exact completion ticket; unsupported
pre-arm/direct calls remain explicitly rejected. The completion helper and
capture-abort fence pass CPU native tests, while actual PE COM CapacityPost
fault integration remains separate. A conditional progress model detects the
missing-release wedge without claiming Metal driver progress. A subsequent
native production-loop test exposed lost exact-event identity during singleton
physical retirement. Identity now transfers through the queue receipt and
ordered finish FIFO; native source reuse, receipt ABA, mixed resident/receipt
ordering and actual C API acknowledgment tests pass. This proves completion of
an unsubmitted production source with a CPU-controlled backend, not GPU progress
or observation of the coordinator asleep on its CV. The bounded CPU R8
policy/lifetime harness is now qualified: explicit reset/cache/device sharing,
phase/process-CPU receipts, 16-repeat external plans and retained failure
reproduction pass. Bounded phases are exclusive and include teardown; general
phase observations retain their own accounting semantics. The final gate passes
25 native tests and five x86_64 tests; the lifecycle models retain their full
188-configuration TLA pass. This is a CPU experiment tool, not a promotion gate.
Both Metal replay policies still produce
the same capture-oracle mismatch; strict pixels and broader delayed
multi-object lifetime qualification remain open. It does not replace the GPU-slice
methodology below or measure game FPS.

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H18 | Primitive-conflict metrics can split rank-1 visible failure from rank2-4 exact owner-masked passes | rejected; only final-color metrics separate | mini-replay-bisection-texture.07 |
| H19 | Existing D3D9 occlusion query or winemetal visibility plumbing can supply the missing runtime oracle as-is | rejected as production oracle; diagnostic scout added separately | [mini-replay-bisection-texture.08](mini-replay-bisection-texture.08.md) |
| H20 | A diagnostic Metal visibility scout can supply per-Metal-draw sample-visible counts after GPU completion | implemented diagnostic; not final-color proof; old rank-1 `36..37` is sample-visible | [mini-replay-bisection-texture.09](mini-replay-bisection-texture.09.md) |
| H21 | No-sample visibility rows are the hidden-backend hot locality owner | rejected; zero rows are small and only `-2,016` of `-182,856` LRU32 delta | mini-replay-bisection-texture.10 |
| H22 | Positive Metal visibility samples can be used as the scoped depth-read final-color oracle | rejected; rank2 has `39,835` samples but `0` final-color pixels, and rank1/rank3 are both sample-positive but fail/pass diverge | mini-replay-bisection-texture.11 |

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [mini-replay-bisection-texture.09 - Metal Visibility Scout Wiring](mini-replay-bisection-texture.09.md)
- [mini-replay-bisection-texture.08 - Occlusion Oracle Feasibility Gate](mini-replay-bisection-texture.08.md)
