# 3DMark05 GT1 probes

Read [supervision](gt1-supervision.md) before launch and [Xcode counter export](xcode-counters.md) before finalization. Run commands from the repository root.

For dxmt9's 3DMark05 GT1 GPU-bottleneck investigation, do not hand-build
`xctrace` / `.gputrace` runs. Use the repository toolchain, which handles
the Wine launch, the final-frame hang timeout, capture/finalize, and the
Xcode-counter ↔ dxmt-attribution join:

For this specific 3DMark05 workflow, do not add `MTL_CAPTURE_ENABLED=1` or
`METAL_CAPTURE_ENABLED=1` unless `DXMT_3DMARK05_SET_MTL_CAPTURE_ENABLED=1` is
deliberately requested. The capture-layer env path has reproduced black-screen
startup with draw/present counters at zero; the standard wrapper uses dxmt9's
`DXMT_METAL_CAPTURE_FRAME/PATH` trigger without it by default.
For draw-local force-white queues generated from effect-geometry CSVs, treat
`command_index`/`command_draw_index` as a narrow replay target, not proof. The
follow-up run is valid only when the perf summary reports nonzero
`probe_force_texture_white_draws`/PSO-bypass counts and the captured frame
matches the baseline target scene. If either count stays zero, or the HUD/scene
has drifted, classify the output as selector/frame drift and use same-run
instrumentation or direct Xcode draw inspection instead of reading the image
delta as an A/B result.
For color-attachment after-draw histories, remember that every matched dump
forces a render-encoder split. If the target draw is adjacent to an earlier
matching draw, an `enc` gate can accidentally exclude it after the first split.
Prefer `seq + texture/command` gates first, then read the resulting sidecar
metadata for the actual `enc`, `draw`, `commandIndex`, and
`commandDrawIndex`.
Apple's file `.gputrace` destination still needs the process capture layer
(`MetalCaptureEnabled` Info.plist key or macOS capture-layer env), so the
expected no-layer failure is `Capture layer is not inserted`.
The standard 3DMark05 wrapper preflights file capture before launching Wine and
fails early when both `wine.real` and `wine-preloader` lack
`MetalCaptureEnabled` and no capture-layer env was requested. Use
`DXMT_3DMARK05_ALLOW_NO_FILE_CAPTURE_LAYER=1` only for an intentional late
`Capture layer is not inserted` diagnostic.
If normal rendering works but file capture fails with `Capture layer is not
inserted`, only try an Xcode attach-after-normal-start route when the capture
layer can be proven on the real Wine temp child; rerun with
`--xcode-developer-tools-capture` or
`--metal-capture-destination developerTools` only for that explicit diagnostic.
This route does not write `frame<N>.gputrace` directly; the wrapper proves
`destination=1` start/stop log lines and the useful Xcode artifacts must be
exported into `traces/<run>/analysis`. Merely opening Xcode at the welcome
window is not enough. Preflight that Xcode can actually attach to the real Wine
child before launching a long run (`--xcode-attach-preflight-only`); Developer
Mode disabled, attach-by-PID disabled, and process-list loading are all
preflight failures. Choose a target frame that the run is known to reach;
otherwise classify the run as attach-preflight or frame-target failure, not
capture-layer evidence. If
`developerTools` still logs
`Capture layer is not inserted`, classify the run as capture-layer failure and
do not use its output as an Xcode/perf sample.
The in-place copied `wine.capture.real` / `wine.capture.real-preloader`
experiment with embedded `MetalCaptureEnabled=true` also rendered GT1 normally
but still failed file capture with `Capture layer is not inserted`. Treat that
as a negative capture-layer result unless a future run proves that Xcode itself
launched or attached the real Wine temp child.
For deliberate 3DMark05 file `.gputrace` diagnostics, prefer
`scripts/tools/run_3dmark05_perf_probe.sh --with-wine-capture-layer ...`; it
uses the standard supervised probe path and wraps the launch in the Wine
capture-layer patcher. For non-3DMark05 Wine/D3D9 capture-layer diagnostics,
or when manually wrapping a lower-level command, the repository wrapper
`scripts/tools/run_with_wine_metal_capture_layer.sh --wine-root <root> -- ...`
can temporarily patch `<root>/bin/wine.real` and `wine-preloader`, let Wine copy
the patched binaries into `/var/folders/.../winetemp...`, and restore the
originals. It produced valid `.gputrace` captures for
`perf-d3d9-present-loop` and for a deliberate 3DMark05 frame60 diagnostic
(`capture-layer-atomic-r9`). The wrapper's file swap must stay atomic:
same-directory temp file plus `mv` for both patch and restore. Do not replace
that with in-place `cp` overwrites; the `cp` path reproduced
`SIGKILL (Code Signature Invalid)` / `Taskgated Invalid Signature` on the
original `wine.real` path even though `codesign --verify` passed. When wrapping
`run_experiment.py` directly, pass the same `--wine-root <root>` to the child
command; otherwise the wrapper can patch one Wine tree while the catalogue
runner auto-detects and launches another one. Also pass
`DXMT_METAL_CAPTURE_PATH` as an absolute path when using the manual wrapper:
catalogue launchers `cd` into the app workdir before Wine starts, so a relative
`traces/...` path is resolved under `experiments/apps/<app>/traces/...` instead
of the repository trace root. The 3DMark05 probe wrapper already uses an
absolute `$repo_root/traces/<run-id>/frame<N>.gputrace` path. The wrapper
refuses 3DMark05 command lines by default because capture-layer runs are
diagnostic/Xcode-counter routes, not normal wall-clock FPS samples; use
`--allow-3dmark05` or `DXMT9_ALLOW_3DMARK05_CAPTURE_LAYER=1` only for a
deliberate capture run and pair it with a no-gputrace scout for FPS claims.

For render-pass dependency debugging, use the Frame Graph DAG dump as a
no-Xcode sidecar before escalating to another `.gputrace`. It is an
observation-only channel on both the traditional and framegraph backends:

```sh
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix <tag> --frame <N> --no-gputrace --timeout 120 \
  --dump-framegraph-dag \
  --framegraph-dag-frame <N> \
  --framegraph-dag-frame-radius 2 \
  --framegraph-dag-formats json,mermaid \
  --framegraph-dag-optimize passcoalesce
# Add --framegraph-dag-draws when JSON draw ownership is needed.
```

Do not leave the DAG frame filter unset on 3DMark05 unless you are deliberately
collecting a flood of chunks. The wrapper stores raw dumps under
`traces/<app-runid>/analysis/dag` and writes combined
`framegraph-dag-summary.{md,csv}` / `framegraph-dag-candidates.csv`, plus
stage-specific `framegraph-dag-preopt-*` and `framegraph-dag-postopt-*`
summaries. Use pre-opt candidates to find legal-looking coalesce targets, and
post-opt summaries to confirm the analysis optimizer removed those re-entry
pairs from the exported DAG.
Interpret `--framegraph-dag-optimize passcoalesce` as a structural proof of
candidate coalescing only; production encode still has to be verified
separately by byte-equal output and Xcode counters.

```sh
# capture (no-gputrace scout = 120s, .gputrace/Xcode candidate = 420s)
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix <tag> --frame <50|60> [--no-gputrace] <probe flags> --timeout <120|420>
# then export Encoder Counters from Xcode (see [Xcode exports](xcode-counters.md)), then:
bash scripts/tools/finalize_3dmark05_perf_probe.sh \
  --suffix <tag> --frame <N> [--baseline-joined <csv> <proof gates>]
```

When `.gputrace` replay is blocked, prefer
`scripts/tools/run_3dmark05_system_trace_sidecar.sh -- ...` for
normal-rendering Metal System Trace sidecars. It runs the probe wrapper dry-run
first, refuses locked sessions before starting xctrace, can wait for unlock
with sidecar `--wait-unlocked-sec N`, records `traces/<run>/metal-system.trace`, then exports/summarizes
`metal-gpu-intervals` against dxmt encoder attribution. Manual fallback runs
can still use `xcrun xctrace record --template 'Metal System Trace'
--all-processes`, keep the trace at `traces/<run>/metal-system.trace`, and run
the wrapper-printed `xctrace_system_trace_export_cmd` and
`xctrace_system_trace_summary_cmd`. Keep at least `4GiB` free before starting
the sidecar; a 10s all-processes 3DMark05 trace has failed at the Instruments
save/trim step with about `2.4GiB` free. For backend-route selection, current
encoder-breakdown rows carry coarse `route_*` primitive counters, so the
System Trace summary can emit depth-only/textured/color route verdicts without
indexed per-draw logging. Add `--measure-index-reuse` only when per-draw route
detail, exact draw selectors, or indexed-cache metrics are needed; indexed
rows override the coarse encoder summary and are guarded by
`--require-indexed-probe-routes` when requested. The wrapper/summarizer require
RenderPass-labelled xctrace rows, high dxmt encoder join coverage, and at least
one route verdict so a header-only or non-joining telemetry path fails instead
of producing a misleading `route-unavailable` sidecar.
The sidecar defaults to `--encoder-breakdown-all-frames` on the wrapped probe
and rejects exact scoped encoder-breakdown sequence settings, because xctrace
records a wall-clock window rather than the single sequence selected by
`DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=<frame>`. All-frame breakdown is useful as a
join proof but can make runtime FPS unrepresentative; for lower-overhead timing
samples, pass sidecar `--encoder-breakdown-seq-range MIN:MAX` to forward
`DXMT9_PERF_ENCODER_BREAKDOWN_SEQ_MIN/MAX` and let the join coverage gate prove
the range covered xctrace's RenderPass seq window. If rows appear before
`MIN`, treat the run as a stale-provider diagnostic first: rebuild and restage
the active Wine unix `winemetal_dxmt9.so` before reading FPS or route timing.

## Experiment classes

| Experiment class | Key wrapper/env flags | docs/perfomance domain |
|---|---|---|
| Baseline capture | `--frame 50` / `--frame 60`, `--no-gputrace`, `--encoder-breakdown-seq N` (`DXMT9_PERF_ENCODER_BREAKDOWN`) | `baselines.md` |
| State / backend-shape classifiers | `--disable-cull`, `--disable-scissor`, `--disable-fog`, `--disable-alpha-test`, `--force-cull-mode`, `--force-texture-white`, `--probe-disable-alpha-blend[-row/-rows/-class/-classes]`, `--probe-depth-func-always[-row/-rows/-class/-classes]`, `--probe-disable-depth-write[-row/-rows/-class/-classes]`, `--force-visible`, `--force-expand-indexed` | `backend-shape-classifiers.md` |
| VSOut layout / shader codegen | `--probe-half-vsout`, `--trim-unused-varyings`, `--dump-shaders` | `vsout-layout.md` + `shader-codegen.md` |
| Index reorder / split diagnostics | `--probe-reverse-indexed-triangles[-row/-rows/-class/-classes]`, `--probe-reverse-opaque-indexed-triangles`, `--probe-reverse-nonopaque-indexed-triangles`, `--probe-sort-indexed-triangles-by-min-index`, `--split-large-indexed-draws[-row/-rows/-class/-classes/-max-chunks-per-draw/-stream0-span-max]`, `--measure-index-reuse`, `--measure-index-cache-opt-candidate` | `primitive-reorder-diagnostics.md` + `index-reuse-measurement.md` |
| Accepted / explicit index-cache opt | `--optimize-opaque-depth-index-cache[-min-gain-pct]` + `--require-opaque-depth-index-cache-proof`; explicit-tolerance screen-blend `--optimize-screen-blend-index-cache[-min-gain-pct]` + `--require-screen-blend-cache-proof` + `--semantic-image-policy exact|lsb1` | `index-cache-locality.md` |
| Const / cbuf upload | encoder-breakdown cbuf class/field rows | `const-upload.md` |
| Render-pass store policy | `--aggressive-color-dontcare` (`DXMT9_AGGRESSIVE_COLOR_DONTCARE`), `--aggressive-depth-dontcare` (`DXMT9_AGGRESSIVE_DEPTH_DONTCARE`) | `render-pass-store.md` |
| Attachment pixel-format view | `--suppress-rt-pixel-format-view`, `--suppress-x8-rt-pixel-format-view`, `--x8-shader-alpha-fill` | `attachment-pixelformat.md` |
| Mini-replay + bisection | `--dump-indexed-geometry[-cbufs]` (`-vs`/`-ps`/`-max-draws`), `--dump-depth-attachment-{handle,seq,enc,path}` (`DXMT9_DUMP_DEPTH_ATTACHMENT_*`), then `run_3dmark05_mini_replay.py --primitive-order {reverse-triangles,sort-min-index,sort-max-index,cache-opt-lru32,cache-opt-lru64} --vertex-order {original,first-reference,scatter} --draw-order {original,reverse} --depth-input <sidecar>` | `mini-replay-bisection.md` |

Flag names above are taken from `run_3dmark05_perf_probe.sh --help` and
`run_3dmark05_mini_replay.py --help`; the wrapper exposes many more
scoped `-row/-rows/-class/-classes` variants and `--require-*` proof
gates (see [Xcode exports](xcode-counters.md) and the full `--help`). Mini-replay selects encoder
draws via the dumped manifest, not a draw-index flag — use
`--primitive-order` / `--vertex-order` / `--draw-order` / `--depth-input`
for A/B probes. The `--vertex-order` lanes reorder vertex storage and
rewrite indices to match, so they are bit-exact permutations that must
produce a pixel-identical image: gate them by comparing the color-output
hash against the `original` lane, and treat a mismatch as a remap bug
rather than a finding. `--primitive-order` lanes legitimately differ.

See also: `docs/perfomance/overview-3dmark05-gt1.md` (experiment
knowledge graph) and `agents/rules/environment_variables.rules.md`
(every `DXMT*`/`DXMT9*` knob these flags set).
