# dxmt9 Environment Variables

Index of `DXMT*` environment variables honored by the dxmt9 runtime or
repository harnesses. The full per-knob tables live in the per-domain
`environment_variables_<domain>.rules.md` files; this file is the map so a
developer can find every knob without grepping. Regenerate the raw var list
from source on demand:

```sh
rg -o '"DXMT9?_[A-Z0-9_]+|DXMT_[A-Z0-9_]+"' src scripts/run_apps scripts/run_suites scripts/tools | sort -u
```

A flag is "set" when its value is a non-empty string that is not `0`, unless
documented otherwise.

## Domains

| Domain file | Covers |
|---|---|
| [`environment_variables_capture.rules.md`](environment_variables_capture.rules.md) | **Capture / Debug** — frame capture, texture/shader dumps, validation, and **mini-replay** per-draw payload capture. |
| [`environment_variables_logging.rules.md`](environment_variables_logging.rules.md) | **Logging / Tracing** — log level, log path, trace switches. |
| [`environment_variables_perf.rules.md`](environment_variables_perf.rules.md) | **Perf counters** — counter system, per-frame snapshot, and 3DMark05 GT1 perf-probe launcher knobs. |
| [`environment_variables_present.rules.md`](environment_variables_present.rules.md) | **Present policy** — present-acquire / boundary / latency tuning. |
| [`environment_variables_renderer.rules.md`](environment_variables_renderer.rules.md) | **Renderer / Frame Graph** — backend selection, modern-renderer feature gates, and DAG debug export. |
| [`environment_variables_cache.rules.md`](environment_variables_cache.rules.md) | **Pipeline cache** — archive prewarm and cache-root controls. |
| [`environment_variables_encoder.rules.md`](environment_variables_encoder.rules.md) | **Encoder / state debug** — force-state knobs, index-cache locality opt-ins, and 3DMark05/Xcode backend-shape classifiers. |
| [`environment_variables_bridge.rules.md`](environment_variables_bridge.rules.md) | **PE bridge / recorder** — PE-side chunk recorder diagnostics. |
| [`environment_variables_adapter.rules.md`](environment_variables_adapter.rules.md) | **Adapter spoofing / Compatibility** — D3D9 driver ID overrides and known-issue opt-out / opt-in flags. |
| [`environment_variables_wine.rules.md`](environment_variables_wine.rules.md) | **Cross-process / Wine / Apple-side** — Wine, PE/unix bridge, experiment-harness plumbing, and macOS / Metal toolchain knobs. |

## Notes

- Values are read once at process start (typically inside a static
  initializer) — changing them after dxmt9 has loaded does not take
  effect.
- A zero string (`""`) and `"0"` mean "off" for boolean flags.
- For tunable numerics, an unparseable value falls back to the default.
- These files are **descriptive**, not a behavioral spec — for that, see
  `specs/`.
