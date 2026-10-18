---
name: gputrace
description: Investigate dxmt9 Metal GPU timing, command-buffer and encoder activity, frame correctness, resources and shaders using integrated Perfetto GPU timing, Instruments Metal System Trace, or Xcode .gputrace capture. Use for GPU profiling, frame capture, Metal validation and shader debugging.
---

# GPU tracing and frame debugging

Choose the question and artifact before launching anything. The default Perfetto
overview (`--trace`) includes optional command-buffer timing; use `gpu-detail`
when GPU timing is required. Keep external capture links and provenance in the
shared companion SQLite store. A supplied capture
can be analyzed without rerunning its workload. Run commands from the repository
root and Python through `scripts/run_python.sh`.

## Choose the observation

| Question | Route | Read before using |
|---|---|---|
| How does engine submission relate to GPU command buffers or stages? | Perfetto `gpu-detail` | [Perfetto rules](../../rules/trace_perfetto.rules.md), [GPU recipe](references/perfetto.md) |
| What GPU/driver activity overlaps the workload? | Metal System Trace `.trace` | [xctrace rules](../../rules/trace_xctrace.rules.md), [Instruments rules](../../rules/trace_instruments.rules.md), [Metal timeline recipe](references/instruments.md) |
| Which draw, resource or pipeline caused a rendering defect? | Xcode Metal Debugger `.gputrace` | [Metal rules](../../rules/debug_xcode_metal.rules.md), [frame capture](references/frame-capture.md) |
| Which encoder dominates replay counters? | Xcode counter export and matched replay analysis | [Xcode exports](references/xcode-counters.md), [counter semantics and variance](references/gpu-counters.md) |
| Is this an API or shader error? | Scoped validation and shader inspection | [Metal rules](../../rules/debug_xcode_metal.rules.md), [validation recipe](references/shader-validation.md) |

Load the references for the selected route. `.trace`, `.gputrace`, and `.pftrace`
are different artifacts; do not substitute one for another or promise that an
arbitrary trace exposes draw stepping, shader hotspots or buffer contents.

## Run and qualify

1. Freeze the workload/frame/window, runtime/build identities and requested
   timing level. Follow [wild-run rules](../../rules/test_wild.rules.md) for Wine.
   For 3DMark05, read [probe supervision](references/gt1-supervision.md) and use
   [the GT1 wrapper workflow](references/gt1-probes.md), including its dry run.
2. Preflight the selected device, collector, capture layer and output capacity.
   Unsupported stage sampling or failed capture admission is a failure for
   that request; keep its reason instead of silently collecting another level.
3. Capture a bounded run into a unique directory. Manual artifacts belong in
   `traces/<app-runid>/`; use harness-owned paths for integrated Perfetto runs.
   Verify capture start/stop and receipt/export completion before analysis.
4. Retain source timestamps, level, process/device identity and typed submission
   relations. For Xcode counters, verify the actual export path, completed
   profiling and same-run encoder join. Keep GPU execution, CPU callbacks,
   replay cost and display presentation separate.
5. Report raw/derived artifacts, coverage, failure outcomes and the supported
   conclusion. Small replay deltas need variance evidence. Performance claims
   also need matched observer controls and rendering correctness.

For CPU/GPU questions, coordinate with [cputrace](../cputrace/SKILL.md) using the
same run plan. Reuse the CPU tracks already in a `gpu-detail` capture; separate
Instruments and Perfetto timestamps require a proven mapping before joining.
Preserve traces and exported evidence after reporting.

## Local inspection helpers

- [scripts/parse_gputrace.py](scripts/parse_gputrace.py) inspects available bundle
  metadata and raw resource files. Its binary-layout/label heuristics do not
  establish draw bindings or complete resource coverage. See [frame capture](references/frame-capture.md).
- [scripts/capture_frame.swift](scripts/capture_frame.swift) is a standalone
  native Metal capture example, not the Wine capture path or a required step.
- Use [shared Perfetto normalization/consumer guidance](../cputrace/references/perfetto.md)
  for native `.pftrace` validation; do not duplicate the common qualification path.
