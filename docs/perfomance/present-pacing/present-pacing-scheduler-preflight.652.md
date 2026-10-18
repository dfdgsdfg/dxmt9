---
domain: present-pacing
workload: ARM64 and Rosetta native busy/idle controls; no game launch
title: "H652 — scheduler intervals are available, but Running alone undercounts CPU"
type: evidence
status: native-tool-preflight-passed-live-clock-and-coverage-open
updated: 2026-10-17
source: traces/stkcop-scheduler-preflight-h652/{probe.cpp,run.py,analyze.py,analysis.json,validate.py,validation.json}; traces/stkcop-scheduler-preflight-h652/{arm64,x86_64}/{receipt.json,probe,record.log,target.jsonl,toc.xml,thread-state.xml,thread-info.xml,context-switch.xml,ThreadTermination.xml,scheduler.trace}
related: present-pacing-owner-coverage.651.md; present-pacing-retirement-live.650.md; present-pacing-cpu-counter-freshness.512.md; present-pacing-snapshot-performance.641.md
---

# H652 — a viable independent native-thread coverage instrument

Xcode/xctrace27.0 on Apple M1 successfully launches and records both ARM64 and
Rosetta controls with the **System Trace** template. No sudo, game/DLL patch,
runtime staging or renderer-policy change is needed. Full Xcode, Metal compiler
32023.921 and XML tooling pass preflight. Raw traces and exported evidence are
retained; nothing is deleted.

Unlike H450's Metal System Trace sample population, these traces expose actual
`thread-state` and `context-switch` interval tables. A trace-schema presence
alone is not success: both tables were exported, XML references resolved and
the target native PID/TIDs matched to actual self-CPU endpoints. Each process
starts a two-phase busy worker and a sleeping worker, joins both, then reports
their buffered endpoints. This avoids per-iteration text logging.

## A real undercount counterexample

| Architecture / worker | Body self CPU | Entire-thread Running | Running + Interrupted |
|---|---:|---:|---:|
| ARM64 busy |300.066000ms|299.712955ms|300.200127ms|
| ARM64 idle |0.076000ms|0.171626ms|0.171626ms|
| Rosetta busy |300.205000ms|299.201552ms|300.378167ms|
| Rosetta idle |0.099000ms|0.199375ms|0.199375ms|

Running alone is below the busy body's self CPU on **both** architectures,
even though scheduler totals include additional thread-entry/exit code. It must
not be used as a conservative CPU upper. Including Interrupted encloses both
actual busy measurements in this fixture. Differences between body and whole
thread are not an observer-overhead measurement or a calibrated correction.

The reducer charges Running, Interrupted and Unknown as possible CPU, clips
them to a requested interval, and rejects uncovered targets or interval gaps/
overlaps. It never converts Unknown to zero.24,010 finite state/CPU-placement
cases validate this arithmetic under its supplied state contract. Eight corrupt
XML variants are rejected. These tests do **not** prove the installed
XNU/Instruments state's universal accounting semantics.

All busy/idle histories begin with Unknown before their first observed state.
The native comparison uses the first non-Unknown transition through Terminated;
it does not pretend the preceding Unknown interval was measured idle. The
separate `ThreadTermination` table exports zero rows in both traces despite
their terminated worker histories. Its schema or empty result must not be
treated as a complete thread-retirement census.

## Next qualification and unchanged conclusions

Before a new original-game diagnostic, bind trace-relative timestamps to the
same camera/request clock, validate attachment to a running Rosetta process,
and establish target-wide thread/event coverage with conservative handling of
unknown or lost intervals. Raw self endpoints provide a cross-check; remote
thread-counter freshness and process-minus-known residuals remain rejected.

Trace-on costs cannot replace H641's audit-off removal experiment or H650's
untraced owner population. This preflight opens a concrete route to bound
native platform threads missing from project-owned lifecycle instrumentation;
it does not establish their live cost, global ranking, a new removable amount,
FPS gain or goal completion.
