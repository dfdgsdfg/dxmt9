# Wine and PE CPU attribution

Read [Instruments rules](../../../rules/trace_instruments.rules.md) and [REA rules](../../../rules/debug_rea.rules.md). Processor Trace guest attribution is unqualified; do not generalize Time Profiler observations to a different collector.

For attributing the wine game thread's opaque
PE samples per module: **the observed Time Profiler lane cannot recover translated PE PCs on Apple Silicon.** PE x86 code
runs Rosetta-translated, and Instruments only maps translated PCs back to
origin images for dyld-known images — PE modules are not dyld images, so
their samples surface as unmapped high addresses (measured 2026-08-19: with a
probe-verified `DXMT9_PE_MODULE_MAP=1` module map, 92.7% of game-thread
samples were `unknown_64bit` and zero fell below 4 GiB). The dyld-loaded unix
side (`winemetal_dxmt9.so`) still symbolizes normally. Use the in-process PE
sampler instead: run with `DXMT9_PE_MODULE_MAP=1 DXMT9_PE_THREAD_SAMPLER=1
DXMT_LOG_LEVEL=info` (rate knob `DXMT9_PE_THREAD_SAMPLER_HZ`, default `250`;
see `agents/rules/environment_variables_bridge.rules.md`), which reads
the game thread's true Win32 `Eip`/`Rip` via SuspendThread/GetThreadContext and
classifies against the same module map. Join its cumulative
`[dxmt9-pe-sampler]` groups with

```sh
scripts/run_python.sh scripts/tools/symbolicate_xctrace_pe.py \
  --sampler-log traces/<run>/3dmark05-direct.log \
  --output-csv traces/<run>/analysis/pe-sampler.csv \
  --output-md  traces/<run>/analysis/pe-sampler.md
```

which also writes `pe-sampler-selfpc.csv` (top self-`d3d9.dll` PC buckets with
RVAs). **A sampler run is not a valid FPS sample** — it stops the game thread
`hz` times a second — so pair it with a clean no-sampler scout for any
wall-clock claim.
