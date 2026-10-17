# dxmt9 Environment Variables — Logging / Tracing

Part of the [`environment_variables.rules.md`](environment_variables.rules.md)
index (log level, log path, trace switches). A flag is "set" when its value is
a non-empty string that is not `0`, unless documented otherwise. See the index
for global notes.

## Logging / Tracing

| Var | Purpose | Default |
|---|---|---|
| `DXMT_LOG_LEVEL` | `Error` / `Warn` / `Info` / `Debug` / `Trace` | `Warn` |
| `DXMT_LOG_PATH` | Redirect log to file | stderr |
| `DXMT_TRACE_FILE` | Trace output file | unset |
| `DXMT_TRACE_RENDER` | Trace render encoder | unset |
| `DXMT_TRACE_QUEUE` / `DXMT_TRACE_QUEUE_FROM` | Trace queue events | unset |
| `DXMT_TRACE_ENCODE_SEQ` | Trace encode sequence ids | unset |
| `DXMT9_TRACE_ENCODE_PROGRESS` / `DXMT9_TRACE_ENCODE_PROGRESS_SEQ` | Trace encodeChunk progress stages and per-command begin/end markers; use the seq filter for capture-layer hang diagnostics | unset |
| `DXMT_TRACE_FVF` / `DXMT_TRACE_FVF_TEX0` / `DXMT_TRACE_FVF_EXPANDED` | FVF decode trace | unset |
| `DXMT_TRACE_SHADER_INPUTS` | Shader input binding trace | unset |
| `DXMT_TRACE_TEXTURE_HANDLE` | Trace per-handle texture events | unset |
| `DXMT9_TRACE_DRAW_GEOMETRY` / `DXMT9_TRACE_DRAW_GEOMETRY_LIMIT` | Draw geometry diagnostics | unset |
| `DXMT9_BRIDGE_VERBOSE` | Log rejected or suspicious winemetal bridge handles | `0` |
| `DXMT9_BRIDGE_TRACE_CALLS` | Log the first N winemetal PE→unix bridge calls with opcode names, status, and duration | `0` |

## Native Perfetto Capture

| Variable | Meaning |
|---|---|
| `DXMT9_TRACE_CONFIG` | Absolute path to the immutable `dxmt9.trace.request.v1` request written by the catalogue runner. Requires an explicit `-Ddxmt9_perfetto=true` unix build; absent means no session or SDK initialization. |
| `DXMT9_TRACE_RECEIPT` | Runner-selected receipt destination, bound to the frozen request. A missing or incomplete receipt fails the requested tracing gate independently of workload status. |
| `DXMT9_TRACE_PE` | Exact value `1` enables bounded PE chunk-boundary batches with QPC and creation identities. The runner sets this only for explicitly requested PE coverage. Disabled paths do not read clocks or cross the bridge. |

Use `run_experiment.py run <app-id> --trace-profile timeline --trace-dry-run`
to inspect the resolved window and paths. GPU/PE clocks retain their source
identity until calibration qualifies alignment. See
[`specs/experiments/harness/tracing/spec.md`](../../specs/experiments/harness/tracing/spec.md).
