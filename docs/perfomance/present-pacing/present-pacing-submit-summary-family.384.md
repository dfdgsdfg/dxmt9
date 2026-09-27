---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "H384: Legacy summary reconstruction, not safety validation, owns the submit prepass"
type: evidence
status: qualified-summary-family-attribution
updated: 2026-09-26
source: traces/app-d3d9-stkcop-submit-summary-h384/{run.py,summarize.py,receipt.jsonl,supply.csv,submit.json,waits.json,summary.json,supply-analysis.json}; experiments/output/app-d3d9-stkcop-bench-submit-summary-h384/{result.json,dxmt9.log}
related: present-pacing-submit-phase-owner.383.md
---

# H384: Separate safety validation from summary reconstruction

Add a validation-end marker immediately before `summarizeSubmissionSources`
and observe the actual summary branch without resolving the payload again.
Family markers name legacy, Arena or receipt-backed source members. Existing
phase numbers stay unchanged; old traces retain the combined phase and have
no invented fine-grained attribution. No scheduler, lock, lifetime or rendering
policy changes are included.

## Evidence

Day/high 1280x720, `-max-threads 2`, `snd_targets 16`, perf profile. All builds
and tests completed before the run; no stack sampler ran concurrently. The
catalogue reports PASS, no failures, zero GPU command-buffer errors, and records
the five staged binary hashes. Trace 2700–2720 closes without overflow or
incomplete writers. The trimmed 2702–2718 window contains 16 intervals,
1087.527ms and scene draws on every selected frame. This is not an observer-OFF
FPS run, a same-input A/B or full human temporal/visual certification.

| Interior-window phase | Wall total | Thread CPU total | Wall / Present |
|---|---:|---:|---:|
| Safety validation | 0.330ms | 0.332ms | 0.0206ms |
| Summary reconstruction | **4.163ms** | **4.167ms** | **0.2602ms** |
| Combined validation/summary | 4.493ms | 4.499ms | 0.2808ms |
| Driver commit | 0.764ms | 0.770ms | 0.0478ms |
| Pending-completion mutex acquisition | 0.026ms | 0.028ms | 0.0016ms |

All **38** complete interior submissions have complete family coverage, with
**38 legacy members, zero Arena and zero receipt-backed members**. Here
"legacy" is `SourcePayloadView::legacyPayload()` / the compatible ChunkSlot
payload representation, not evidence that the removed DrawRunSubmission carrier
was restored. No complete holder or submission endpoints are missing.

About **92.7%** of the previously combined phase belongs to summary rebuilding,
not safety checks. Clock quantization and independent CPU/wall reads explain
small CPU-over-wall differences. The costs include observer overhead.

## What is and is not established

`src/dxmt9/dxmt9_queue.cpp`, `summarizeCommands` allocates/reserves a temporary
`ChunkObservation` vector, obtains each command view and derives compatibility
flags/shader hashes, then traverses the observations again in `summarizeChunk`.
That is the path now demonstrated by runtime family evidence. The 4.163ms is
the **whole summary**, not a measured vector-allocation or memcpy cost.

The independent Arena candidate remains real in code:
`src/dxmt9/dxmt9_source_payload.cpp`, `SourcePayloadView::commandAt` builds a
complete view even when callers only inspect `kind()`, and chain lookup scans
segment offsets. But no Arena summary executed in this window, so that
candidate cannot explain the measured summary cost here.

Mark-ticket attempts total 24.942ms across 1,501 calls, with only 1.212ms
covered by other-thread held segments (0.888ms encode submit, 0.254ms finish
retire, smaller remaining sites). Driver commit overlaps only 0.104ms of these
attempts. Most acquisition time remains unattributed. No StateBlock saturation
wait occurs in the selected window. Replay is empty 68.46% of it.

Do not interpret H383→H384 wall/FPS differences as regression: these are
different diagnostic runs, and even the source counts differ (32 versus 38).
Removing all measured summary service would save at most its observed
0.2602ms/Present from this submit work, not guarantee an equivalent end-to-end
or FPS gain. Upstream starvation and unaccounted waits remain separate owners.

## Next implementation gate

1. Replace the temporary observation vector plus second fold with a shared
   pure incremental fold, retaining command order, compatibility-flag OR and
   last Draw-class shader/hash semantics. Clear/ColorFill are Draw-class in
   this diagnostic contract and must not accidentally preserve prior hashes.
2. Differentially compare the existing summary oracle and streaming result
   for mixed commands, empty sources, Present and last-writer cases. Existing
   `tests/native/backend/dod_replay_observer_spec.cpp` covers part of this
   contract; add explicit last-writer and callback-order coverage.
3. Re-measure summary service before considering an immutable summary cache.
   Such reuse must be source/generation-qualified and cannot skip validation
   or assume all diagnostic fields are expendable.

The low-risk goal is redundant materialization removal, not an assertion that
summary cleanup will resolve STALKER's late slowdown. The incomplete mutex
coverage and missing saturation reproduction remain open.

Validation: 34 Python tests (including fine split bounds, missing family
coverage and malformed markers), x86_64 replay-offload/supply-trace tests,
Unix install-name audit and TLA verification passed. No optimization is
promoted by this diagnostic result alone.
