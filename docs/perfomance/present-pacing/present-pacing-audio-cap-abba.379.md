---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2
title: "H379: Explicit 16-voice control lane improves the measured tail"
type: evidence
status: controlled-lane-evidence
updated: 2026-09-26
source: traces/app-d3d9-stkcop-audio-cap-h379/{run.py,receipt.jsonl,analysis.json,snapshot-2.txt}; traces/app-d3d9-stkcop-audio-abba-h379/{run.py,receipt.jsonl,analyze.py,analysis.json}; experiments/output/app-d3d9-stkcop-bench-audio-abba-h379-{1-a,2-b,3-b,4-a}/{result.json,daybenchmark.result,stkcop-controls.txt,stkcop-requested.ltx,stkcop-after-run.ltx}
related: present-pacing-audio-mix-coefficients.378.md
---

# H379: Audio source control is useful, but remains a quality-changing lane

The shipped high preset and base config both specify `snd_targets 32`. Add
the explicit harness override `DXMT_STKCOP_SND_TARGETS=16`, appended after
the preset. Unset preserves the preset. No audio DLL replacement, volume-zero
shortcut, dxmt9 runtime change, catalogue-default change or README FPS rewrite.
The supported override range is deliberately bounded to 4–32; only 16 is
runtime-qualified here.

All STALKER launcher paths now acquire an app-tree config lock before staging
and restore the original `test.ltx`/`user.ltx` bytes or absence on exit.
Requested/post-run configs and the override/worker arguments are artifacts.
An uncatchable kill can leave a recovery lock/backup; manual recovery is
documented in the experiment guide. Failed restores retain their backup.

## Actual allocation check, separate from FPS

A diagnostic run reuses H376's owner-qualified source-list reader with cap 16.
Four early and 28 late observations qualify. All have declared/traversed source
count 16; all 28 late observations have exactly 16 playing sources. Thus the
setting actually changes source allocation, not merely a config line. Both
80-snapshot windows have balanced resume/context checks. This intrusive run
is not used for performance comparison.

## Observer-OFF A–B–B–A

All runs: Day/high, 1280x720, `-max-threads 2`, perf profile, counters/frame
sampling/PE sampler OFF, Wine logging `-all`. A uses preset audio (32); B uses
cap 16. No builds, tests or diagnostic samplers overlap these four runs.
All PASS and stage identical binaries. FPS is the engine result's reported
average; frame-time summaries derive from its per-frame FPS records.

| Order | Voices | Average FPS | Whole-run p99 ms | Frames >250ms | Last-30s p95 ms | Last-30s frames >100ms |
|---|---:|---:|---:|---:|---:|---:|
| A1 | 32 | 28.026 | 143.25 | 8 | 145.87 | 75 |
| B1 | 16 | 29.828 | 87.08 | 1 | 82.02 | 13 |
| B2 | 16 | 29.661 | 87.84 | 0 | 81.07 | 17 |
| A2 | 32 | 27.324 | 154.47 | 8 | 135.62 | 46 |

Mean reported FPS: 27.675 → 29.745 (**+7.48%**). Both adjacent orderings favor
the cap, and tail distributions improve more visibly than the full average.
There are only two runs per policy and baseline drift remains; no confidence
interval or universal benefit is claimed. Last-30s boundaries reconstruct
completion time from frame durations, not exact matched scene identities.
Whole-run hitch counts have different frame denominators; they are not rates.

## Decision and limits

Keep this as an **explicit audio-controlled renderer experiment**, not a
transparent production optimization. Sound selection/quality changes are
expected, and human audio/temporal visual confirmation remains open. Harness
PASS does not certify those semantics. The original 32-voice lane remains the
real-workload regression/comparison baseline.

This intervention supports the causal relevance of audio load to the observed
tail. It does not assign all residual stalls to audio or show which renderer
stage becomes critical after the cap. Use the labelled 16-voice lane for a
subsequent app/replay/encode supply capture, while retaining original-audio
checks before promoting rendering changes.

Validation: launcher shell syntax, three focused test cases covering cap/default,
invalid input, locked app, app failure and restoration of original absence/bytes;
artifact audit and `git diff --check`. Native graphics/TLA tests are not needed
for this launcher-only control. Default audio and runtime semantics stay intact.
