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
