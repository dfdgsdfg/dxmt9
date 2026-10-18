---
name: cputrace
description: Investigate dxmt9 CPU hotspots, function flow, thread activity and waits using Instruments Time Profiler or Processor Trace, the integrated Perfetto timeline, and qualified Wine PE observations. Use for CPU profiling, CPU-side trace analysis and CPU-to-GPU submission questions.
---

# CPU tracing

Start with the workload, question, target process/thread, and measurement window.
Analyze existing artifacts first when supplied. Run commands from the repository
root, and Python through `scripts/run_python.sh`.

## Choose the observation

| Question | Route | Read before using |
|---|---|---|
| Where is native CPU work concentrated? | Time Profiler samples | [Instruments rules](../../rules/instruments.rules.md), [record/export recipe](references/instruments.md) |
| Which short functions ran, in what order, with what instruction/cycle cost? | Processor Trace on supported devices | [Instruments rules](../../rules/instruments.rules.md), [Processor Trace recipe](references/processor-trace.md) |
| How do import, replay, queue, encode and waits relate? | Integrated Perfetto timeline | [Perfetto rules](../../rules/perfetto.rules.md), [capture and qualification](references/perfetto.md) |
| Is a thread running, blocked or descheduled? | Available Thread State/System Trace evidence | [Instruments rules](../../rules/instruments.rules.md), [record/export recipe](references/instruments.md) |
| Which guest module or code site is involved? | Qualified PE samples plus static REA analysis | [Wine/PE recipe](references/wine-pe.md), [REA rules](../../rules/rea.rules.md) |

For xctrace collection/export, also read [xctrace rules](../../rules/xctrace.rules.md).
Load only the selected route's references. A missing capability is a recorded
limitation; switching collectors changes the evidence and requires a distinct
attempt. Processor Trace is not a universal fallback for opaque PE stacks.

## Run and qualify

1. Freeze the workload/build/runtime identity, collector, target, window and
   observer settings. For Wine, follow [wild-run rules](../../rules/test_wild.rules.md)
   and the workload's rules; retain the catalogue launch and supervisor.
2. Check the selected collector's hardware, toolchain, permission and schema
   support. Use a bounded capture with a unique output directory. Keep manual
   `.trace` artifacts in `traces/<app-runid>/`; retain harness-owned output paths.
3. Confirm the actual child process and window were observed, recording/export
   completed, and expected rows exist. Record missing symbols, loss and coverage.
   Use the route's parser and qualification checks before interpreting results.
4. Separate sampled weight, native CPU time, wall time, elapsed waits and traced
   instructions/cycles. Keep inclusive and self cost distinct. Compare matched
   workload/window/observer configurations; unknown work is not zero cost.
5. Report the question, run and source identities, commands, raw/derived paths,
   observation kind, clock/coverage limits and supported conclusion. Preserve
   raw evidence; cleanup stops owned collectors and temporary activity, not
   automatic trace deletion.

For CPU-to-GPU analysis, coordinate with [gputrace](../gputrace/SKILL.md) using
one capture plan and existing artifacts. A `gpu-detail` Perfetto capture already
includes engine CPU categories. Do not launch a second workload merely because
both skills apply. Cross-collector joins still need proven identities and clocks.

## Existing helpers

- `scripts/tools/summarize_xctrace_cpu_threads.py`: process/thread sample summaries.
- `scripts/tools/symbolicate_xctrace_pe.py`: module-map or PE sampler evidence;
  see the Wine/PE limitations before interpreting it.
- [scripts/parse_trace.py](scripts/parse_trace.py): generic exported-XML inspection,
  not a Processor Trace decoder or an attribution qualifier.
- [Shared Perfetto normalization and consumer](references/perfetto.md): legacy
  CPU/wait/PE inputs and native `.pftrace` validation, also used by `gputrace`.
