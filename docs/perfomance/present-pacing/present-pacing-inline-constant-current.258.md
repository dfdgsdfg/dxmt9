---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Current semantic-owner inline-constant economics"
type: evidence
status: mechanism-verified-not-promoted
updated: 2026-09-08
source: experiments/output/pe-inline-constant-current-20260907/analysis; experiments/output/app-d3d9-3dmark05-pe-inline-current-5d764f37-{off,on}-n127-r1-20260908; experiments/output/app-d3d9-3dmark{05,06}-pe-inline-current-5d764f37-{off,on}-perf-r1-20260908
related: present-pacing-pe-constant-copy.257.md; present-pacing-inline-const-delta.201.md; ../../../specs/backend/spec.md
---

# Current semantic-owner inline-constant economics

## Decision

Keep `DXMT9_PE_INLINE_CONST_DELTA` default-off. The current compact owner
removes almost all standalone constant records, but one OFF/ON pair per
workload yields only +1.25%/+0.91% FPS and no demonstrated process CPU
reduction. This reopens [H214](present-pacing-inline-const-delta.201.md) against
the later semantic-owner architecture; it neither inherits the old ceiling
nor establishes a new promotion. No runtime source, binary or default changed.

## Identity and validation

Documentation HEAD is `f842867f`; runtime is the frozen `5d764f37` set from
[H257](present-pacing-pe-constant-copy.257.md). All six runs match its five
staged artifact receipts. PE32/PE64 hash prefixes are
`43b7da5b28e223b8`/`cb503ce41826308d`; Unix is `e4543457d6992f1a`.
Canonical build checks were no-ops. Runtime is Sikarugir cx-24.0.7;
05 uses 1024x768, 06 uses 1280x720, named GT2 through the catalogue runner.
Index reorder and segmented transport are explicitly OFF in both arms;
offload is ON. There is no GPU trace or PE thread sampler.

Six distinct native targets pass: producer differential, semantic owner,
binding reuse, chunk record value, chunk record validation and chunk record
replay. `runOwnerPlanLane` calls the real compact plan, owner admission,
`acceptSparseStatePlan` and ExactFixed emission. The all-category fixture
carries all six constant families; other rows cover ABA/reuse and stale
constant-range rejection. The closed constant rollback matrix covers
standalone owner transactions, not the Unix run planner. The sparse replay
fixture verifies VS-float section application before Draw, not a complete
six-family production draw-island equivalence proof.

The producer target was also executed in fresh OFF/ON environment processes,
but its fixtures pass the inline boolean explicitly: these host runs do not
exercise the PE environment selector. Real Wine record counts below establish
that runtime distinction. Native logs are retained under the analysis root.
No new algorithm was implemented, so this experiment adds no new TLA proof,
Wine fault matrix or GPU differential oracle. Those layers cannot be inferred
from zero errors or a single rendered capture and remain promotion work.

## 05 diagnostic mechanism pair

Both diagnostic arms use Info logging, `DXMT9_PE_STATS_DECIMATION=127`,
semantic-owner phase split ON, recorder full stats OFF and frame sampling ON.
The parser validates all 29/30 cumulative groups; the final groups cover
1,740/1,800 Presents versus 1,779/1,817 encoded Presents. The final unreported
39/17 Presents are not in PE normalization. Do not sum cumulative reports.
All reported owner admissions succeed; all retained renderer error counters
are zero. Different frame/tail populations are normalized, not identical.

| Quantity per observed Present | OFF | ON |
|---|---:|---:|
| Standalone VS + PS constant records | 1,003.205 | 0.903 |
| Owner append attempts | 2,711.952 | 1,705.064 |
| Corrected inclusive owner estimate, ms | 8.794 | 6.344 |
| Corrected prepare estimate, ms | 0.557 | 0.668 |
| Corrected canonical materialization estimate, ms | 0.976 | 1.109 |
| Corrected PendingDelta settlement estimate, ms | 0.144 | 0.217 |

Complete-run wire records decrease 2,697.435 to 1,700.929 per encoded Present,
but bytes decrease only 1,267,527 to 1,252,139: constants still cross the
boundary. CB/pass ratios remain about 4/15.77. Folding moves preparation,
materialization and settlement work into the Draw transaction; it does not
remove setter comparison/copy or Unix constant application.

Owner null-pair calibration is 278.11/247.23 ns. Estimates subtract the run's
null pair from the sampled per-event mean and multiply by events/Present.
They are **not product CPU savings**: child clocks and reconstructed identity
metrics remain inside parents, and the observed owner uses
`appendPreparedRecord<true>` instead of the unobserved constant specialization
`appendPreparedRecord<false, true>`. The apparent 2.45 ms parent reduction
must not become a CPU or FPS budget. Short scopes at/below calibration are
unresolved, not zero-cost. Parents/children and family aggregates are not
additive. `05-phase-comparison.json` retains raw counts and calibration.

## Detailed-observer-OFF spot pairs

Perf/Warn, frame sampling ON, PE decimation/full stats/phase split/module map/
thread sampler/copy ledger OFF. Each product runs OFF then ON once, without
order counterbalancing or a controlled cooldown. A PID-bound read-only process
CPU/RSS observer polls every two seconds. No build, test or separate benchmark
runs alongside the scene; host isolation is not claimed.

| Product | OFF FPS | ON FPS | Delta | OFF / ON rows | OFF / ON wall ms |
|---|---:|---:|---:|---:|---:|
| 05 GT2 | 29.631 | 30.000 | +1.25% | 1,914 / 1,938 | 64,594.152 / 64,599.266 |
| 06 GT2 | 18.188 | 18.354 | +0.91% | 1,165 / 1,175 | 64,051.748 / 64,019.745 |

05 includes all positive-wall frames starting at 1. 06 includes positive-wall
frames with more than 100 draws: OFF 1,020–2,184, ON 998–2,172. Loading is
excluded, but slow scene frames remain. These are frame-log scene averages,
not process-elapsed FPS or decoded benchmark scores. All results pass with
zero chunk/GPU/pipeline/no-pipeline failure counters; 06 permits the completed
scene's result-GUI timeout. ON captures at 05 1:00.73 and 06 0:35.66 show
rendered forest frames, not whole-scene pixel equivalence.

| Product | OFF / ON CB per frame | OFF / ON passes per frame | OFF / ON partial CPU ms per frame | OFF / ON CPU frame increments |
|---|---:|---:|---:|---:|
| 05 GT2 | 4.001 / 4.001 | 15.788 / 15.791 | 70.264 / 70.424 | 1,893 / 1,910 |
| 06 GT2 | 4.002 / 4.002 | 19.866 / 19.860 | 95.694 / 95.161 | 1,145 / 1,120 |

CPU joins cover different partial intervals and every process thread, not PE
exclusive time. The +0.23%/-0.56% differences do not establish reduction.
Sampled RSS peaks are 3,312,764/3,126,824 KiB for 05 and
3,521,944/3,528,488 KiB for 06. Summed GPU CB times are
1.982/2.008 and 11.874/11.879 ms/frame, not GPU critical-path latency.
`perf-comparison.json` retains exact arithmetic, populations and identity
checks. README remains unchanged; its index-reorder-ON historical profile is
not a matched reference for these index-reorder-OFF measurements.

## Next boundary

This rules out treating the old uncalibrated recorder estimate or record-count
reduction as a large established PE win. It does not rule out a smaller gain
or another workload. Repeating long runs alone is lower priority than proving
and removing actual duplicate source/settlement work identified in H257.
Any eventual inline-mode promotion still needs current end-to-end effective
state/constant equivalence, fault and pixel evidence, and repeatable cooled
observer-OFF CPU/FPS gains. Do not change backend options to claim PE recovery.
