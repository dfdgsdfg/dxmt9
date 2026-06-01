# experiments/launchers

Per-app or synthetic-probe shell launchers invoked by
`scripts/run_apps/run_experiment.py` per `experiments/CATALOGUE.toml`'s
`launcher` field. Filenames match `CATALOGUE.name` (kebab-case) so
`grep <app>` finds every artifact.

## Shared helpers (no CATALOGUE entry)

- `common.sh` — sourced by every launcher; provides `exp_stage_dxmt9`,
  `exp_run_wine_binary`, prefix discovery, capture/log hooks.
- `conf-d3d9-fast-sanity.sh` — bundle launcher for the 5 d9vk fast-sanity apps
  (`conf-d3d9-clear`, `conf-d3d9-buffer`, etc.), shared rather than
  per-app.

## App-bound launchers (1 per CATALOGUE entry)

D3D9 SDK / DXUT / third-party samples:

- `sample-d3d9-basic-hlsl.sh` — DX SDK sample-d3d9-basic-hlsl.
- `sample-d3d9-tutorial07.sh` — DX SDK sample-d3d9-tutorial07 (skinned mesh).
- `sample-d3d9-hdr-formats.sh` — DX SDK sample-d3d9-hdr-formats (FP16/FP32 RT).
- `sample-d3d9-dxut-simple.sh` — DXUT framework simple sample.
- `sample-d3d9-irrlicht-lights.sh` — Irrlicht engine managed-lights demo.

Self-authored apps:

- `conf-d3d9-srgb-texture.sh` — conf-d3d9-intent-probe `srgbtexture` mode.
- `conf-d3d9-float-texture.sh` — conf-d3d9-intent-probe `float-texture` mode.
- `conf-d3d9-stream-source.sh` — conf-d3d9-intent-probe `stream-source` mode.
- `conf-d3d9-shademode-provoking.sh` — conf-d3d9-intent-probe `shademode-provoking` mode.
- `conf-d3d9-pointsize.sh` — conf-d3d9-intent-probe `pointsize-policy` mode.
- `conf-d3d9-yuv-format.sh` — conf-d3d9-intent-probe `yuv-format-policy` mode.
- `conf-d3d9-vendor-format.sh` — conf-d3d9-intent-probe `vendor-format-policy` mode.
- `sample-d3d9-multitexture-terrain.sh` — multitexture terrain demo.
- `sample-d3d9-water-rt.sh` — water render-target / refraction demo.
- `conf-d3d9-wsi-present.sh` — minimal WSI present smoke (CI bootstrap).

Commercial / 3rd-party titles (require external prefix):

- `app-d3d9-anno-1404.sh` — Anno 1404 Gold (Heroic prefix).
- `app-d3d9-sfiv-benchmark.sh` — SFIV benchmark (Heroic + CrossOver
  oracle lanes — see `scripts/run_apps/run_app-d3d9-sfiv-benchmark_experiment.sh`
  for installer-extraction wrapper).

## Synthetic perf probes (no app source — shared probe binary, parameter-driven)

- `perf-d3d9-bridge-empty.sh` — bridge round-trip baseline (no draw work).
- `perf-d3d9-chain-parametric.sh` — parametric chain length probe.
- `perf-d3d9-depth-heavy.sh` — depth/Z heavy workload.
- `perf-d3d9-encode-replay.sh` — encode-then-replay throughput.
- `perf-d3d9-ffp-only.sh` — fixed-function pipeline only.
- `perf-d3d9-many-draw.sh` — many small draws / draw-call throughput.
- `perf-d3d9-multi-rt.sh` — MRT (multiple render target) workload.
- `perf-d3d9-offscreen-heavy.sh` — offscreen render-target heavy.
- `perf-d3d9-present-loop.sh` — present-only inner loop.
- `perf-d3d9-present-only.sh` — present without encode (drawable cycle).
- `perf-d3d9-skeletal.sh` — skeletal / skinned mesh probe.

## Conventions

- Each launcher sources `common.sh` first, then calls `exp_stage_dxmt9`
  and `exp_run_wine_binary`. Per-app variation comes from CATALOGUE
  fields (`binary`, `window_title`, `capture_frame`, env vars).
- `DXMT_EXPERIMENT_PROFILE` selects shared runtime defaults:
  - `debug` (default): `DXMT_VALIDATE=1`, `DXMT_LOG_LEVEL=debug`.
  - `perf`: `DXMT_VALIDATE=0`, `DXMT_LOG_LEVEL=warn`,
    `DXMT_PERF_COUNTERS=1`, `DXMT_PERF_COUNTERS_PERIODIC_PRESENTS=60`,
    `WINEDEBUG=-all`.
  - For encoder-level attribution runs, add
    `DXMT9_PERF_ENCODER_BREAKDOWN=1`; it emits one
    `[dxmt9-perf-encoder ...]` summary line per render encoder plus
    `[dxmt9-perf-encoder-stream ...]` lines for used vertex streams. The
    summary includes stream handle/offset/stride churn, stream Metal-bind
    first/handle/offset reasons, stream/IB unique handle bytes/usage/pool
    buckets, IB handle changes, argbuf
    table/cbuf bytes split by VS/FFPVS/PS/FFPPS plus VS/FFPVS
    first/rewrite/field-attribution buckets, setVertexBytes slot-5/other
    bytes, transient vertex/index bytes split by UP preupload, decl/shadow
    fallback, and indexed expansion, primitive/vertex/FFP/pre-transformed
    geometry shape, PSO/shader-variant/VS/PS-hash/VSOut-layout attribution, VSOut
    layout-cache hit/miss counts, and VS float upload-plan ranges. Add
    `DXMT9_MEASURE_INDEX_REUSE=1` only for targeted diagnostics when you need
    indexed reference count, draw-local unique-index estimates, and finite
    vertex-cache miss estimates for 16/32/64 entries to compare against Xcode
    `VS Invocations`; it scans accessible index buffers on the draw path and
    is intentionally off by default. It is
    intentionally not enabled by the shared perf profile. After the run,
    `python3 scripts/tools/summarize_3dmark05_perf.py experiments/output/<run>`
    writes `3dmark05-perf-encoders.csv` and
    `3dmark05-perf-encoder-streams.csv` for UI-free per-encoder analysis, with
    top-encoder tables for cbuf bytes, transient vertex bytes, transient source
    split, stream handle churn, and IB handle churn. The stream CSV includes
    per-stream handle/offset/stride churn plus per-stream unique handle count,
    bytes, usage, and pool buckets.
  - `app-d3d9-3dmark05` requires an unlocked macOS desktop for its GUI
    launcher and foreground Wine window. The launcher fails early when
    `CGSSessionScreenIsLocked=Yes` unless
    `DXMT_3DMARK05_REQUIRE_UNLOCKED=0` is set deliberately; locked-session
    attempts otherwise produce factory-only perf logs and misleading black
    captures.
  - `DXMT_3DMARK05_RESULT_FILE=<name>.3dr` appends the documented result-file
    argument after the test options, for example `-gt1 ... dxmt9_gt1.3dr`.
    Use it for unattended perf runs together with an unlocked desktop; keep
    `DXMT_3DMARK05_AUTO_ENTER=1` available as a fallback for editions that do
    not honor command-line result runs.
  - `scripts/tools/run_3dmark05_perf_probe.sh` wraps the current GT1 perf probe
    recipe: perf profile, direct 3DMark05 launcher, no auto indexed expansion,
    encoder breakdown, optional `DXMT_METAL_CAPTURE_FRAME/PATH`, trace output
    under `traces/app-d3d9-3dmark05-<suffix>/`, `MTL_CAPTURE_ENABLED=1` when
    gputrace capture is enabled, and automatic
    `3dmark05-perf-summary.md` / CSV generation after the run. Use `--dry-run`
    first to verify paths, desktop lock state, and free-space guard. The
    wrapper requires `2048MiB` free by default when gputrace capture is enabled
    (`--min-free-mb N` / `DXMT_3DMARK05_MIN_TRACE_FREE_MB=N` overrides it).
    Use `--measure-index-reuse` for the optional unique-index diagnostic; the
    final joined report will include `dxmt indexed references / unique
    estimate`, `VS invocations / dxmt indexed unique estimate`, and 16/32/64
    finite-cache miss estimates when the index data was readable.
    Dry-run and guard failures print `traces/`, `experiments/output/`, and
    the largest trace/output files when free space is below the guard, so
    cleanup can happen before launching Wine. If `--baseline-joined` or
    `--require-top-pso-attribution` is passed to the wrapper, dry-run also
    prints the exact `finalize_cmd_after_xcode_export` command to run after
    Xcode exports `frame<N>-counters-xcode.csv`.
    Add `--dump-shaders` only for root-cause captures that need shader source
    inspection; it writes translated MSL under
    `traces/<run-id>/analysis/shaders/msl` and D3D shader bytecode under
    `traces/<run-id>/analysis/shaders/bytecode` so top
    `dxmt_shader_variant_*` / `dxmt_vsout_layout_*` rows can be inspected
    without rerunning the app. The finalizer writes
    `frame<N>-shader-dump-report.md` and `frame<N>-shader-dump-summary.csv` by
    matching joined-summary `dxmt_vertex_shader_last` /
    `dxmt_pixel_shader_last` plus current-log
    `dxmt_vertex_shader_source_last` / `dxmt_pixel_shader_source_last` to
    `analysis/shaders/msl/*-shader-<hash>-source-<source>.metal`. If multiple
    dumped source hashes exist for the same shader hash and the log has no
    source hash, the report marks the row as `ambiguous_*_dump`; inspect the
    candidate count before making exact source-level claims. For shader
    root-cause captures, add
    `--require-shader-dump-matches` with `--dump-shaders`; the finalizer fails
    when top render rows have zero shader hashes, no matching dumped MSL, or
    only ambiguous dumped-source candidates. The shader dump report also
    records approximate `VSOut` byte width, `VSOut` field types, and
    stage-output assignment count from the matched MSL, plus fragment
    stage-in `VSOut` read fields, texcoord read mask, and emitted-but-unread
    `VSOut` fields with estimated bytes, plus
    local translated-VS `outTexcoord[]` scratch size/literal span/zero-init
    bytes, plus `VS Buffer Bytes/Invocation` to dumped-MSL-`VSOut` ratio and
    unread `VSOut` byte share. This lets Xcode VS buffer-write traffic be
    compared against both the translated output shape, source-visible local
    scratch, and the FS-visible liveness without reopening Xcode.
    For the current VS-buffer-write hypothesis, run a paired candidate with
    `--trim-unused-varyings`; this sets `DXMT9_TRIM_UNUSED_VARYINGS=1` so the
    pair-local VSOut liveness path is active. Compare it to the baseline joined
    CSV with `--baseline-joined <csv> --require-top-vs-buffer-write-decrease`
    to prove whether trimming moves the Xcode VS buffer-write counter.
    If the broad trim does not move Xcode's bucket, use
    `--drop-vsout-point-size` for a narrower pipeline-shape A/B. This sets
    `DXMT9_PROBE_DROP_VSOUT_POINT_SIZE=1` and removes only
    `VSOut.pointSize [[point_size]]` while preserving texcoords/color/fog, so
    any counter movement is attributable to the Metal point-size path rather
    than ordinary FS liveness.
    If live VSOut and point-size probes still leave Xcode's bucket unchanged,
    use `--probe-position-only-vsout` as a correctness-invalid lower-bound
    diagnostic. This sets `DXMT9_PROBE_POSITION_ONLY_VSOUT=1`, forces a
    position-only VSOut layout, and makes translated/FFP fragment shaders
    return a constant color so the reduced stage-in shape can compile. Accept
    it only as evidence about whether visible stage-out shape can move
    `VS Buffer Device Memory Bytes Written`.
    To separate the constant-fragment side of that probe from the VSOut-layout
    side, use `--force-fragment-color`. This sets
    `DXMT_DEBUG_FORCE_FRAGMENT_COLOR=1`, keeps the current VSOut layout, and
    forces translated/FFP fragment shaders to return a constant color. Compare
    it against the same baseline before attributing position-only movement to
    VSOut width.
    If `--force-fragment-color` moves Xcode's VS-write counters, use
    `--disable-alpha-test` as the next narrower classifier. This sets
    `DXMT_DISABLE_ALPHA_TEST=1`, keeps the normal fragment shader body apart
    from the alpha-test branch, strips the generated `discard_fragment()` path,
    and also forces `FfpPsConsts.alphaTestEnable = 0`. Treat it as a
    correctness-invalid discard/raster backend probe, not as an optimization.
    Use `--disable-fog` as the matching fog-source classifier. This sets
    `DXMT_DISABLE_FOG=1`, keeps the normal fragment shader body and VSOut
    shape, but strips generated fog blending from translated, FFP, and
    tile-FFP paths. Treat it as a correctness-invalid fog/raster backend
    probe.
    Use `--force-texture-white` as the matching texture-source classifier.
    This sets `DXMT_FORCE_TEXTURE_WHITE=1`, keeps the normal fragment shader
    body and VSOut shape, but replaces generated fragment texture sample
    results with `float4(1.0f)`. Treat it as a correctness-invalid
    texture/raster backend probe.
    If VSOut trimming leaves Xcode's VS buffer-write bucket unchanged, run the
    next paired candidate with `--trim-vertex-temps`; this sets
    `DXMT9_TRIM_VERTEX_TEMPS=1` so translated VS `float4 r[]` is sized from
    observed temp source/dest usage instead of the conservative 32-slot array.
    Keep this as an experiment until a shader-corpus run and gputrace A/B prove
    it does not reproduce the older VS trim visual regression.
    If that also leaves Xcode's VS buffer-write bucket unchanged, run the next
    paired candidate with `--trim-vs-output-scratch`; this sets
    `DXMT9_TRIM_VS_OUTPUT_SCRATCH=1` so translated VS `float4 outTexcoord[]`
    is sized from emitted/mapped texcoord output usage instead of the
    conservative 8-slot local scratch array. Gate it the same way with
    `--baseline-joined <csv> --require-top-vs-buffer-write-decrease` and keep
    shader dumps enabled so the `VS outT[]` columns show whether the source
    shape actually changed.
    After Xcode exports
    `analysis/frame<N>-counters-xcode.csv`, run
    `scripts/tools/finalize_3dmark05_perf_probe.sh --suffix <suffix> --frame <N>`.
    It regenerates the dxmt summary, joins the Xcode-only summary with dxmt
    encoder attribution, writes the Markdown bottleneck report, and optionally
    runs baseline comparisons. The report includes VS/FS buffer-write split,
    texture and depth writes, tiled vertex/primitive-block bytes, stream/IB
    churn, cbuf bytes and cbuf class split, transient bytes, VS-write density,
    VS L1/LLC write, primitive/tile counters, cull/clip/shaded-vertex-read
    limiter shape, and PSO/shader-variant/VS/PS hash/VSOut attribution. The
    report also includes a DXMT encoder writer/state table
    with stream handle/offset/stride churn, IB handle churn, argbuf table
    bytes, cbuf bytes split by VS/FFPVS/PS/FFPPS, `setVertexBytes`,
    transient vertex/index bytes, writer-to-buffer ratio, stream0 input-byte
    bounds, VS-buffer-to-stream0-input ratio, and VS-invocation-to-dxmt-vertex
    ratio per top encoder. When `3dmark05-perf-encoder-streams.csv` is
    present, it also embeds a per-stream table for top encoders so stream
    handle/offset/stride churn can be attributed to the exact stream slot
    without opening a separate CSV.
    The joined CSV also derives `dxmt_cpu_writer_bytes`,
    writer-to-Xcode-buffer-write ratios,
    `dxmt_vs_buffer_write_share`,
    `dxmt_unexplained_buffer_write_mib`,
    `dxmt_unexplained_buffer_write_ratio`, per-draw stream/IB churn rates,
    `dxmt_vs_buffer_bytes_per_dxmt_vertex`, decoded `dxmt_vsout_*` layout
    fields, VS-buffer-to-expected-stage-out ratio,
    `dxmt_vsout_layout_cache_hits` / `dxmt_vsout_layout_cache_misses`, and
    `dxmt_vertex_shader_last` / `dxmt_pixel_shader_last` shader hashes,
    `dxmt_vertex_shader_source_last` / `dxmt_pixel_shader_source_last`
    shader-source hashes,
    plus `dxmt_gpu_write_hint` / `dxmt_write_owner_confidence` so a run can
    distinguish dxmt upload pressure from unexplained GPU-side VS buffer-write
    pressure without reopening Xcode.
    `dxmt_pso_state_samples_per_draw` should be near `1.0` on logs produced
    after the DrawRun attribution update; lower values indicate an older log
    where PSO/VSOut attribution was sampled only on base-state binds. Add
    `--encoder-breakdown-seq <N>` for targeted frame probes when the full
    encoder breakdown log would become too large; this sets
    `DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=<N>` and keeps only matching
    `RenderPass[seq=<N>,...]` rows while preserving the run-level perf
    counters. Use it for quick no-gputrace validation of a known frame such as
    `--encoder-breakdown-seq 60`; omit it for whole-run attribution or when
    comparing top encoders across arbitrary frames.
    Add
    `--require-xcode-counter-coverage --require-dxmt-join-coverage
    --require-top-pso-attribution` to the finalizer when the run is intended to
    prove a shader/VSOut root cause. The first gate fails incomplete Xcode
    exports that lack the required GPU counter columns or
    `RenderPass[seq=...,enc=...]` labels; the second fails when top Xcode rows
    do not join to dxmt encoder attribution; the third fails if top-encoder
    `dxmt_pso_state_samples / dxmt_draw_calls` is below `0.90` by default.
    Compare candidate fixes by passing `--baseline-joined
    <baseline-joined.csv>` to the wrapper or finalizer. Add requirement flags
    such as `--require-top-gpu-decrease`,
    `--require-top-buffer-write-decrease`,
    `--require-top-vs-buffer-write-decrease`,
    `--require-top-unexplained-buffer-write-decrease`,
    `--require-stream-handle-churn-decrease`, or
    `--require-ib-handle-churn-decrease` so Xcode counter regressions fail the
    comparison automatically. The wrapper rejects these Xcode comparison gates
    unless `--baseline-joined` is present, and the finalizer does the same,
    because otherwise there is no before/after CSV to compare.
    Use `--max-top-unexplained-buffer-write-ratio N` when a candidate is
    expected to make Xcode buffer writes explainable by dxmt writers; it fails
    if the residual top-encoder write ratio remains above `N`.
    Compare run-level mechanisms such as store-action policy, same-key
    preservation bytes, draw-run formation, and queue waits with
    `scripts/tools/finalize_3dmark05_perf_probe.sh --baseline-output
    <baseline-output-dir>`. For DontCare-store experiments, add
    `--require-color-dontcare-increase`,
    `--require-depth-dontcare-increase`, or
    `--require-tile-preservation-decrease` so the comparison exits nonzero
    when the intended mechanism did not move. The probe wrapper can also run
    this run-level comparison automatically after the candidate run with
    `--compare-baseline-output <baseline-output-dir>` plus the same gates.
    For draw-run and CPU encode experiments, add
    `--require-draw-run-records-increase`,
    `--require-draw-run-records-per-submit-increase`,
    `--require-binding-overrides-present`,
    `--require-const-upload-passthrough-present`, or
    `--require-encode-draw-cpu-decrease` so the run-level comparison proves the
    intended batching mechanism moved before treating the run as a perf result.
    For sparse/coalesced constant-upload experiments, add
    `--require-const-upload-break-bytes-decrease` and
    `--max-const-upload-break-count-ratio <N>` to require fewer const-upload
    bytes while bounding record-count churn.
    The same comparison report now derives const-upload breaks per draw/present,
    const-upload break bytes per draw/present/break, registers per break,
    const-upload passthrough per draw/present, const-upload-to-state-delta
    ratio, state-delta subtype/pair shares, stream/IB deltas per draw, and
    VS/PS F/I/B subtype count/byte shares plus subtype coverage, so
    a constant-upload coalescing candidate can be checked without hand-dividing
    the raw counters.
    For a paired sparse-constant candidate, add
    `--split-sparse-const-records`; it sets
    `DXMT9_SPLIT_SPARSE_CONST_RECORDS=1`, which keeps default merged-constant
    behavior off the baseline but splits an opt-in dirty const range into
    actual changed register runs when the app updates sparse registers.
    For the R32F render-target compression hypothesis, add
    `--suppress-rt-pixel-format-view`; it sets
    `DXMT9_SUPPRESS_RT_PIXEL_FORMAT_VIEW=1`, which keeps the default D3D9
    shader-read swizzle path intact but, for opt-in probes only, stops R32F
    render targets from requesting `PixelFormatView` and creating the swizzled
    shader-read view. Validate with Xcode encoder counters before considering
    any broader policy change.
    For the X8 render-target compression hypothesis, add
    `--suppress-x8-rt-pixel-format-view`; it sets
    `DXMT9_SUPPRESS_X8_RT_PIXEL_FORMAT_VIEW=1`, which applies the same
    diagnostic policy to `X8R8G8B8`/`X8B8G8R8` render targets. This is more
    correctness-risky than the R32F probe because X8 shader reads depend on
    the D3D alpha-fill contract; use it only to classify Xcode
    lossless-compression and texture/pass-store counters.
    Pair it with `--x8-shader-alpha-fill` when testing whether the alpha-fill
    contract can be preserved in shader code while the Metal texture view is
    suppressed; this sets `DXMT9_X8_SHADER_ALPHA_FILL=1` and creates PSO
    variants for active X8 fragment samplers.
    Run-level comparison gates require `--compare-baseline-output` in the
    wrapper or `--baseline-output` in the finalizer; the scripts reject them
    without a baseline and validate that the baseline resolves to an existing
    `result.json` before launching or finalizing.
    The run summary also reports draw-run binding override counters
    (`commit_chunk_draw_run_binding_override_*`) so stream/IB per-draw payloads
    can be separated from remaining draw-run break causes. It also reports
    `commit_chunk_draw_batch_const_upload_passthrough`, which counts
    constant-upload records crossed without flushing the pending draw
    submission batch. Pair it with
    `--require-draw-submission-batch-present` after adding a baseline to ensure
    `commit_chunk_draw_submission_batch_{submits,records,max_records}` are
    populated and the fallback batch size can be judged from the report.
    Render-pass
    load/store action, same-key re-entry/preservation, transition, and depth
    store-proof counters are grouped in the same summary so store-traffic
    changes can be checked without reopening Xcode. Color store proof counters
    also distinguish the default next-clear win from the opt-in
    `DXMT9_AGGRESSIVE_COLOR_DONTCARE=1` dead-at-end experiment.
    The probe wrapper has `--aggressive-color-dontcare` and
    `--aggressive-depth-dontcare` switches for paired baseline/candidate runs.
    For hidden vertex/tiler/backend state-shape probes, use the narrow
    diagnostic switches before broad visibility changes:
    `--probe-disable-alpha-blend` sets
    `DXMT9_PROBE_DISABLE_ALPHA_BLEND=1` and disables Metal color blending while
    preserving color-write masks, and `--probe-disable-depth-write` sets
    `DXMT9_PROBE_DISABLE_DEPTH_WRITE=1` and keeps depth tests but forces depth
    writes off. Gate both with Xcode `VS Buffer Device Memory Bytes Written`
    deltas; they are not correctness-preserving optimizations by themselves.
    `--force-cull-mode none|front|back` sets
    `DXMT_DEBUG_FORCE_CULL_MODE` and is the narrow cull/backend shape classifier
    to use when broad `--disable-cull` has already been rejected. Pair it with
    Xcode counters and check whether `VS Invocations`, `VS B/invocation`, or
    named tiled counters move.
    `--force-expand-indexed` sets `DXMT_FORCE_EXPAND_INDEXED=1`; it preserves
    indexed geometry intent but changes vertex submission/cache behavior, so use
    it only as a primitive/backend pressure classifier and expect possible CPU
    and GPU regressions.
    `--disable-alpha-test` is the narrower fragment/raster classifier for the
    alpha-test discard path and should be tried before more invasive shader
    substitutions when `--force-fragment-color` changes hidden VS-write
    counters. `--disable-fog` is the matching classifier for the fog blend /
    fog-factor read path, and `--force-texture-white` isolates texture sample
    results without removing the rest of the fragment body.
    A solid yellow/clear-like GT1 frame from the alpha-blend probe is expected
    evidence that the probe invalidated final pixels, not a valid target state.
  Explicit environment variables still override profile defaults.
- Launcher filename must exactly match `CATALOGUE.name`. Adding a new app:
  1. Add `[[app]]` entry to `experiments/CATALOGUE.toml` with `name`,
     `binary`, `launcher`, `reference` (optional), `features`, `status`.
  2. Create `experiments/launchers/<name>.sh` (sources `common.sh`).
  3. (Optional) Create `scripts/build_apps/build_<name>.sh` and add
     `build_script` field to CATALOGUE for `--build` support.
- Synthetic probes share the same probe binary
  (`apps/perf-d3d9-probe`, `apps/perf-d3d9-bridge-empty`, etc.) — the launcher
  injects different env vars / args.
- Run via:
  ```
  python3 scripts/run_apps/run_experiment.py run <name>
  DXMT_EXPERIMENT_PROFILE=perf python3 scripts/run_apps/run_experiment.py run <name>
  python3 scripts/run_apps/run_experiment.py run <name> --build
  ```
