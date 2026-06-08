# dxmt9 Environment Variables — Perf counters

Part of the [`environment_variables.rules.md`](environment_variables.rules.md)
index (counter system, per-frame snapshot, and 3DMark05 GT1 perf-probe knobs).
A flag is "set" when its value is a non-empty string that is not `0`, unless
documented otherwise. See the index for global notes, and
[`metal_debugging.rules.md`](metal_debugging.rules.md) §9 for the 3DMark05 GT1
perf-probe toolkit.

## Perf counters

| Var | Purpose | Default |
|---|---|---|
| `DXMT_PERF_COUNTERS` | Enable `[dxmt9-perf]` counter line at exit | `0` |
| `DXMT_PERF_COUNTERS_PERIODIC_PRESENTS` | Emit counters every N presents (numeric) | `0` |
| `DXMT9_PERF_FRAME_SAMPLING` | Per-frame counter delta snapshots | `0` |
| `DXMT9_PERF_ENCODER_GPU_TIME` | Opt-in per-encoder GPU-time via counter sample buffers at render-encoder boundaries; pairs with `DXMT_PERF_COUNTERS`. See `metal_debugging.rules.md` §5 path B | `0` |
| `DXMT9_PERF_ENCODER_BREAKDOWN` | Emit per-render-encoder `[dxmt9-perf-encoder]` summary and `[dxmt9-perf-encoder-stream]` stream breakdown lines for stream handle/offset/stride churn, stream Metal-bind first/handle/offset reasons, stream/IB unique handle bytes/usage/pool buckets, IB handle churn, primitive/vertex/FFP/pre-transformed geometry shape, PSO/shader-variant/VSOut-layout attribution including layout-cache hit/miss counts, VS/PS shader hash and, when `DXMT_DUMP_SHADER_DIR` is set, exact VS/PS source hash attribution, argbuf table/cbuf bytes including VS/FFPVS first/rewrite/field splits, setVertexBytes slot-5/other bytes, transient vertex/index bytes split by UP preupload, decl/shadow fallback, and indexed expansion, VS float upload-plan ranges, and write attribution | `0` |
| `DXMT9_PERF_ENCODER_BREAKDOWN_SEQ` | Optional numeric filter for `DXMT9_PERF_ENCODER_BREAKDOWN=1`; when set, emit encoder breakdown rows only for one render-pass sequence id such as frame-capture `seq=60`, keeping 3DMark05 no-gputrace logs bounded during per-frame attribution probes | unset |
| `DXMT9_PERF_RENDER_PASS_REENTRY_TOP` | Optional top-N per-frame same-key render-pass re-entry diagnostic. Emits `[dxmt9-perf-render-pass-reentry]` rows with A/B attachment handles, true B->A encoder path, read-relation bits, store-proof owners, touch distances, and preservation bytes. Use with `DXMT9_PERF_ENCODER_BREAKDOWN=1` or wrapper `--render-pass-reentry-top N` when joining re-entry role pairs to encoder load/store/clear action shape | unset |
| `DXMT9_PERF_TEXTURE_SAMPLER_DIRECT_SPLIT` | Heavy opt-in per-entry attribution for the fragment texture/sampler direct lane: texture resolve, direct texture branch/set, direct sampler branch/set. Use only for short no-gputrace CPU attribution probes because the nested scopes perturb the default perf profile | `0` |
| `DXMT_3DMARK05_REQUIRE_UNLOCKED` | 3DMark05 launcher guard: fail early when macOS reports `CGSSessionScreenIsLocked=Yes`, avoiding false black/factory-only perf captures. Set to `0` only for deliberate locked-session experiments | `1` |
| `DXMT_3DMARK05_PREFIX` | Direct 3DMark05 launcher prefix override. `run_3dmark05_perf_probe.sh` sets this to `experiments/prefixs/app-d3d9-3dmark05` so standard probes use the catalogue app prefix even if the optional `app-d3d9-3dmark05-verify` prefix was cleaned up | launcher default: verify prefix |
| `DXMT_3DMARK05_RESULT_FILE` | Append a 3DMark05 result-file argument such as `dxmt9_gt1.3dr` after the selected command-line tests, enabling documented unattended result runs when the desktop is unlocked | unset |
| `DXMT_3DMARK05_PROBE_TIMEOUT` | Default runner timeout for `run_3dmark05_perf_probe.sh`; when unset the wrapper uses `420s` with gputrace and `180s` with `--no-gputrace`, passes that value to `run_experiment.py --timeout`, and rejects disabled/zero timeouts because 3DMark05 may hang on the final frame | derived |
| `DXMT_3DMARK05_PROBE_TIMEOUT_SLACK` | Extra seconds added to the perf wrapper's top-level watchdog beyond `DXMT_3DMARK05_PROBE_TIMEOUT` / `--timeout`. The watchdog runs the whole `caffeinate run_experiment.py ...` command in a fresh process group and terminates it if the catalogue runner does not return after its own timeout/finalization window | `45` |
| `DXMT_3DMARK05_SET_MTL_CAPTURE_ENABLED` | Opt back into adding `MTL_CAPTURE_ENABLED=1` for 3DMark05 perf-probe gputrace runs. Keep unset for normal probes because `MTL_CAPTURE_ENABLED=1` alone has reproduced black-screen startup with draw/present counters at zero | `0` |
| `DXMT_3DMARK05_METAL_CAPTURE_DESTINATION` | Forwarded to `DXMT_METAL_CAPTURE_DESTINATION` by the 3DMark05 perf wrapper; use `developerTools` for an attached-Xcode capture route, otherwise default file `.gputrace` capture remains `gpuTraceDocument` | unset |
| `DXMT_3DMARK05_DIRECT_TIMEOUT` | Timeout used by `scripts/run_apps/run_app-d3d9-3dmark05-verify_direct.sh` for manual direct-prefix runs. The wrapper kills the process group on timeout so a final-frame hang does not require manual cleanup | `180` |
| `DXMT_3DMARK05_DIRECT_DRY_RUN` | Print the direct 3DMark05 wrapper timeout and launcher command without starting Wine | `0` |
| `DXMT_3DMARK05_LAUNCHER_TIMEOUT` | Direct-shell fallback timeout used by `experiments/launchers/app-d3d9-3dmark05.sh` only when it is not already supervised by `run_experiment.py` or the direct wrapper. Set a positive value for longer manual suites; disabled/zero values are rejected | `180` |
| `DXMT_3DMARK05_ALLOW_UNSUPERVISED` | Bypass the 3DMark05 launcher's direct-shell fallback timeout. Use only when another documented supervisor owns process lifetime | `0` |
| `DXMT_3DMARK05_KILL_SERVER_ON_EXIT` | Direct 3DMark05 launcher cleanup: kill the app prefix wineserver on normal exit or TERM/INT so timeout-finalized runs do not leave a detached 3DMark05 process alive | `1` |
| `DXMT_3DMARK05_MIN_TRACE_FREE_MB` | `run_3dmark05_perf_probe.sh` free-space guard before launching Wine/gputrace; defaults to `2048` with gputrace and `256` with `--no-gputrace` | derived |
| `DXMT_3DMARK05_CLASS_PROXY_TOP` | Default top-N indexed state/class proxy rows emitted by `finalize_3dmark05_perf_probe.sh` after Xcode/dxmt joining; use it to bound `frame<N>-indexed-state-class-xcode-proxy.{md,csv}` | `12` |
| `DXMT_3DMARK05_MAX_TOP_UNEXPLAINED_BUFFER_WRITE_RATIO` | Default for the Xcode comparison gate `--max-top-unexplained-buffer-write-ratio`, failing candidates whose top encoder buffer-write traffic remains mostly unexplained by dxmt CPU-side writers | unset |
| `DXMT_3DMARK05_MAX_CONST_UPLOAD_BREAK_COUNT_RATIO` | Default for the run-level comparison gate `--max-const-upload-break-count-ratio`, failing sparse/coalesced constant-upload candidates that reduce bytes by creating too many const-upload draw-run breaks | unset |
