# Native CPU and thread recording

Read [xctrace rules](../../../rules/trace_xctrace.rules.md) and
[Instruments rules](../../../rules/trace_instruments.rules.md). Commands run from the
repository root. Replace the run ID and target PID with a verified workload
child; retain its command line, start identity and loaded runtime images.

## Preflight

```sh
xcode-select -p
xcrun xctrace version
xcrun xctrace list templates
xcrun xctrace list instruments
xcrun xctrace help record
```

Select Time Profiler for sampled hotspots. For state transitions, choose an
available Thread State/System Trace configuration and verify the required
tables after recording. Do not label Time Profiler sample weights as scheduler
residency. For exact function-flow questions, use the separate
[Processor Trace recipe](processor-trace.md).

## Bounded capture

Keep Wine launch under the catalogue runner and workload supervisor. Attach to
the actual application child when practical; a launcher-only recording can miss
the work. For a native executable, `--launch -- /absolute/path/to/app` is another
mode, but does not replace Wine runtime selection. The following example assumes
the target remains alive through the selected window:

```sh
cpu_trace_dir="$PWD/traces/<app-runid>"
cpu_trace_pid=<verified-pid>
mkdir -p "$cpu_trace_dir/analysis"
xcrun xctrace record --template 'Time Profiler' \
  --attach "$cpu_trace_pid" --time-limit 10s --no-prompt \
  --output "$cpu_trace_dir/cpu.trace"
```

Check recording exit status and save completion. Preserve stderr and provenance
with the attempt. For a short app, select an earlier/shorter window; any dwell
added to exercise closure is not throughput evidence.

## Export and summarize

```sh
xcrun xctrace export --input "$cpu_trace_dir/cpu.trace" --toc \
  --output "$cpu_trace_dir/analysis/cpu-toc.xml"
```

Read the TOC before this example: use its actual run number and schema. If the
`time-profile` table is absent, retain the unsupported/empty outcome instead of
running the CPU summarizer on another table.

```sh
xcrun xctrace export --input "$cpu_trace_dir/cpu.trace" \
  --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]' \
  --output "$cpu_trace_dir/analysis/time-profile.xml"
scripts/run_python.sh scripts/tools/summarize_xctrace_cpu_threads.py \
  --time-profile "$cpu_trace_dir/analysis/time-profile.xml" \
  --process-regex '<verified-process-regex>' \
  --trace "$cpu_trace_dir/cpu.trace" --run-label '<app-runid>' \
  --output-csv "$cpu_trace_dir/analysis/cpu-threads.csv" \
  --output-md "$cpu_trace_dir/analysis/cpu-threads.md"
```

When available, export `time-sample` and `thread-info` from the TOC and supply
`--time-sample` / `--thread-info`. Use
`--producer-thread-regex-from-pe-log <same-run-log>` when the verified native
commit-entry marker is available. Otherwise verify any selected producer:
the tool's highest-sampled-weight default is a heuristic, not thread ownership.

For unfamiliar XML, inspect bounded rows with:

```sh
scripts/run_python.sh agents/skills/cputrace/scripts/parse_trace.py \
  "$cpu_trace_dir/analysis/time-profile.xml" --summary
```

The generic helper is not a source/coverage qualifier. Retain unknown symbols
and separate native Wine/unix cost from opaque guest execution. See
[Wine/PE analysis](wine-pe.md). For call-tree/flamegraph analysis in Instruments,
record the time range, filters, self/inclusive choice and charge/prune/flatten
settings. Compare the same observation kind and workload phase.
