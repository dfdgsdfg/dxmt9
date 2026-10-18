# Metal frame capture and inspection

Read [Metal rules](../../../rules/debug_metal.rules.md). Run commands from the repository root; replace placeholder paths and frame numbers. For 3DMark, use [the supervised probe](gt1-probes.md).

## Programmatic frame capture (.gputrace)
`MetalCaptureController` (`src/dxmt9/dxmt9_capture.{hpp,cpp}`) triggers a
Metal frame capture on a target frame number, writing the result to a
`.gputrace` file you can open in Xcode.

```sh
export DXMT_METAL_CAPTURE_FRAME=120          # capture the 120th present
export DXMT_METAL_CAPTURE_PATH="$PWD/traces/<app-runid>/frame120.gputrace"
./your-app
# Use the supervised catalogue runner for Wine workloads.
```

For 3DMark05 perf probes, do **not** set `MTL_CAPTURE_ENABLED=1` or
`METAL_CAPTURE_ENABLED=1` by default.
It has reproduced black-screen startup with draw/present counters at zero.
The standard wrapper therefore relies on dxmt9's own
`DXMT_METAL_CAPTURE_FRAME/PATH` trigger and only adds Apple's capture-layer
env when `DXMT_3DMARK05_SET_MTL_CAPTURE_ENABLED=1` is deliberately set.
Apple's file `.gputrace` destination still requires the process capture layer
(`MetalCaptureEnabled` Info.plist key or macOS capture-layer env), so without
that layer the expected failure is `Capture layer is not inserted`.
The standard 3DMark05 wrapper now preflights file capture before launching
Wine and fails early when both `wine.real` and `wine-preloader` lack
`MetalCaptureEnabled` and no capture-layer env was requested. Use
`DXMT_3DMARK05_ALLOW_NO_FILE_CAPTURE_LAYER=1` only when intentionally
collecting the late `Capture layer is not inserted` failure.
For 3DMark05 file-capture diagnostics, prefer
`scripts/tools/run_3dmark05_perf_probe.sh --with-wine-capture-layer ...`; it
wraps the standard supervised probe in the Wine capture-layer patcher. For
general Wine/D3D9 capture-layer diagnostics, or when manually wrapping a
lower-level command, use
`scripts/tools/run_with_wine_metal_capture_layer.sh --wine-root <root> -- ...`.
It temporarily replaces `<root>/bin/wine.real` and `wine-preloader` with
capture-enabled copies, lets Wine copy those names into its
`/var/folders/.../winetemp...` launcher, then restores the originals. This has
produced valid `.gputrace` captures for `perf-d3d9-present-loop` and a
deliberate 3DMark05 frame60 file-capture diagnostic
(`capture-layer-atomic-r9`). The wrapper must replace and restore the Wine
loader files with same-directory temp files plus `mv`; do not change it back to
in-place `cp` overwrites. The `cp` path reproduced `SIGKILL (Code Signature
Invalid)` / `Taskgated Invalid Signature` on the original `wine.real` path even
when `codesign --verify` passed. If the wrapped child is a direct
`run_experiment.py` invocation, pass the same `--wine-root <root>` to that
child; patching one Wine tree while the catalogue runner auto-detects another
one does not insert the capture layer into the launched process. Also pass
`DXMT_METAL_CAPTURE_PATH` as an absolute path. Catalogue launchers `cd` into the
app workdir before Wine starts, so relative `traces/...` capture paths are
resolved under `experiments/apps/<app>/traces` rather than the repository trace
root. The 3DMark05 probe wrapper already uses an absolute
`$repo_root/traces/<run-id>/frame<N>.gputrace` path. Do not treat
capture-layer-inserted 3DMark05
runs as normal FPS samples; use them for `.gputrace`/Xcode counters and pair
them with no-gputrace scouts for wall-clock claims. For a `developerTools`
diagnostic, use wrapper `--xcode-developer-tools-capture` or
`--metal-capture-destination developerTools` only after Xcode is attached to the
real Wine child before the target frame. Preflight the Xcode UI first: if
`/usr/sbin/DevToolsSecurity -status` reports Developer Mode disabled,
`Debug > Attach to Process by PID or Name...` is disabled, or
`Debug > Attach to Process` stays at `Getting Process List...`, the attach route
is unavailable and a long `developerTools` run should not be started. Use
`run_3dmark05_perf_probe.sh --xcode-attach-preflight-only` for a no-launch
check, and add `--require-xcode-attach-preflight` to fail before launch on a
`developerTools` capture run. Also pick a target frame that the run is known to
reach; a run that exits or times out before `DXMT_METAL_CAPTURE_FRAME` cannot
prove capture-layer behavior because no start/stop log is expected. This route
does not write
`frame<N>.gputrace` directly, so the wrapper proves `destination=1` start/stop
log lines and the useful Xcode artifacts must be exported under
`traces/<run>/analysis`. Merely opening Xcode at the welcome window is not
enough; if `developerTools` still reports `Capture layer is not inserted`,
classify the run as capture-layer failure, not Xcode/perf evidence. **If you
trigger the capture from Xcode's own capture button on a layer-inserted Wine
child, rather than through dxmt9's `DXMT_METAL_CAPTURE_FRAME`, set
`DXMT9_PER_DRAW_DEBUG_GROUPS=1`** — the per-draw `Draw[...]` groups are gated on
a capture env being present, and an externally-initiated capture sets none, so
the `.gputrace` would arrive with no per-draw narrative and no warning.
`run_with_wine_metal_capture_layer.sh` exports it for you. The wrapper
refuses 3DMark05 command lines by default; use
`--allow-3dmark05` or `DXMT9_ALLOW_3DMARK05_CAPTURE_LAYER=1` only for a
deliberate diagnostic that is expected to be invalid as a perf sample.

Find request/start/stop sites in `src/dxmt9/dxmt9_command_queue.cpp` and
`src/dxmt9/dxmt9_queue.cpp`; source line numbers drift between builds.

## Reading the capture (labels + debug groups)

When you open the `.gputrace`, captured resources and command encoders
are already labeled and grouped:

- **Buffers**: `vb_h0xN`, `ib_h0xN`, `argbuf_seqN`, `transient_seqN`
- **Textures**: `tex_h0xN_<format>_<w>x<h>`
- **CommandQueue**: `dxmt9-q-<deviceId>`
- **CommandBuffer**: `cb_seq_N`
- **Pipelines**: `pso_h<shader-hash>`
- **Render encoders**: label + nested debug groups
  `RenderPass[rt=0xH,depth=0xH]` → `Draw[seq=N,prim=M]`. The label is what
  `xctrace` (`metal-application-encoders-list` and `metal-gpu-intervals`)
  uses to identify the encoder in text traces — without it the encoder
  falls back to Metal's default "Render Command N".

  **The per-draw `Draw[...]` groups are emitted only when a capture is
  configured** — `DXMT_METAL_CAPTURE_FRAME`, `MTL_CAPTURE_ENABLED` or
  `METAL_CAPTURE_ENABLED` — or when `DXMT9_PER_DRAW_DEBUG_GROUPS` forces them
  either way. They are not free: a string format, a bridged `WMT::String`
  allocation, and a `push`/`pop` pair on every draw cost `2.7 ms/present` of
  encode-thread time on GT2
  ([append-decomposition.16](../../../../docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.16.md)).
  Externally initiated Xcode captures need the explicit per-draw override above. The
  per-render-pass, blit and present groups are always on — there are a few per
  frame, not ~1,690, and `xctrace` joins on them.
- **Blit encoders**: `Blit[<reason>]`
- **Present encoder**: `Present[seq=N]`

If a capture shows opaque `Buffer 0x12345` instead of these names, an
upstream creation site is missing a `setLabel` call — check
`scripts/check/audit_perf_counter_callsites.py`-style audit on M1
sites is healthy.

## Validation layer

Run with the standard Apple Metal Validation Layer:

```sh
export MTL_DEBUG_LAYER=1
export MTL_DEBUG_LAYER_VALIDATE_LOAD_ACTIONS=1
./your-app
```

Validation messages go to stderr. dxmt9 also surfaces command-buffer
errors itself: `WMTCommandBufferStatusError` is logged via the
`dxmt9-metal` logger and incremented as the
`gpu_command_buffer_errors` perf counter (M5,
`dxmt9_queue.cpp:1444-1448`).

For a regression-resistant gate, the perf probes set
`expected_counters{ gpu_command_buffer_errors = { max = 0 } }` — a
single GPU fault during a probe trips the L3 expected-range gate.
