---
domain: present-pacing
workload: STALKER CoP Day
title: "Query Ready frontier and retrospective END-qualified replay drain"
type: evidence
status: measured
updated: 2026-09-18
source: experiments/output/query-phase-r645-r646.noindex/analysis-frontier-r647.json; experiments/output/query-phase-r645-r646.noindex/occlusion-frontier-r647-day.guard.json; experiments/output/app-d3d9-stkcop-bench-occlusion-frontier-r647-day/result.json; experiments/output/app-d3d9-stkcop-bench-occlusion-frontier-r647-day/scene-restored.png
related: present-pacing-query-queue-drain-attribution.321.md; overview.md
---

# Later replay work is a concrete drain-scope candidate, not a proven saving

After commit `ebd6e331`, cold diagnostics distinguish Ready residence from
the immediately preceding source's encode interval and associate core query
END with its raw replay ordinal. **No drain, scheduling, completion or HRESULT
policy changes.** The current device-wide CPU drain remains the production
boundary before core polling.

R647 finds 1,773 GetData calls whose copied END identity stays unchanged
across the drain and whose entry FIFO contains later raw work. Their drain
intervals total **5.751 s**. This establishes a concrete scope mismatch to
investigate; it does not establish that those whole intervals are removable.
Most calls are inexpensive empty-queue checks, not the optimization target.

## Qualification and limits

The run uses Sikarugir cx24.0.7, Day High, `renderer_r2.5`, 1280×720 windowed,
perf profile and Info logging. Actual occlusion, asynchronous fences, query
observer and drain-site observer are ON; END coalescing, StateBlock, query-split,
frame sampling and Metal validation are OFF. Global health counters remain ON.
All five staged images match the canonical outputs by size and SHA-256.

The app exits normally without timeout, records 2,140 benchmark rows and
2,229 encoded Presents, and has all eight required health counters present
and zero. No session-scheduled build, test, profiler or subagent work overlaps
the runtime; 49 five-second guard samples, 47 with the engine launch active,
find no competing build/test/TLC/profiler process. OS/user activity and short
unsampled work are not excluded.

The runner reports `capture_error`: its title-based on-screen capture fails.
Computer Use initially also sees no on-screen window; one explicit restore
then obtains `scene-restored.png`, showing the cafe, vegetation and ground.
This is one scene spot, not whole-sequence or frame-aligned equality. Do not
use the runner's fallback `actual.png` as successful app capture evidence.
The recorded **16.405720 FPS is diagnostic only**: the run has added observers
and a mid-run window restore, and is not a performance comparison with H321.
README, provider defaults and promotion gates are unchanged.

## Ready residence: several earlier sources, not just sequence N-1

At ordinary publication, the observer snapshots prior Ready depth/head,
unique Encoding sequence, and submitted/completed frontiers under the
existing ownership locks. A separate 128-entry bounded ledger records ordinary
encode intervals. Dequeue joins only the exact same-queue `sequence - 1`
interval, with a real intersection against publication-to-dequeue residence.
No absent multi-source/Arena stage or evicted interval is invented.

After skipping 5,000 ENDs, the retained window contains 126 OCCLUSION and two
EVENT summaries. All have the six stage timestamps; all 126 retained occlusion
results are zero. This is a different selected population from H321's 56
mixed-result occlusion rows, not a paired scene or proof that the whole game
has zero visible samples. The ledger drops 18,730 summaries, evicts 28,199
stage-evidence and 28,199 encode entries, and has 166,548 polls outside the
retained set.

| Selected occlusion evidence | Result |
|---|---:|
| Prior Ready depth, median / p95 / maximum | 9 / 27 / 29 sources |
| Publication → dequeue, median / p95 | 15.043 / 18.027 ms |
| Dequeue → post-encode relock, median | 0.148 ms |
| Post-encode relock → submit observation, median | 0.022 ms |
| Known Encoding identity at publication | 122 / 126 rows |
| Encoding at publication is immediate predecessor | 6 / 126 rows |
| Exact predecessor overlaps Ready residence | 122 / 126 rows |
| Predecessor intersection, median across matched rows | 0.147 ms |
| Matched predecessor overlap sum / selected residence sum | 53.783 / 1,304.438 ms = 4.12% |

The queue already contains multiple earlier sources in this window. The
immediate predecessor alone explains little of the selected residence, so
optimizing only that small queried source or its immediate predecessor is not
an adequate latency model. Full earlier-source-chain attribution is still
open. These are overlapping elapsed intervals, not unique wall time, thread
CPU or proof of a GPU wait. In particular, a submitted/completed frontier gap
does not establish why the encode thread is delayed.

## GetData drain: stable identity versus retrospective association

On the replay owner thread, the core observer copies query identity, issue,
epoch, backend queue/sequence and current raw ordinal. GetData looks up only
this copied ledger by immutable object identity. It never reads mutable query
issue/fence state before draining. Entry/exit FIFO snapshots preserve unknown
placeholder endpoints and in-flight identity as explicit gaps.

The entry/exit END and FIFO snapshots are individually synchronized, not one
atomic system snapshot. An END first seen after the drain or changed during
it remains retrospective evidence, not authority for the requested issue.
A tail seen only after draining could be newly admitted work and has its own
class. Detail storage reserves 16 busy and 16 generic rows; aggregates include
all calls. The END map has zero evictions and 21,599 invalidations in R647.

All **166,676** calls match across PE bridge, Unix split, core and drain
observer, with FLUSH set, zero failed drains and no invalid clocks.

| Inner drain population | Calls | Cumulative elapsed |
|---|---:|---:|
| All calls | 166,676 | 10,791.520 ms |
| Entry queue busy (queued and/or in flight) | 3,516 | 10,769.354 ms |
| Entry queue empty | 163,160 | 22.166 ms |

Busy-entry calls contain 99.79% of this measured interval population. The
entry snapshot can race progress before the real predicate; "busy" does not
mean every one of those calls blocks. Likewise an empty entry is not a proof
against newly admitted work. These are not blocked-only CPU timers.

The disjoint END classifications conserve both call counts and durations:

| END classification | Calls | Cumulative drain elapsed |
|---|---:|---:|
| Same END, queued tail beyond END present before drain | 1,773 | 5,751.275 ms |
| END changed between snapshots | 212 | 4,744.150 ms |
| END absent before, known after | 3 | 81.949 ms |
| Same END, no sampled queued tail | 164,687 | 214.145 ms |
| No END known after | 1 | 0.000166 ms |
| After-only tail / no raw ordinal / failed drain | 0 | 0 ms |

The stable-tail class is 53.29% of the inner drain total. Across all
retrospective post-drain mappings, 1,988 entry-tail cases total 10,577.374 ms;
do not present that larger figure as stable-current-issue authority. In-flight
identity is unknown in 2,853 calls, and records after END within the same raw
are not separated. Neither table determines the isolated later-work cost.

A concrete retained detail pins the scope issue: query 16, issue 2, epoch 1
has END raw ordinal 70,945 before and after the call. At entry, 42 queued items
span ordinals 70,983–71,024, all later than END; an additional item is in
flight with unknown identity. The unchanged global drain takes 3.193 ms and
returns with the sampled FIFO empty. This proves later work was present, not
that its entire elapsed interval can safely be skipped.

The enclosing Unix split records 10,886.201 ms in its drain scope and
432.377 ms in its provider scope; the outer PE bridge is 11,528.403 ms.
Core polling is a nested 332.049 ms. The new inner drain timer excludes its
own END lookups, FIFO snapshots and reporting, so the enclosing drain has
an additional 94.681 ms of diagnostic/wrapper work. Do not add nested scopes
or call these measurements observer-free product CPU costs.

## Verification and next implementation boundary

Native focused tests pass 12/12; x86_64 tests pass 6/6. Native, Unix and both
PE builds pass; install-name and bridge classification audits pass. The full
unchanged 214-configuration TLA gate passes before the runtime. Deterministic
tests cover late END joins, exact queue/predecessor identity, bounded eviction,
END invalidation/reissue, before/after-only tails, duration conservation and
reserved busy details. Tests against the actual replay queue cover placeholder
endpoints, zero ordinals, and in-flight work with/without queued entries.
No new scheduler transition is implemented or claimed formally proven.

Prioritize an **identity-qualified immutable polling receipt**, not removal
of `waitDrained()` in isolation:

1. Associate END at admission with successful FIFO replay completion, qualified
   by query/issue/reset epoch. Missing, failed or ambiguous receipts retain a
   conservative path; a post-replay diagnostic map is insufficient.
2. Separate the polling receipt from mutable Device/Query ownership. Current
   `Device::getQueryData` reads device/query state and may resolve Query;
   the COM adapter adds no independent lock. Reaching an END raw ordinal alone
   cannot authorize that call concurrently with later replay.
3. Specify and test Issue/GetData/reissue/reset/failure/lifetime ordering,
   then compare exact query results and readbacks against the full-drain
   reference. Same-raw suffix handling remains distinct from later-raw scope.

Only after this boundary is safe should a matched observer-OFF experiment
price the change. Separately extend Ready attribution across the older-source
chain if needed. Neither the 5.751 s class total nor the 4.12% predecessor
coverage is an end-to-end FPS improvement bound.
