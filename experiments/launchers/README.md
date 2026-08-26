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

- `app-d3d9-3dmark05.sh` — 3DMark05 (external prefix; GT1/GT2/GT3).
- `app-d3d9-3dmark06.sh` — 3DMark06 (external payload; SM2 GT1/GT2 and
  HDR/SM3 HDR1/HDR2).
- `app-d3d9-sfiv-benchmark.sh` — SFIV benchmark (Heroic + CrossOver
  oracle lanes (the CrossOver oracle wrapper was removed on 2026-07-29;
  `agents/rules/test_wild.rules.md` rejects CrossOver as a runtime)
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
    is intentionally off by default. Add
    `DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE=1` only after that axis is
    implicated; it builds a cache-aware LRU32 candidate order without
    submitting it and reports original-vs-candidate LRU16/32/64 miss deltas.
    It is intentionally not enabled by the shared perf profile. For a mutating
    A/B after the no-mutate scout, use
    `DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE=1` or the wrapper's
    `--probe-apply-index-cache-opt-candidate`; it submits that same candidate
    only through row/draw/class/span, opaque-depth-write, stable-IB, and
    minimum-LRU32-gain gates, then reuses a source-IB keyed reordered index
    buffer cache rather than uploading a fresh transient IB per draw. When
    finalizing a mutating Xcode A/B, pass each mutated row with
    `--target-row-key`. Use `--require-cache-opt-apply-proof` only for
    diagnostic apply paths that emit finite generic actual indexed telemetry;
    that preset expands to the stable frame proof plus
    `--require-target-index-cache-miss32-decrease
    --require-target-vs-buffer-write-decrease
    --require-target-vs-invocations-decrease`, and now rejects target rows
    whose actual indexed reference/unique/LRU telemetry is missing or
    nonpositive. For production cached-IB opt-ins, where the submitted
    reordered-buffer proof is represented by cache-hit counters and the
    generic indexed telemetry can be `0`, use
    `--require-target-reordered-index-cache-hits` with the VS write/invocation
    gates. If the run also emits after-side candidate/effective LRU telemetry
    for the applied cached prelookup rows, add
    `--require-target-index-cache-opt-miss32-decrease` as well. For
    diagnostic-only depth-read/blended A/B probes, add
    `--probe-apply-index-cache-opt-candidate-unsafe-nonopaque` only with tight
    row/class filters and same-input semantic replay validation; it bypasses
    the opaque-depth-write safety gate and can change final color writers. Use
    `python3 scripts/tools/compare_experiment_images.py --before
    <baseline>/actual.png --after <candidate>/actual.png --crop-bottom 96
    --output <trace-run>/analysis/<name>-image-compare.md` as a screenshot
    sanity check when a visual regression is suspected. Treat cross-run image
    comparison as a frame/time-drift detector unless the two captures are known
    to represent the same presented frame; exact correctness proof still needs
    a same-run replay or draw-local comparison. After the run,
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
  - `app-d3d9-3dmark06` follows the same bounded-run convention without the
    3DMark05-only direct/probe wrapper. Its default arguments are
    `-gt1 -nosplash -nosysteminfo -noscreens`; set `DXMT_3DMARK06_ARGS` to
    select `-gt2`, `-hdr1`, or `-hdr2`, and set
    `DXMT_3DMARK06_RESULT_FILE=<name>.3dr` to append the result file as the
    final positional argument. Most command-line test-selection switches are
    a 3DMark06 Professional Edition facility; an installed Basic or Advanced
    edition must not be treated as a valid per-test lane until its own CLI
    behavior is observed. The switch definitions are recorded in Futuremark's
    [3DMark06 whitepaper](https://s3.amazonaws.com/download-aws.futuremark.com/3DMark06_Whitepaper.pdf).
    Use `DXMT_3DMARK06_DRY_RUN=1 bash
    experiments/launchers/app-d3d9-3dmark06.sh` to inspect the plan without
    staging or launching Wine. The unlocked-session guard defaults on, while
    the still-unqualified Enter fallback defaults off; enable it explicitly
    with `DXMT_3DMARK06_AUTO_ENTER=1` only if the installed edition needs UI
    confirmation.
  - `app-d3d9-3dmark05` is timeout-tolerant in the catalogue
    (`run_timeout_sec=180`, `allow_timeout=true`, `require_positive_timeout=true`)
    because the app can hang on the final frame after useful output is already
    present. Routine runs should use
    `python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05` with the
    catalogue timeout, or pass an explicit positive `--timeout N`; `--timeout 0`
    is rejected for this app. If
    `experiments/launchers/app-d3d9-3dmark05.sh` is started directly from a
    shell without `run_experiment.py`, it now self-supervises with a positive
    launcher timeout (`DXMT_3DMARK05_LAUNCHER_TIMEOUT`, default `180s`; set
    `DXMT_3DMARK05_ALLOW_UNSUPERVISED=1` only for a deliberately external
    supervisor). For verify-prefix direct runs, use
    `scripts/run_apps/run_app-d3d9-3dmark05-verify_direct.sh`; it defaults to
    `DXMT_3DMARK05_DIRECT_TIMEOUT=180` and supports
    `DXMT_3DMARK05_DIRECT_DRY_RUN=1` to verify the resolved command without
    starting Wine. The direct launcher traps `TERM`/`INT` and kills the app
    prefix wineserver on exit by default
    (`DXMT_3DMARK05_KILL_SERVER_ON_EXIT=1`), preventing detached final-frame
    Wine processes after timeout.
  - `scripts/tools/run_3dmark05_perf_probe.sh` wraps the current GT1 perf probe
    recipe: perf profile, direct 3DMark05 launcher, no auto indexed expansion,
    encoder breakdown, optional `DXMT_METAL_CAPTURE_FRAME/PATH`, trace output
    under `traces/app-d3d9-3dmark05-<suffix>/`, and automatic
    `3dmark05-perf-summary.md` / CSV generation after the run. Use `--dry-run`
    first to verify paths, desktop lock state, free-space guard, and the
    downstream `run_experiment.py --timeout`. Because 3DMark05 can hang on the
    final frame, the wrapper always uses a positive runner timeout: `420s` by
    default with gputrace and `180s` with `--no-gputrace`, unless `--timeout`
    or `DXMT_3DMARK05_PROBE_TIMEOUT` overrides it. It also wraps the full
    `caffeinate run_experiment.py ...` command in a top-level watchdog at
    timeout plus `DXMT_3DMARK05_PROBE_TIMEOUT_SLACK` (default `45s`), so a
    detached final-frame Wine process is still terminated and available logs are
    postprocessed. The wrapper scopes encoder breakdown to the requested frame
    by default for gputrace runs and for
    no-gputrace indexed diagnostics
    (`DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=<frame>`). This keeps expensive
    index-reuse/cache-opt diagnostics from slowing earlier GT1 frames enough
    to change the semantic workload selected by `seq/enc` row keys. Use
    `--encoder-breakdown-all-frames` only when intentionally collecting
    whole-run encoder diagnostics. For no-gputrace run-level/default-policy
    smokes that should avoid per-encoder diagnostic overhead entirely, use
    `--no-encoder-breakdown`; do not use it for gputrace/Xcode proof runs,
    where joined encoder rows are required. The wrapper requires `2048MiB`
    free by default when gputrace capture is enabled
    (`--min-free-mb N` / `DXMT_3DMARK05_MIN_TRACE_FREE_MB=N` overrides it).
    It does not set `MTL_CAPTURE_ENABLED=1` by default because that env can
    black-screen 3DMark05 before draw/present; use
    `DXMT_3DMARK05_SET_MTL_CAPTURE_ENABLED=1` only for capture-layer
    experiments, and
    `DXMT_3DMARK05_METAL_CAPTURE_DESTINATION=developerTools` for attached-Xcode
    capture-route experiments.
    Gputrace runs refuse lower guards unless
    `DXMT_3DMARK05_ALLOW_LOW_TRACE_FREE_MB=1` is set deliberately; low-space
    captures can still export Xcode counters, but they risk Wine state-save
    failures and missing `result.json`, so they should not be used as strict
    proof runs. Add `--require-stable-frame-proof` to proof captures so the
    printed finalizer command rejects partial logs, requires Xcode/dxmt counter
    coverage and PSO attribution, requires top row-key match, requires top
    GPU/VS/unexplained write decreases, and applies default `0.05` top
    draw/vertex/triangle drift gates. Use the lower-level
    `--require-result-json`, `--require-top-row-key-match`, and
    `--max-top-*-delta-ratio` flags only when a probe needs a custom gate.
    Finalization also emits
    `analysis/frame<N>-indexed-state-class-xcode-proxy.{md,csv}` when indexed
    probe draw telemetry is present, using the joined Xcode counters to rank
    row/state/class candidates by proxy hidden-backend bytes. The default
    queue length is `12`; override it with `--class-proxy-top N` or
    `DXMT_3DMARK05_CLASS_PROXY_TOP=N`.
    Use `--measure-index-reuse` for the optional unique-index diagnostic; the
    final joined report will include `dxmt indexed references / unique
    estimate`, `VS invocations / dxmt indexed unique estimate`, and 16/32/64
    finite-cache miss estimates when the index data was readable. Add
    `--dump-indexed-geometry` only for row-local mini replay prep; it implies
    `--measure-index-reuse` and writes capped raw index/stream0 payload files
    under `traces/<run-id>/analysis/geometry` using the same reverse-indexed
    row/class/span filters and indexed encoder draw range.
    Dry-run and guard failures print `traces/`, `experiments/output/`, and
    the largest trace/output files when free space is below the guard, plus
    the largest `traces/app-d3d9-3dmark05-*` and
    `experiments/output/app-d3d9-3dmark05-*` run directories. Use those run-id
    groups as the first manual cleanup candidates before launching Wine, after
    preserving any analysis artifacts still needed by
    `docs/perfomance/`. They also print large ignored prefix/app/vendor
    payloads as manual-review candidates; do not delete those blindly because
    they may be the active Wine prefix or installed benchmark payload. If
    `--baseline-joined`,
    `--require-stable-frame-proof`, or `--require-top-pso-attribution` is
    passed to the wrapper, dry-run also prints the exact
    `finalize_cmd_after_xcode_export` command to run after Xcode exports
    `frame<N>-counters-xcode.csv`. When a mutating primitive-order probe also
    has same-input mini-replay PPMs, add `--semantic-image-policy exact|lsb1
    --semantic-image-before <original.ppm> --semantic-image-after
    <candidate.ppm>` to the wrapper or finalizer. The finalizer then runs
    `compare_experiment_images.py`, writes a policy report/CSV/diff under
    `analysis/`, and enforces a `1%` before/after active-pixel floor by
    default so all-clear replays cannot satisfy the image gate.
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
    `--probe-half-vsout` sets `DXMT9_PROBE_HALF_VSOUT=1` and requests `half4`
    for color/secondary/texcoord VSOut fields plus `half` for fogFactor, while
    keeping `position`, `[[point_size]]`, and `[[clip_distance]]` as float.
    This is a mechanism probe for the shader spec's per-output VSOut precision
    hypothesis; gate it with Xcode counters and semantic images before treating
    it as more than a backend-shape classifier.
    Use `--force-fragment-color` as a constant-fragment control. This sets
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
    For a row/class-scoped version, use `--probe-force-texture-white-row`
    with `--probe-force-texture-white-classes`, for example
    `--probe-force-texture-white-row 50/2 --probe-force-texture-white-classes depth-read,screen-blend,textured`.
    This builds a separate shader-source variant only for selected indexed
    triangle-list draws and records `probe_force_texture_white_draws` in the
    encoder breakdown.
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
    because otherwise there is no before/after CSV to compare. When comparison
    gates are requested, the Markdown comparison report includes a
    `Requirement Status` section, plus `Requirement Failures` when a gate
    fails, so the reduced `traces/.../analysis` artifact remains
    self-contained even if stderr is no longer available.
    Use `--max-top-unexplained-buffer-write-ratio N` when a candidate is
    expected to make Xcode buffer writes explainable by dxmt writers; it fails
    if the residual top-encoder write ratio remains above `N`.
    For primitive-order, visibility, or backend-shape classifiers, also add
    `--require-top-row-key-match` and bounded shape drift gates such as
    `--max-top-draw-call-delta-ratio 0.05`,
    `--max-top-vertex-count-delta-ratio 0.05`, and
    `--max-top-triangle-delta-ratio 0.05`. These fail comparisons where the
    top `RenderPass[seq=...,enc=...]` set, draw count, or submitted geometry
    changes enough that an Xcode VS-buffer-write delta could be a different
    frame shape instead of the candidate mechanism.
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
    preserving color-write masks. Prefer the scoped
    `--probe-disable-alpha-blend-row`, `--probe-disable-alpha-blend-rows`,
    `--probe-disable-alpha-blend-class`, and
    `--probe-disable-alpha-blend-classes` variants for hot-row A/B; unlike the
    global switch they do not set `DXMT9_PROBE_DISABLE_ALPHA_BLEND=1` and only
    build blend-off PSO variants for matching indexed triangle draws.
    `--probe-disable-depth-write` sets
    `DXMT9_PROBE_DISABLE_DEPTH_WRITE=1` and keeps depth tests but forces depth
    writes off. Gate both with Xcode `VS Buffer Device Memory Bytes Written`
    deltas; they are not correctness-preserving optimizations by themselves.
    `--force-cull-mode none|front|back` sets
    `DXMT_DEBUG_FORCE_CULL_MODE` and is the narrow cull/backend shape classifier
    to use when broad `--disable-cull` has already been rejected. Pair it with
    Xcode counters and check whether `VS Invocations`, `VS B/invocation`, or
    hidden backend write move. Named tiled counters are subtype evidence, not
    the primary gate for hidden-expanded TVB/parameter storage.
    `--probe-force-cull-mode none|front|back` sets
    `DXMT9_PROBE_FORCE_CULL_MODE` and should be preferred for hot-row
    backend-shape A/B because it can be constrained by
    `--probe-force-cull-mode-row`, `--probe-force-cull-mode-rows`,
    `--probe-force-cull-mode-class`, and
    `--probe-force-cull-mode-classes`. Use the same row/geometry gates as
    split/reverse probes and confirm the target row's `cull n/f/b` bucket
    changes in the dxmt/Xcode joined report.
    `--force-expand-indexed` sets `DXMT_FORCE_EXPAND_INDEXED=1`; it preserves
    indexed geometry intent but changes vertex submission/cache behavior, so use
    it only as a primitive/backend pressure classifier and expect possible CPU
    and GPU regressions.
    `--split-large-indexed-draws N` sets
    `DXMT9_SPLIT_LARGE_INDEXED_DRAWS=N`; it splits large indexed triangle-list
    draws into multiple Metal indexed draws while preserving the original index
    path. This is a backend/tiler pressure classifier, not a production
    optimization by itself because it increases Metal draw count.
    `--split-large-indexed-draws-row SEQ/ENC` and
    `--split-large-indexed-draws-rows ROWS` constrain that split probe to one
    Xcode/DXMT render encoder row or a comma/semicolon/space separated row set.
    `--split-large-indexed-draws-class CLASS` further limits it to one indexed
    triangle state bucket. Accepted classes are `any`, `opaque-depth-write`,
    `nonopaque`, `depth-read`, `alpha-blend`, `no-alpha-blend`,
    `screen-blend`, `standard-alpha`, `additive-alpha`, `scissor`,
    `no-scissor`, `textured`, and `large4096`. Use these filters for bounded
    primitive-partition probes after broad full-frame split/reverse runs have
    been rejected by shape gates.
    `--probe-reverse-indexed-triangles` sets
    `DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES=1`; it keeps indexed draw count and
    render state stable while submitting a transient IB with triangle-list
    primitive order reversed. Use it as an index-locality/backend classifier,
    not as an optimization or correctness target.
    `--probe-reverse-opaque-indexed-triangles` sets
    `DXMT9_PROBE_REVERSE_OPAQUE_INDEXED_TRIANGLES=1`; it limits the same
    transient-IB reversal to solid, depth-writing, non-blended,
    non-alpha-tested, non-stencil triangle-list draws so blended visibility
    stays intact during narrower backend-locality probes.
    `--probe-reverse-nonopaque-indexed-triangles` sets
    `DXMT9_PROBE_REVERSE_NONOPAQUE_INDEXED_TRIANGLES=1`; it applies the same
    reversal only outside that opaque depth-writing subset. Use it to isolate
    whether full reverse-order wins come from blended, depth-write-off, or
    other visibility-sensitive rows.
    `--probe-reverse-indexed-triangles-row SEQ/ENC` sets
    `DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_ROW=SEQ/ENC`; it constrains any
    reverse-indexed-triangle probe to one `RenderPass[seq=...,enc=...]` row so
    row-scoped A/B runs can keep hot-row membership stable enough for
    attribution. Pair row-scoped reverse probes with the top-row and geometry
    drift gates above; if those gates fail, keep the run as a classifier only
    and do not promote the result as a correctness-preserving optimization.
    `--probe-reverse-indexed-triangles-rows ROWS` sets
    `DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_ROWS=ROWS`; it accepts a
    comma/semicolon/space separated row set such as `60/0,60/1,60/3,60/4`.
    Use it after single-row probes fail to move the hidden VS-write bucket, so
    the full hot-row set can be tested without broad full-frame primitive-order
    changes.
    `--probe-reverse-indexed-triangles-class CLASS` sets
    `DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_CLASS=CLASS` and constrains any
    reverse-indexed-triangle probe to the same state buckets used by
    `--split-large-indexed-draws-class`: `any`, `opaque-depth-write`,
    `nonopaque`, `depth-read`, `alpha-blend`, `no-alpha-blend`,
    `screen-blend`, `standard-alpha`, `additive-alpha`, `scissor`,
    `no-scissor`, `textured`, and `large4096`. Combine it with a row selector
    for material-scoped visibility/backend probes such as `60/4` alpha-blend or
    scissor subsets.
    Pair reverse/order probes with `--measure-index-reuse` when investigating
    Apple vertex/tiler backend pressure: the generated
    `3dmark05-perf-indexed-probe-draws.csv` then includes per-draw original
    and effective index locality, cache-miss estimates, triangle index span,
    and stream0 byte-span proxies. Use
    `scripts/tools/analyze_indexed_probe_classes.py --row SEQ/ENC --group
    row-state-class` on that CSV before selecting a narrow AND class filter.
    The report exposes blend/scissor/depth/cull/fill buckets such as
    `depth-read,no-alpha-blend,no-scissor,textured`, plus semantic risk,
    proof family, preflight gate, and Xcode replay gate fields when an Xcode
    joined summary is supplied. Use
    `--measure-index-cache-opt-candidate` for no-mutate full-frame scouts when
    mini replay shows post-transform cache locality is predictive; the encoder
    breakdown then includes original-vs-candidate cache-opt miss estimates for
    the joined Xcode/dxmt comparison. With `--encoder-breakdown-seq`, the same
    CSV is also emitted without a mutating reverse/split/scissor probe, so a
    no-mutate scout can capture draw identity, state, stream/IB handles, PSO,
    shader variant, VS/PS hashes, and VSOut key for row-local replay planning.
    Use `--probe-apply-index-cache-opt-candidate` for the follow-up mutating
    A/B once the no-mutate candidate and row filters are known; it keeps the
    same candidate counters enabled so the submitted order and predicted LRU32
    miss reduction can be joined against Xcode `VS Invocations` and
    `VS Buffer Device Memory Bytes Written`. Keep this safe path scoped to
    opaque depth-writing rows. After the row-local proof is established, use
    `--optimize-opaque-depth-index-cache` for the production-shaped opt-in path
    (`DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1`). This submits the same
    cached LRU32 reordered IBs only for opaque depth-writing triangle lists and
    never bypasses the safety gate. This production opt-in is deliberately
    independent of diagnostic reverse-triangle row/class/span filters, so stale
    probe environment variables cannot silently narrow the accepted path. It is
    still explicitly opt-in: the shared `perf` profile does not enable mutating
    index-cache optimizations, and a general runtime default would require
    wider app correctness evidence beyond the accepted 3DMark05 GT1 gates. The
    runtime cache records both positive
    reordered-IB entries and rejected keys; the rejected entries are important
    because failed gain-gate candidates must not be remeasured every draw in a
    full GT1 run. Read `reordered_index_cache_hits` as positive cached-IB hits
    and `reordered_index_cache_rejected_hits` as rejected-key hits; the latter
    should not be counted as submitted reordered geometry. Unscoped production
    opt-in full runs keep aggregate encoder counters but suppress per-draw
    `dxmt9-perf-indexed-probe-draw` lines. The production path uses LRU32-only
    candidate measurement because the gain gate needs only LRU32; full
    16/32/64 + unique/span locality telemetry remains diagnostic-only. Read
    `indexed_cache_opt_candidate_*_miss32` for production accounting; miss16
    and miss64 are intentionally zero on fast production runs. When indexed
    locality/reorder or geometry-dump diagnostics are enabled, the wrapper now
    auto-scopes encoder breakdown to `--frame` even for `--no-gputrace` smoke
    runs. Use
    `--encoder-breakdown-all-frames` only for deliberate whole-run sampling,
    or `--no-encoder-breakdown` only for no-gputrace run-level/default-policy
    smokes that intentionally skip per-encoder diagnostics.
    In the finalizer for production-shaped
    `--optimize-opaque-depth-index-cache` runs, use `--target-row-key` plus
    `--require-opaque-depth-index-cache-proof`. That preset includes
    stable-frame gates, target cache-opt candidate/effective LRU32 decrease,
    positive reordered-cache hits, and target VS buffer write/invocation
    decreases, and it enables index-reuse/cache-opt-candidate telemetry in the
    wrapper. Keep `--require-cache-opt-apply-proof` for diagnostic
    `--probe-apply-index-cache-opt-candidate` runs where the generic actual
    LRU32 telemetry is the intended target-row gate. Use
    `--probe-apply-index-cache-opt-candidate-unsafe-nonopaque` only for
    diagnostic depth-read/blended/scissored rows after the opaque path is
    understood, because the primitive order can affect final color writers.
    When this unsafe path is paired with `--require-cache-opt-apply-proof`,
    the wrapper also requires `--semantic-image-policy` with before/after
    images and passes `--require-semantic-image-proof` to the finalizer. Use
    explicit target-row Xcode gates instead when the goal is only a
    performance-mechanism measurement.
    `--optimize-screen-blend-index-cache` is separate from that accepted opaque
    opt-in path. It is a profiling-only opt-in for strict screen-blend rows:
    force-color same-input replay can prove raster/depth-open coverage, but
    translated-FS replay has shown small bit-exact output differences from
    destination-dependent blend ordering. Treat combined opaque+screen-blend
    runs as performance ceilings or mechanism checks, not production-safety
    proof runs. When a screen-blend cache run is meant to be interpreted as a
    proof, use `--require-screen-blend-cache-proof` instead of only
    `--require-cache-opt-apply-proof`; it adds stable-frame gates, target
    cache-opt candidate/effective telemetry, reordered-cache-hit, target VS
    write/invocation, and same-input semantic image gates, and refuses to run
    without at least one `--target-row-key` and `--semantic-image-policy` plus
    before/after mini-replay images. The target row is required so a
    screen-blend proof cannot pass on semantic tolerance alone without proving
    the specific Xcode row's VS invocation/write movement.
    The wrapper also requires `--optimize-screen-blend-index-cache` for this
    preset and enables `DXMT9_MEASURE_INDEX_REUSE=1` plus
    `DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE=1`, because the proof compares
    cache-opt candidate/effective LRU32 telemetry and Xcode VS counters; the
    generic actual-indexed LRU estimate can remain an original-index locality
    measure for this opt-in path.
    For primitive-order-preserving backend-shape experiments, add
    `--require-tvb-mechanism-proof` to the wrapper once a joined Xcode
    baseline is available. The wrapper forwards it to the finalizer so the
    candidate must reduce top hidden backend write, Xcode VS buffer write, VS
    invocations, and GPU time before it can be treated as a
    TVB/parameter-backend mechanism proof. Named tiled-buffer bytes should still
    be inspected as subtype evidence, but they are not sufficient as the main
    gate for hidden-expanded storage.
    Combine that CSV with a joined Xcode/dxmt summary and shader-dump summary
    using `python3 scripts/tools/plan_3dmark05_mini_replay.py --joined
    <frameN-xcode-dxmt-joined-summary.csv> --shader-summary
    <frameN-shader-dump-summary.csv> --probe-draws
    <3dmark05-perf-indexed-probe-draws.csv> --output
    <frameN-mini-replay-readiness.md>`. Add `--geometry-dir
    <trace-run>/analysis/geometry` after a `--dump-indexed-geometry` scout.
    The report lists hot rows, top replay
    target groups, shader-source availability, index-locality availability,
    and whether raw replayable vertex/index payload bytes have been captured.
    Add `--dump-indexed-geometry --dump-indexed-geometry-max-draws N` to a
    tightly filtered no-mutate scout when those bytes are needed. Do not reuse
    a draw-index window from an older run unless the same run's probe CSV proves
    it still selects the target group. Use
    `python3 scripts/tools/select_3dmark05_payload_window.py --probe-draws
    <3dmark05-perf-indexed-probe-draws.csv> --row SEQ/ENC --max-draws N
    --output <trace-run>/analysis/frameN-payload-window-selection.json` to rank
    same-run shader/state groups and emit the exact
    `--probe-indexed-triangle-encoder-draw-min/max` flags for that run. The
    selector also emits `shader_capture_flags` for cross-run payload scouts.
    Add `--class-filter depth-read,no-alpha-blend,no-scissor,textured` and
    `--applied-only` when preparing payloads for a mutating class-gated probe;
    this keeps the selected window aligned with the rows the runtime actually
    changed after row/class/min-gain gates.
    Pass the same selection JSON to
    `build_3dmark05_mini_replay_manifest.py --payload-selection
    <frameN-payload-window-selection.json>` so the replay manifest uses the
    selected row, encoder-local draw window, and draw ordinals instead of
    silently taking the first matching payloads. If a second run drifts by row
    or draw order, prefer shader/state payload filters:
    `--dump-indexed-geometry-vs HASH --dump-indexed-geometry-ps HASH` plus the
    existing class filters, for example
    `--probe-reverse-indexed-triangles-classes alpha-blend,depth-read,textured`.
    Add `--dump-indexed-geometry-cbufs` when the replay must use real per-draw
    `VsConsts`, `PsConsts`, `FfpVsConsts`, and `FfpPsConsts` bytes instead of
    dummy constants; it writes those cbuf files beside each selected geometry
    payload. The geometry payload metadata also records active texture slot
    handles, LODs, formats, sizes, and storage flags. The manifest builder
    preserves that data under `textures`, so a later real-texture replay can
    select the correct dump target by draw/stage instead of guessing from
    `texture_mask`. The metadata also carries color/depth attachment handles,
    formats, sizes, sample counts, and alias texture information under
    `attachments`; the mini replay uses this to choose the standalone Metal
    color/depth/stencil pixel formats when present.
    For depth-sensitive semantic replays, prefer the perf wrapper's
    `--dump-depth-attachment-handle HANDLE` with optional
    `--dump-depth-attachment-seq N`, `--dump-depth-attachment-enc N`, and
    `--dump-depth-attachment-path PATH`. The wrapper defaults the raw sidecar
    to `traces/<run-id>/analysis/frameN-depth.bin` and resolves relative paths
    under the repository root before passing
    `DXMT9_DUMP_DEPTH_ATTACHMENT_PATH`, so the dump does not land under the
    3DMark05 working directory. Feed the resulting raw D24X8 sidecar to
    `run_3dmark05_mini_replay.py --depth-input <raw.bin>` for same-input
    primitive-order image gates.
    The geometry dumper skips invalid index/stream0 ranges before consuming the
    max-draw cap, so early matching setup draws without replayable stream bytes
    do not hide later valid payloads.
    After single-draw semantic bisection, run
    `scripts/tools/analyze_mini_replay_semantics.py`; its `Proof Verdict`
    section classifies whether the payload is production proof, mechanism-only,
    or should block further production gputrace spending.
    Once payloads exist, build the next harness input with
    `python3 scripts/tools/build_3dmark05_mini_replay_manifest.py
    --shader-summary <frameN-shader-dump-summary.csv> --probe-draws
    <3dmark05-perf-indexed-probe-draws.csv> --geometry-dir
    <trace-run>/analysis/geometry --payload-selection
    <trace-run>/analysis/frameN-payload-window-selection.json --vs HASH --ps HASH --output
    <frameN-mini-replay-manifest.json>`. Use `--row SEQ/ENC` as an additional
    filter only when the payload scout was intentionally row-local.
    Prepare an isolated Metal replay app with
    `python3 scripts/tools/run_3dmark05_mini_replay.py
    <frameN-mini-replay-manifest.json> --output-dir <trace-run>/analysis/mini-replay`.
    Add `--run --repeat N` for a no-capture smoke test. After enough disk is
    available, add `--capture-path <trace-run>/analysis/mini-replay.gputrace`
    with `--run` to create an Xcode-openable isolated replay capture. The helper
    requires `2048MiB` free by default before starting capture
    (`--min-capture-free-mb N` or
    `DXMT9_MINI_REPLAY_MIN_CAPTURE_FREE_MB=N` overrides it); if the guard fails,
    keep the no-capture smoke result and free disk before producing `.gputrace`.
    Add `--color-output <name>.ppm` with `--run` to read back the isolated
    replay color attachment. This produces a same-input image artifact that can
    be compared with `compare_experiment_images.py`, unlike cross-run
    `actual.png` screenshots that may be different animation frames.
    The helper rewrites dxmt9's `buffer(30)` argument-buffer MSL into standalone
    constant-buffer bindings, choosing free Metal buffer slots instead of
    assuming fixed `6/7` slots because dumped shaders may already use those
    indices for vertex streams. It uses real cbuf payloads from the manifest when
    present, falls back to dummy constants otherwise, binds dumped extra vertex
    stream payloads when `geometry.streams` contains stream files, falls back to
    a zero-filled dummy stream for missing extra streams, and emits
    `mini-replay-summary.json` with the exact buffer/texture/sampler slots it
    found. Until real sampled texture payloads are dumped, the generated replay
    binds a deterministic white texture and default sampler to every declared
    vertex/fragment texture/sampler slot; binding only slot 0 can produce an
    all-black false result when the shader declares `texture(1..N)` or
    `sampler(1..N)`. For primitive/backend-locality classifiers, add
    `--primitive-order reverse-triangles|sort-min-index|sort-max-index|cache-opt-lru32|cache-opt-lru64`
    to rewrite each dumped uint16 triangle-list index payload into
    `$output_dir/index-order/`, and optionally add `--draw-order reverse` to
    reverse the manifest draw sequence before Xcode counter capture. For a
    correctness gate, run the same manifest once with `--primitive-order
    original --color-output original.ppm` and once with the candidate order,
    then compare the two PPMs with `compare_experiment_images.py
    --policy exact --min-before-active-pct 1 --min-after-active-pct 1`; the
    active-pixel gates prevent an all-black/all-clear replay from passing as
    correctness. Use `--policy lsb1` only as an explicit visual-tolerance
    decision for known destination-dependent blend-order rounding differences;
    it is not the default correctness policy. For gputrace proof runs, prefer
    passing the same pair to `finalize_3dmark05_perf_probe.sh` with
    `--semantic-image-policy exact|lsb1 --semantic-image-before
    <original.ppm> --semantic-image-after <candidate.ppm>` so the Xcode counter
    proof and the semantic image gate are recorded in the same trace run's
    `analysis/` directory. For the screen-blend cached-index opt-in, pair this
    with `--require-screen-blend-cache-proof` so an Xcode performance win cannot
    pass without an explicit semantic policy.
    If the real fragment replay is empty, rerun both orders with
    `run_3dmark05_mini_replay.py --force-fragment-color` to separate
    geometry/depth/raster coverage from texture or fragment-shader replay
    fidelity. Treat that as a weaker coverage/order diagnostic, not a
    real-fragment correctness proof.
    When the real fragment replay fails but force-fragment-color passes, add
    `--force-fragment-primitive-id` to both orders and compare them with
    `analyze_primitive_id_replay.py`. A color-diff pixel whose primitive-id
    owner changed is evidence of a final-writer/order problem, not missing
    geometry. Primitive-owner changes without color differences are not enough
    to reject an exact-safe draw by themselves; conversely, no owner change is
    a strong but conservative safety signal. Keep nonopaque/cache-order probes
    diagnostic unless the same-input semantic image policy passes for the exact
    mutated draw set.
    `--probe-reverse-indexed-triangles-stream0-span-min BYTES` and
    `--optimize-screen-blend-index-order-stream0-span-min BYTES` add a direct
    minimum original stream0 byte-span gate after the row/class filters. Use
    these to classify large stream-span primitive pressure after class filters
    such as `large4096,alpha-blend` prove too indirect.
    `--probe-indexed-triangle-encoder-draw-min N` and
    `--probe-indexed-triangle-encoder-draw-max N` add an encoder-local draw
    index window after row filters. The window is shared by reverse/sort/
    vertex-cache reorder probes, screen-blend index-order probes, and
    split-large-indexed probes. Use it with a row selector to target a concrete
    material run from `3dmark05-perf-indexed-probe-draws.csv`, for example
    `--probe-reverse-indexed-triangles-row 60/2 --probe-indexed-triangle-encoder-draw-min 71 --probe-indexed-triangle-encoder-draw-max 188`.
    `--probe-indexed-triangle-encoder-draw-exclude LIST` subtracts specific
    encoder-local draw indexes from that window. This is trace-local and
    diagnostic only; use it to validate exact-safe mini-replay subsets such as
    "apply draw 14..32 except draw 18" before spending a gputrace.
    `--disable-alpha-test` is the narrower fragment/raster classifier for the
    alpha-test discard path and should be tried before more invasive shader
    substitutions when `--force-fragment-color` changes hidden VS-write
    counters. `--disable-fog` is the matching classifier for the fog blend /
    fog-factor read path, and `--force-texture-white` or scoped
    `--probe-force-texture-white-*` isolates texture sample results without
    removing the rest of the fragment body.
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
