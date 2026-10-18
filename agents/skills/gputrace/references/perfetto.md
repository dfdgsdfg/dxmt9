# Integrated Perfetto GPU timing

Read [Perfetto rules](../../../rules/trace_perfetto.rules.md). Use the
[shared setup, closure and qualification workflow](../../cputrace/references/perfetto.md)
for build/staging, fixture preparation, normalization and pinned Trace Processor
validation. Commands run from the repository root.

Choose `command-buffer` or `encoder-stage` according to the question and actual
Metal device capabilities. `gpu-detail` requires Metal timing and includes the
engine CPU categories needed for submission correlation. `timeline` with
`--trace-include-metal` requests optional Metal timing and must retain its explicit
outcome; it is not equivalent to a required `gpu-detail` collector.

After building the [self-closing fixture](../../../../tests/integration/tracing/README.md),
inspect and then execute the same request:

```sh
scripts/run_python.sh scripts/run_apps/run_experiment.py run conf-d3d9-triangle \
  --binary experiments/output/tracing-acceptance/wine-triangle-x64.exe \
  --trace-profile gpu-detail --trace-gpu-level encoder-stage \
  --trace-gpu-capacity 64 --trace-duration-ms 500 --timeout 60 --trace-dry-run
scripts/run_python.sh scripts/run_apps/run_experiment.py run conf-d3d9-triangle \
  --binary experiments/output/tracing-acceptance/wine-triangle-x64.exe \
  --trace-profile gpu-detail --trace-gpu-level encoder-stage \
  --trace-gpu-capacity 64 --trace-duration-ms 500 --timeout 60
```

Use `--trace-detail` only for bounded per-draw observations. `--trace-include-pe`
adds guest batches, but the recorded x86/x64 QPC cases do not qualify complete
guest clock attribution. Preserve raw events and the actual rejection reasons.

Verify engine receipt, GPU collector/retirement outcome, artifact hashes,
normalized clock/window/identity checks and pinned Trace Processor results.
An unsupported stage level must fail explicitly; a new command-buffer request
is a different attempt. Do not infer stage timing from CPU completion callbacks.

The 500 ms wrapper window and 1.5 s post-work dwell exercise capture closure.
They establish neither throughput nor observer overhead. This route does not
collect scheduler residency, shader occupancy or display presentation times.
See the [current evidence matrix](../../../../specs/experiments/harness/tracing/evidence.md)
for qualified devices, workloads and remaining limits.
