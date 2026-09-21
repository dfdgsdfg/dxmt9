---
domain: present-pacing
workload: STALKER CoP Day
title: "H350: Real-source prepared-plan cost is dominated by ineligible small sources"
type: evidence
status: partial-attribution
updated: 2026-09-21
source: experiments/output/app-d3d9-stkcop-bench-prepared-h350-{off,on}-day/{result.json,actual.png,daybenchmark.result}; experiments/output/prepared-encode-shadow-20260921/stalker-{off,on}.csv; experiments/output/prepared-encode-shadow-20260921/stalker-on.json; experiments/output/prepared-encode-shadow-20260921/stalker-summary.json; experiments/output/prepared-encode-shadow-20260921/summarize_stalker.py
related: present-pacing-prepared-publication-shadow.349.md; present-pacing-stalker-access-cardinality.347.md
---

# H350: STALKER publication shadow

Commit `61fc003a`. Day/high, renderer_r2.5, 1280x720 windowed, Sikarugir
cx24.0.7, perf. OFF then ON with the same five staged artifacts; Unix SHA-256
prefix `8db3bd2744caeb12`. Native/Unix and both PE builds completed before runs.
No build, test or other benchmark ran alongside them.

Both runs explicitly enabled Metal occlusion, async fences, covered Pending
and publication dedup. Tape capture, CB join, END coalescing, parallel observer,
command/draw split and wait trace were OFF; partition mode was `identity`.
The supply window was 2450–2470 encoded Presents, inclusive. Only
`DXMT9_CPU_SUPPLY_PREPARED_OBSERVER` changed between diagnostic policies.
Screenshot delay differed: OFF 90s captured loading; ON 150s captured the scene.
Neither screenshot proves pixel equality. There was no GPU capture.

Both runners PASS, with no reported GPU command-buffer errors. Both supply
traces close with zero overflow/incomplete writers. OFF has zero prepared
events; ON has 1,209 complete generation-qualified shadow sources. All prepared
work runs on native TID 3482840, also the Replay TID; Encode is TID 3482826.
The shadow remains inside the existing publication queue-mutex section.

## Cost and disposition

The following are inclusive diagnostic CPU totals across the selected window,
not removable production costs or additive pipeline-stage times.

| Source population | Sources | Plan outcome | Total shadow CPU |
|---|---:|---|---:|
| Single command, bound to nonempty query scope | 1,146 | Success, but current parallel admission rejects query scope | 25.175 ms |
| Other single-command sources | 21 | Success | 0.506 ms |
| Multi-command sources accepted by builder | 21 | Success | 2.175 ms |
| Multi-command sources exceeding identity capacity | 21 | Rejected before any execution | 2.749 ms |
| All | 1,209 | 1,188 success / 21 capacity rejection | 30.605 ms |

The query join uses actual `ScopeBoundary` publication sequence and prior
nonempty scope. There is one publication owner and one shadow owner in this
process/window. Query-bound sources account for **82.26%** of shadow CPU.
Single-command sources are **96.53%** of attempts and use 25.681 ms total.
Structural success is not parallel execution eligibility.

| Phase | CPU total | Per-source median |
|---|---:|---:|
| Allocation/initialization | 12.224 ms | 8 us |
| Command/access projection | 6.169 ms | 2 us |
| Seal/structural validation | 1.272 ms | 1 us |
| Total including metadata observation/destruction | 30.605 ms | 19 us |

Stage timings include endpoint overhead. Their sums omit metadata events and
destruction included in total. Median values must not be added. The total is
about 1.457 ms per selected encoded Present, **diagnostic overhead**, not a
measured FPS opportunity.

All 1,209 attempts allocate the 149,576-byte storage: **180,837,384 bytes
(172.46 MiB) cumulative requested capacity**. This is allocation churn, not
RSS, peak retained memory, or measured memory-write traffic. Single-command
attempts account for 166.47 MiB of that sum.

Rejected sources contain only 264–277 commands. Every one allocated storage,
so these are not pre-allocation 1024-command denials. With at most 24 access
indices per command, these sources cannot hit the 8192-access or 1024-range
bounds either; the builder reaches its 256-identity ceiling. Full rejected
cardinality is not retained by this observer. Rejected-source append CPU totals
2.301 ms, median 101 us; total median is 122 us, maximum 313 us. Accepted plans
contain at most 79 identities. No bound was raised and no rejection was bypassed.

## Comparison limitations

OFF/ON native benchmark averages are 22.497402 / 20.690126 FPS. These are one
live pair, not frozen-input A/B or a promotion result. Within the same Present
ordinal window, OFF publishes 1,203 sources / 13,726 commands, whereas ON
publishes 1,209 / 11,797. Live camera time, polling and publication shape are
not work-identical; screenshot timing also differed. Do not attribute the
whole-run FPS delta to the 21-Present observer, subtract stage totals to claim
saved work, or infer the future prepared consumer's speed. Encode still
executes the unchanged original path.

## Decision

Do not adopt the current fixed builder unconditionally at publication.

1. Before allocation, apply cheap policy/query/coordinator and useful-work
   screening; current identity-serial execution needs no new parallel plan.
   Preserve exact serial behavior when preparation is skipped. Revalidate
   live Encode admission later; an early screen is not an execution certificate.
2. Replace the per-source maximum-sized scratch policy with reusable or
   count-sized storage, preserving private construction and failure-atomic
   ownership. Fuse projection into final Slot construction where justified;
   do not move this second walk under the production queue mutex.
3. Size the identity table from measured large-source/pass needs without
   expanding every child metadata record. H347 and this source-level rejection
   rule out treating 256 as a workload maximum. First retain exact over-bound
   cardinality and validate wide-resource semantics.
4. Only after independent original-Encode comparison and ownership binding,
   measure saved Encode CPU against added Replay CPU on fixed input and then
   the live supply critical path. This shadow removes no existing work.

This investigation changes documentation only after the measured commit; it
does not promote preparation, parallel execution, or any new runtime policy.
