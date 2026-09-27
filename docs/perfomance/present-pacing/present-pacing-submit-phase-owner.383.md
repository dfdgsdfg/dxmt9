---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "H383: Submit validation/summary exceeds driver commit; most acquisition time remains unresolved"
type: evidence
status: partial-submit-cost-attribution
updated: 2026-09-26
source: traces/app-d3d9-stkcop-submit-phases-h383/{run.py,summarize.py,receipt.jsonl,supply.csv,submit.json,waits.json,summary.json,supply-analysis.json}; experiments/output/app-d3d9-stkcop-bench-submit-phases-h383/{result.json,dxmt9.log}
related: present-pacing-app-wait-holders.382.md
---

# H383: Investigate repeated summary work before moving driver commit

H381/H382 and their instrumentation were committed as `dfa24aa9`. This
follow-up appends source-qualified submit phase markers and adds actual-mutex
holder events to existing uninterrupted CommandQueue RAII probes. Sites marked
`skipHold` remain excluded rather than wrapping CV/unlock spans as holds.
No execution, ordering, capacity, completion or resource lifetime policy changed.

## Qualified run

Same explicit Day/high 1280x720, worker limit two, voice cap 16 and perf
profile. No builds, tests or stack samplers ran concurrently. The catalogue
run passes with no failures and zero GPU command-buffer errors. All five
staged artifact hashes are retained in `result.json`; this is not a same-binary
A/B or an observer-OFF FPS result. No full human temporal/visual certification
is claimed.

Trace Presents 2700–2720 close without overflow/incomplete writers. Trim two
on each side: 16 intervals, 745.112ms, scene draws in every selected frame.
All measured holder and submit phase sets have complete endpoints in the full
trace. The interior contains 32 complete submit sources.

## Submit partition

| Phase | Wall total (ms) | Thread CPU total (ms) | Wall / Present (ms) |
|---|---:|---:|---:|
| Validation and summary | 4.297 | 4.302 | 0.269 |
| Lifecycle and preparation | 0.130 | 0.123 | 0.008 |
| Capture/signpost setup | 0.061 | 0.067 | 0.004 |
| Driver commit call | 0.667 | 0.665 | 0.042 |
| Post-commit/watchdog | 0.078 | 0.072 | 0.005 |
| Pending-completion mutex acquisition | 0.035 | 0.034 | 0.002 |
| Pending handoff/unlock | 0.124 | 0.127 | 0.008 |

Independent clock sampling/quantization allows small CPU-over-wall differences;
these are not negative waiting times. Markers are observer-inclusive. The
partition excludes caller setup before `submit`, final notification after the
last marker, null/test submissions, early failures and unbound paths.

Validation/summary is about 80% of this measured submit body. The driver call
is not its largest owner, and pending-completion acquisition is small in this
window. These small absolute costs do not explain the whole app-side deficit.

## Mark-ticket wait coverage

1,330 complete attempts total 17.095ms (1.068ms/Present). Observed same-mutex,
other-thread holder intersection is only 0.775ms: encode submit 0.655ms,
finish retire 0.087ms, encode dequeue 0.032ms and make-encode-context 0.001ms.
Only 0.072ms overlaps the driver-commit subphase inside an observed submit
hold. The remaining 16.320ms acquisition time is **unattributed**. New RAII
coverage therefore does not close the gap. Do not assign it automatically to
the scheduler, or infer a lock-removal benefit from the longest sample.

No StateBlock queue-space wait recurs in this selected window. Replay is empty
57.61% of it. App/replay/encode CPU envelopes are 38.689 / 20.834 / 23.431ms per
Present, with different coverage and nested scopes; do not sum them. H382 and
H383 are different diagnostic runs, not evidence of performance regression or
improvement.

## Code candidates and next gate

`src/dxmt9/dxmt9_queue.cpp`, `submit` combines identity/span checks,
`commandRangeHasPresent` range traversal and `summarizeSubmissionSources` in
the first measured phase. Those costs are **not yet separated**.

For locator-backed sources, `summarizeSubmissionSources` calls
`summarizeSubmission` again. Arena payloads traverse command kinds; legacy
payloads call `summarizeCommands`, allocate a temporary `ChunkObservation`
vector, build observations, then fold it into diagnostics. This is a concrete
repeat-traversal/materialization candidate, not proof that the legacy branch
was responsible for the measured 4.297ms.

Next distinguish safety validation from summary traversal and qualify the
payload family. Prefer reusing immutable, generation-qualified metadata or
folding without temporary materialization if equivalence is demonstrated;
do not remove identity checks or treat all diagnostics fields as optional.
Separately cover uninterrupted portions of the remaining `skipHold` sites,
and capture an actual StateBlock saturation burst. Moving command-buffer
commit outside the queue mutex is not justified by this window alone.

Validation: 34 Python tests, x86_64 replay-offload and supply-trace tests,
Unix install-name audit and TLA verification passed. Analyzer tests reject
duplicate/reversed/missing-interior phases and exclude incomplete sources.
