# Metal System Trace timeline

Read [xctrace rules](../../../rules/trace_xctrace.rules.md) and
[Instruments rules](../../../rules/trace_instruments.rules.md). Use this route for
available GPU/driver intervals and CPU submission context, not frame replay.

For 3DMark05 use the guarded sidecar from [GT1 probes](gt1-probes.md). It retains
Wine launch supervision, checks lock/disk state, exports Metal intervals and
validates the same-run dxmt encoder join. Read [supervision](gt1-supervision.md)
before launching it. System-wide capture includes other applications; retain
that context and identify the target's rows explicitly.

For another verified target, discover the template first:

```sh
xcrun xctrace version
xcrun xctrace list templates
xcrun xctrace list instruments
```

The following bounded attach example assumes Metal System Trace is available
and the PID is the actual application child. Commands run from the repository
root; replace placeholders with the run's verified values:

```sh
gpu_trace_dir="$PWD/traces/<app-runid>"
gpu_trace_pid=<verified-pid>
mkdir -p "$gpu_trace_dir/analysis"
xcrun xctrace record --template 'Metal System Trace' \
  --attach "$gpu_trace_pid" --time-limit 10s --no-prompt \
  --output "$gpu_trace_dir/metal-system.trace"
xcrun xctrace export --input "$gpu_trace_dir/metal-system.trace" --toc \
  --output "$gpu_trace_dir/analysis/metal-toc.xml"
```

Check recording/export status, then select the actual run number and table from
the TOC. For `metal-gpu-intervals`, an example export is:

```sh
xcrun xctrace export --input "$gpu_trace_dir/metal-system.trace" \
  --xpath '/trace-toc/run[@number="1"]/data/table[@schema="metal-gpu-intervals"]' \
  --output "$gpu_trace_dir/analysis/metal-gpu-intervals.xml"
```

Other versions/templates may expose `metal-driver-event-intervals`, counter or
application tables. Export only present tables needed for the question. For
unfamiliar exports, the [generic XML helper](../../cputrace/scripts/parse_trace.py)
can inspect rows; it does not prove source coverage or attribution.

For a same-run 3DMark encoder CSV, use:

```sh
scripts/run_python.sh scripts/tools/summarize_xctrace_metal_intervals.py \
  --gpu-intervals "$gpu_trace_dir/analysis/metal-gpu-intervals.xml" \
  --dxmt-encoders '<same-run-3dmark05-perf-encoders.csv>' \
  --output-csv "$gpu_trace_dir/analysis/metal-joined.csv" \
  --output-md "$gpu_trace_dir/analysis/metal-joined.md" \
  --require-xctrace-render-rows --min-dxmt-join-coverage 0.99
```

Route-selection investigations additionally require route verdicts and, when
requested, indexed-probe routes; use the sidecar's generated commands for those
gates. Do not suppress a failed join or interpret missing rows as zero GPU work.

dxmt signposts use subsystem `com.dxmt9.translator`, category `metal`, with
`frame`, `commit` and `draw` intervals. They describe host boundaries, not GPU
execution. Match `RenderPass[seq=...,enc=...]` and submission identities before
attributing cost. Check which counter/shader tables contain actual observations;
metadata alone does not establish occupancy or shader hotspots.
