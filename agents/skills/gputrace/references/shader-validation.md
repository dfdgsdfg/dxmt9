# Metal validation and shader inspection

Use [Metal rules](../../../rules/debug_xcode_metal.rules.md) and the workload's
supervised runner. Enable `MTL_DEBUG_LAYER=1` for an API diagnostic and
`MTL_SHADER_VALIDATION=1` only when shader instrumentation addresses the question.
Retain stderr and the actual observer settings. Such runs are not clean timing
baselines. `WMTCommandBufferStatusError` diagnostics and
`gpu_command_buffer_errors` are failure evidence even if output is visible.

For dxmt-translated shaders, use the GT1 wrapper's `--dump-shaders` and finalizer
source-hash attribution described in [Xcode exports](xcode-counters.md). Inspect
the exact dumped MSL variant and matching bindings before changing shader code.
For standalone MSL, preserve the production target, language and feature flags:

```sh
xcrun -sdk macosx metal --version
xcrun -sdk macosx metal -c -gline-tables-only -Weverything \
  '<shader.metal>' -o '<analysis-dir>/shader.air'
xcrun -sdk macosx metallib '<analysis-dir>/shader.air' \
  -o '<analysis-dir>/shader.metallib'
```

Compiler success does not prove runtime resource correctness. Inspect buffers,
textures, offsets, formats and draw bindings in Xcode when needed. The local
bundle parser is best-effort inspection, not a replacement for the Metal Debugger.
Run it from the repository root:

```sh
scripts/run_python.sh agents/skills/gputrace/scripts/parse_gputrace.py \
  'traces/<app-runid>/frame<N>.gputrace'
scripts/run_python.sh agents/skills/gputrace/scripts/parse_gputrace.py \
  'traces/<app-runid>/frame<N>.gputrace' \
  --buffer '<verified-label>' --layout float4 --index 0-10
```

Verify the true element layout/stride and resource file before decoding bytes.
The helper's `float3` is a packed three-float interpretation, not proof of Metal
struct alignment. Preserve canonical dxmt labels rather than adding spaces or
renaming resources to satisfy its label heuristic. Missing raw files require
Xcode inspection or an explicit limitation, not inferred buffer values.

For screenshots, prefer existing workload oracle/readback support. Do not add
production readback or alter `framebufferOnly` merely to inspect a supplied
capture. A screenshot can locate visual corruption; exact correctness claims
need the [rendering evidence workflow](../../../rules/rendering_correctness.rules.md).
For HUD diagnostics, scope `MTL_HUD_ENABLED` / `MTL_HUD_LOGGING_ENABLED` to the
workload rather than changing the user's global launch environment.
