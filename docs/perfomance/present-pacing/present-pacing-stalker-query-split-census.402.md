---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "H402: Current query publication and repeated Pending census"
type: evidence
status: publication-owner-confirmed-cpu-cost-unpriced
updated: 2026-09-27
source: traces/stkcop-split-h402/{run.py,analyze.py,summary.json}; traces/stkcop-split-h402/{default,visibility}/invocation.json; experiments/output/app-d3d9-stkcop-bench-split-h402-{default,visibility}/result.json
related: present-pacing-stalker-visibility-gap.401.md; present-pacing-stalker-scope-fragmentation.331.md; present-pacing-query-scope-cb-composition.332.md
---

# H402: Scope replacement explains extra publication; polling still takes locks

Two sequential diagnostic runs on HEAD `dbc216d5`, same five staged images,
same requested game configuration as H399/H401. Sikarugir CX24.0.7, Day/high,
1280x720, two game threads and 16 sound targets. Enable frame/perf counters,
query split observer and backend observer in `counts` mode. No query timeline,
native sampling, GPU tracing, concurrent build or second benchmark. Only the
second run adds `DXMT9_METAL_OCCLUSION=1`. Both exit 0 with runner pass and
zero sampled GPU errors. The visibility capture contains the game; it is not
a same-camera or whole-sequence visual oracle.

This is a counts experiment, not a timing or FPS promotion experiment.
The split observer logs cumulative rows under a diagnostic mutex and backend
counts also aggregate under a diagnostic mutex. No claim of zero overhead.
Original engine FPS is 27.771202 / 27.546873; do not compare these to H399's
observer-OFF scores or call this a non-regression test.

## Exact whole-run publication accounting

| Population | Default | Real visibility |
|---|---:|---:|
| Present publications | 3,372 | 3,368 |
| QueryEnd publications | 7,842 | 3,337 |
| SemanticBoundary publications | 0 | 35,922 |
| Total publications | 11,214 | 42,627 |
| Scope-to-nonempty replacements publishing work | 0 | 2,837 |
| Scope-to-empty replacements publishing work | 0 | 33,085 |
| Empty END | 0 | 33,085 |
| Deferred-marker FLUSH publications | 0 | 0 |
| Completion-bearing tail CBs | 11,214 | 42,627 |

`2,837 + 33,085 = 35,922`: scope replacement extents account for the entire
SemanticBoundary population. `scope_* count` counts all successful replacements;
`extent` counts only populated writers. In the visibility run both replacement
counts are 33,085, but nonempty extent is only 2,837. These are membership
states, not universal BEGIN/END labels for nested queries.

The queried writer is often published by the scope-to-empty replacement;
the following END sees an empty writer. Delaying only `publishQueryEnd`
therefore does not remove these 33,085 source publications. FLUSH-publication
zero does not mean FLUSH calls or submission pressure are absent.

Frame snapshots contain 25,193 / 54,846 CB creations, 13,980 / 12,218
mid-source sub-CB commits and 101,669 / 102,777 render-pass begins. The source
tail increase, not a rise in mid-source sub-CBs, accompanies total CB growth.
Do not demand exact conservation between independent frame snapshots and
teardown ledgers: creation, submission and initializer populations differ and
the snapshot boundary differs by one Present in the default run.

Whole-run sampled-Present ratios (include loading, not matched camera frames):
CBs 7.47 / 16.28, passes 30.16 / 30.52, draws 2,891.21 / 2,621.38.
These are work-shape observations, not isolated causal per-frame savings.

Source binding: `src/dxmt9/dxmt9_command_queue.cpp` / `setOcclusionScope`
publishes the populated writer before replacing membership. The observed
source-tail population is one CB per publication in both runs. This repeats
H331's mechanism on current binaries; it is not a newly invented finding.

## Repeated Pending checks still use the slow locked path

| Backend census | Default | Real visibility |
|---|---:|---:|
| Calls / FLUSH calls | 1,875,302 | 2,595,578 |
| Pending | 1,867,461 | 2,559,181 |
| Complete | 7,841 | 36,397 |
| Invalid / failed | 0 / 0 | 0 / 0 |
| Already requested, not yet submitted | 893,484 | 1,371,599 |
| Already submitted | 973,515 | 1,186,188 |
| New submission-target requests | 462 | 1,394 |
| Covered-Pending fast path | 0 | 0 |

99.975% / 99.946% of Pending outcomes need no new submission-target request.
Those categories do **not** indicate redundant GPU submissions: wakes are
already deduplicated. `requestQuerySubmissionImpl` checks existing/submitted
targets under `pendingCompletionMutex_`; `pollQueryFenceImpl` holds the queue
mutex before reaching that check. Backend `counts` mode takes no phase clocks,
so this run establishes frequency, not contention duration or CPU milliseconds.

The current `src/dxmt9/dxmt9_query_policy.hpp` / `coveredPendingEnabled` is
default OFF. H399 clears inherited flags, so its baseline really uses that
default, not the opt-in fast path. H328 explicitly records only a per-run
enablement, not a production default promotion. The implemented fast path
uses immutable receipt identity, atomic completion/coverage/initializer
observations and terminal checks; possible success and unqualified cases
still take the locked path. A high redundant-check ratio does not prove every
such call qualifies for that fast path.

## Decision and remaining comparison gap

- Do not implement another END deferral under the assumption that it removes
  scope publication. The current extra sources are accounted for elsewhere.
- Do not claim CB joining closes the FPS gap. H332 already reduced CBs about
  46% without a demonstrated FPS gain. Its historical timings are not current
  evidence, but they prevent treating CB count alone as the objective.
- The smallest existing-policy cost experiment is a same-binary observer-OFF
  covered-Pending OFF/ON/reversed repeat, with a separate census proving its
  eligibility rate. Preserve receipt/terminal/initializer/FLUSH obligations.
- Real visibility still requires broader correctness qualification. Any
  source/CB optimization must preserve scope identity and attachment contents.
- D9VK same-camera draw/query census and cross-renderer CPU/GPU critical-path
  attribution are **not completed** here. The 1.825x gap is still not assigned
  to these mechanisms. D9VK's reference HUD samples draw deltas periodically
  and reports a maximum for submissions; screenshot values are not a directly
  interchangeable whole-run ledger.

No runtime code, default, README or policy promotion changes. Raw receipts and
counts are retained; `analyze.py` verifies publication and backend-category
conservation plus binary/config equality.
