# dxmt9 Environment Variables — Pipeline cache

Part of the [`environment_variables.rules.md`](environment_variables.rules.md)
index (archive prewarm and cache-root controls). A flag is "set" when its value
is a non-empty string that is not `0`, unless documented otherwise. See the
index for global notes.

## Pipeline cache

| Var | Purpose | Default |
|---|---|---|
| `DXMT9_PREWARM` | Override Metal binary archive prewarm mode: `full` / `lazy` / `disabled` | release=`full`, debug=`lazy` |
| `DXMT9_CACHE_DIR` | Override dxmt9 cache root for shader archives | platform cache dir |
| `DXMT9_PSO_COMPILE_THREADS` | Override the PSO compile thread-pool worker count (numeric); `0`/unparseable falls back to the hardware-concurrency default | derived |
