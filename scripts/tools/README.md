# scripts/tools

General-purpose developer tooling that does not belong to a single suite or
build. `shader_corpus_tool.py` is also imported by the Meson tests under
`tests/meson.build` for shader-corpus listing and gap reporting.

- `shader_corpus_tool.py` — manage the shader corpus: list passing files,
  drift against upstream, gap reporting (used by the Meson test wiring).
- `package_app_local.py` — assemble a self-contained `dxmt9-app-local`
  distribution from PE and unix builds.
- `cleanup_dxmt9_temp_prefixes.py` — list/prune temporary Wine prefixes
  created by experiment runs.
- `summarize_3dmark05_cleanup_candidates.py` — non-destructively rank
  3DMark05 `traces/` and `experiments/output/` run-id cleanup candidates,
  marking run ids referenced by `docs/perfomance/**/*.md`.
- `summarize_index_cache_runtime.py` — summarize 3DMark05 encoder-only
  reordered-index-cache lookup/apply/reject telemetry for opt-in proof runs.
- `summarize_3dmark05_perf.py` — parse dxmt9 per-run perf logs into encoder,
  stream, indexed-probe, and per-frame sampling CSVs. Enable
  `DXMT9_PERF_FRAME_SAMPLING=1` or `run_3dmark05_perf_probe.sh --frame-sampling`
  for `3dmark05-perf-frames.csv`; the Markdown slow-frame table is the
  preferred first gate for visual/perf-coupling hypotheses such as bloom,
  glow, and rifle muzzle effects. Indexed probe rows include runtime
  VS/PS-constant and uniform-payload hashes so mini-replay
  `vsconsts_hash`/`psconsts_hash` blocker evidence can be compared against
  runtime-visible draw telemetry before spending another `.gputrace`. Future
  rows also include `stream_extra_bindings` (`sN:0xhandle@offset/stride`) so
  stream1+ handle alternation can be joined to draw windows rather than only
  encoder-level totals. Use
  `run_3dmark05_perf_probe.sh --probe-draw-packet-actual-change` when snapshot
  cache misses need declared-vs-actual draw-packet delta evidence.
- `summarize_xctrace_metal_intervals.py` — parse xctrace
  `metal-gpu-intervals` XML and join dxmt9 encoder attribution by
  `RenderPass[seq=...,enc=...]`. Use it as a timing/label sidecar for 3DMark05
  when `.gputrace` replay counters are blocked by capture-layer mechanics. The
  joined CSV and Markdown include primitive-class labels plus normalized
  `xctrace_vertex_ms_per_mvertex` / `xctrace_stage_ms_per_mvertex` so
  scene-phase changes can be separated from raw total-time changes. The
  Markdown also emits aggregate tables by primitive class and render-pass end
  reason, which is the fastest way to see whether the residual owner is
  opaque/depth, alpha, clear, RT-change, or present work. Current encoder
  breakdown rows also carry route-level primitive counters, so the sidecar can
  add depth-only/textured/color route verdicts without per-draw indexed probe
  logging. Pass `--indexed-probe-draws <3dmark05-perf-indexed-probe-draws.csv>`
  only when that CSV contains draw rows and per-draw route detail is needed.
- `run_3dmark05_system_trace_sidecar.sh` — guarded wrapper around
  `run_3dmark05_perf_probe.sh` and `xcrun xctrace record --template
  'Metal System Trace' --all-processes`. It runs the probe wrapper dry-run
  first, refuses locked sessions before starting xctrace, records a short
  normal-rendering no-gputrace sidecar, then exports/summarizes
  `metal-gpu-intervals` against `3dmark05-perf-encoders.csv` and
  `3dmark05-perf-indexed-probe-draws.csv`. Use `--wait-unlocked-sec N` when
  the command should poll preflight and start automatically after the desktop
  unlocks; locked waits and dry-runs do not create trace artifacts. When the
  trace is summarized, RenderPass-labelled xctrace rows, high dxmt encoder
  join coverage, and at least one route verdict are required. Route verdicts
  normally come from encoder-summary `route_*` fields; indexed probe rows are
  optional and override the summary when per-draw detail is needed. This
  prevents another route-selection capture from ending as a silent
  `route-unavailable` sample while avoiding unnecessary indexed per-draw log
  volume. The sidecar defaults to
  `--encoder-breakdown-all-frames` on the wrapped probe, because xctrace
  records a wall-clock window and cannot reliably join against a single
  `DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=<frame>` CSV. All-frame breakdown is valid
  as a join proof but is too heavy for representative runtime FPS. For actual
  timing samples, pass `--encoder-breakdown-seq-range MIN:MAX` with a window
  that covers the expected xctrace RenderPass seq range; the wrapper forwards
  that to `DXMT9_PERF_ENCODER_BREAKDOWN_SEQ_MIN/MAX` and the join coverage gate
  verifies the range was correct. Exact scoped encoder-breakdown settings are
  rejected for this wrapper. If a range run logs encoder or indexed-probe rows
  before `MIN`, suspect a stale installed unix provider first: the standard
  staging path now builds `build-x86_64-builtin/src/winemetal/unix/winemetal.so`
  before copying it, but manual installs must do the same or new env filters
  will not exist in the active Wine provider.
- `run_with_wine_metal_capture_layer.sh` — temporarily replace a Wine root's
  actual `bin/wine.real` and `bin/wine-preloader` names with capture-enabled
  copies, run a command, then restore the originals. Use it for non-3DMark05
  Wine/D3D9 `.gputrace` diagnostics when `DXMT_METAL_CAPTURE_FRAME/PATH`
  reports `Capture layer is not inserted`. Do not use it as a standard
  3DMark05 path: it proves the temp Wine launcher can receive
  `MetalCaptureEnabled`, but current 3DMark05 black-screens before draw/present
  when that layer is present. The wrapper rejects 3DMark05 command lines by
  default; pass `--allow-3dmark05` or set
  `DXMT9_ALLOW_3DMARK05_CAPTURE_LAYER=1` only for an explicit invalid-sample
  diagnostic.
- `summarize_framegraph_dag.py` — parse
  `DXMT9_RENDERER_DUMP_DAG` JSON dumps and report same-attachment re-entry
  pairs, direct A->B edge resources, intervening same-attachment accesses,
  intervening edge counts, draw ranges, and load/store shape. Use it on
  `traces/<app-runid>/analysis/dag` after a frame-scoped DAG run, or let
  `run_3dmark05_perf_probe.sh --dump-framegraph-dag` invoke it automatically,
  to turn H6 render-pass coalesce candidates into CSV/Markdown before spending
  another Xcode counter capture. The wrapper writes combined, pre-opt, and
  post-opt summaries; pre-opt owns candidate discovery, post-opt owns
  optimizer-effect confirmation.
- `select_3dmark05_payload_window.py` — rank row-local shader/state draw groups
  from an indexed-probe CSV and emit a same-run payload-window selection with
  geometry capture flags. Use it before `--dump-indexed-geometry`; row-local
  draw windows are not stable across independent 3DMark05 runs.
- `build_3dmark05_mini_replay_manifest.py` — join shader summaries, indexed
  probe rows, and geometry sidecars into a mini-replay manifest. The summary
  includes `texture_capture_handles_arg` and `texture_capture_flags` so a
  follow-up draw-texture sidecar run can be launched without manually reading
  geometry `.meta` files.
- `run_3dmark05_semantic_replay_gate.py` — run the standardized mini-replay
  semantic gate: original/candidate color replays, exact/`lsb1` image compare,
  primitive-id replays, canonical original-triangle owner compare, and optional
  primitive-conflict analysis. The summary JSON includes aggregate conflict
  counters when conflict summaries are present. Use this before spending
  Xcode/gputrace budget on a primitive-order candidate.
- `analyze_vs_buffer_scaling.py` — compare joined Xcode/dxmt summaries across
  3DMark05 captures. The optional `--delta-output` CSV decomposes VS write
  movement into invocation-count and bytes/invocation effects, matching
  `compare_xcode_dxmt_bottlenecks.py`, so non-reorder backend-shape candidates
  can be preflighted before another Xcode replay.
- `analyze_alpha_backend_candidates.py` — preflight large alpha-blend indexed
  draw classes before Xcode spend. It joins indexed-probe rows with dumped MSL
  shaders and rejects blend-disable as a correctness fix unless the blend state
  is statically equivalent to replace.
- `analyze_pso_backend_churn.py` — preflight whether PSO/state churn is
  isolated enough to justify Xcode spend for hidden backend storage. It reads
  `3dmark05-perf-encoders.csv`, compares PSO changes against stream and
  index-buffer handle churn, optionally joins
  `3dmark05-perf-indexed-probe-draws.csv`, and reports whether any
  stream/IB-handle-stable draw run still changes PSO. It emits a
  `no-pso-xcode-candidate` gate when the hot rows are still stream/IB dominated
  or when per-draw probes show PSO movement coupled to binding-tuple motion.
- `analyze_stream_ib_backend_churn.py` — preflight stream/IB as a possible
  hidden-backend denominator experiment. It joins encoder and per-stream CSVs,
  optionally joins indexed probe draws, separates handle churn from
  offset/stride churn, reports explicit writer bytes per vertex, counts
  draw-level stream0/IB/extra-stream binding identity changes, reports complete
  binding-tuple churn/run length, reports stream0/IB pair deltas and
  stream0/first-extra-stream/IB triplet deltas, and emits a handle-stable A/B
  requirement before Xcode counters.
- `analyze_stream_ib_staging_feasibility.py` — estimate the copy cost and
  offset-churn tradeoff for a row-stable stream/IB staging A/B before writing
  renderer code or spending Xcode counters. It uses indexed probe-draw stream0,
  extra-stream, and effective-IB ranges, joins encoder explicit writer bytes
  when available, and reports whether a no-gputrace staging preflight is
  plausible or whether allocation-time coalescing/narrower windows are needed.
- `analyze_tile_ffp_expansion.py` — split Tile-FFP hot-row coverage failures
  into current eligible primitives, not-FFP fallback, unsupported-state
  fallback, precision fallback, programmable draws, and textured draws. Use it
  after `analyze_tile_ffp_coverage.py` when `tile-ffp` is blocked by coverage;
  for GT1 frame60 it shows the hot rows require a programmable/textured tile or
  mesh-style route rather than a minor FFP selector widening.
- `analyze_programmable_route_feasibility.py` — consume indexed probe draw CSVs
  and split programmable backend-route work into depth-only, programmable color,
  and programmable textured buckets. Use it after Tile-FFP expansion points at a
  programmable route; for frame60 it identifies `60/0` as the smaller
  depth-only reduced A/B candidate and `60/2` as the larger textured route.
- `summarize_fragmentless_depth_route_gate.py` — combine the row-scoped
  fragmentless-depth-only encoder CSV, perf summary counters, optional route
  logs, optional same-input equality CSV, and optional baseline/treatment Xcode
  counter summaries. Use it after
  `--probe-fragmentless-depth-only-row 60/0`: route coverage alone should emit
  `route-reachable-needs-equality`, not an Xcode promotion. Only a passed
  equality CSV should move the gate to `ready-for-xcode-counters`; flat Xcode
  `VS Buffer Device Memory Bytes Written / VS Invocations` then rejects the
  denominator route.
- `audit_backend_escape_surface.py` — static/no-gputrace preflight for the
  remaining backend-denominator escape hatches. It separates winemetal bridge
  support from actual dxmt9 draw-route/shader-emitter support for mesh/object,
  distinguishes visible position-only `VSOut` probes from a real
  position/binning route, and can attach Tile-FFP coverage CSVs so bridge-only
  or no-coverage candidates do not become Xcode capture targets.
- `plan_backend_escape_reduced_ab.py` — consume
  `audit_backend_escape_surface.py` CSV output and turn "reduced A/B required"
  into explicit route/coverage, same-input equality, reduced counter, and GT1
  promotion gates for mesh/object, position/binning, and Tile-FFP. Use this
  before implementing a new backend escape or scheduling another GT1 Xcode
  capture from that lane. `--tile-ffp-expansion-csv` can attach
  `analyze_tile_ffp_expansion.py` output so the Tile-FFP row says whether hot
  coverage needs a programmable/textured route instead of generic eligibility
  expansion.
- `summarize_3dmark05_perf_gates.py` — combine Xcode VS-buffer scaling,
  optional VS delta attribution, semantic payload, primitive-conflict selector,
  semantic selector sweep, visibility scout summaries, and indexed state/class
  proxy summaries into a current optimization gate decision plus semantic final-color,
  final-color/final-writer runtime-selector, final-color runtime-blocker,
  implementation-track, and gate-aware next experiment queues in Markdown and
  optional CSV form. `--visibility-summary-csv` adds a
  `visibility-no-sample-hotpath` gate so no-sample proof is not kept alive when
  zero-sample rows are too small. If the semantic candidate CSV contains
  `--visibility-csv` join columns, the gate also emits
  `visibility-positive-oracle` so positive Metal visibility is not mistaken for
  final-color proof. Class-proxy production rows are not treated as
  proof by themselves: if the VS scaling input lacks the opaque-depth Xcode proof
  run, the gate reports `missing-production-gate-input` and queue rows report
  `needs-production-gate-input`. `--screen-blend-semantic-csv` must point at the
  same-input image-comparison CSV for screen-blend rows; historical exact/`lsb1`
  notes are not treated as current gate proof unless that artifact is attached.
  `--pso-backend-churn-csv` joins the per-draw PSO isolation output from
  `analyze_pso_backend_churn.py`; when no stream/IB-handle-stable run changes
  PSO, the gate emits `pso-backend-isolation=reject-current` and blocks current
  PSO churn from becoming an Xcode replay target.
  `--locality-semantic-ceiling-csv` joins the calibrated locality ceiling CSV;
  when color-exact/zero-sample rows are too small but sample-visible rows are
  large, the gate emits `locality-semantic-ceiling=oracle-required` so another
  locality Xcode capture is blocked until a final-color/final-writer oracle can
  keep enough sample-visible gain. `--semantic-replay-summary-json` can be
  passed multiple times with `run_3dmark05_semantic_replay_gate.py` summaries;
  the gate emits `final-writer-replay-oracle` so same-input real-texture replay
  results are not reduced to a generic "oracle needed" note. A replay set with
  final-color+owner movement or owner-masked color-exact movement remains
  blocked before Xcode. `--backend-escape-surface-csv` attaches
  `audit_backend_escape_surface.py` output; when mesh/object is bridge-only,
  position/binning is visible-probe-only, and Tile-FFP has no hot-row coverage,
  the gate emits `backend-escape-surface=reduced-ab-required` and keeps
  non-reorder backend guesses out of direct GT1 Xcode spend.
  `--backend-escape-reduced-ab-plan-csv` attaches
  `plan_backend_escape_reduced_ab.py` output; when every backend escape is
  still blocked by route/coverage preconditions, the gate emits
  `backend-escape-reduced-ab-plan=blocked-before-reduced-ab` and adds that
  reason to implementation tracks and next-experiment queue rows.
  For no-mutate class proxies, the
  next-experiment queue reports `candidate_miss32_delta` ahead of
  `miss32_delta`, so unapplied locality ceilings do not appear as zero-LRU rows.
- `run_3dmark05_perf_probe.sh` — standard 3DMark05 GT1 perf launcher. Always
  use a positive `--timeout` because 3DMark05 can hang at the final frame.
  Add `--frame-sampling` for no-gputrace visual/perf-coupling runs; it emits
  per-Present wall-clock `wall_ms/fps` plus draw/pass/wait deltas and writes
  `3dmark05-perf-frames.csv` through the summary step.
  When a timeout-finalized capture has complete Xcode encoder counters and
  `dxmt9.log` but no `result.json`, pass `--allow-partial-stable-frame-proof`
  with stable-frame proof presets; keep `--require-result-json` for clean
  catalogue-result gates and run-level comparisons.
  Diagnostic index-cache candidate variants include
  `--index-cache-candidate-frontier-cap`,
  `--index-cache-candidate-lazy-frontier`,
  `--index-cache-candidate-bucketed-select`,
  `--index-cache-candidate-strict-lru`, and
  `--index-cache-candidate-upper-bound-gate`; treat all candidate-order
  changes as hypotheses until no-gputrace counters and a `v0.0.1` visual anchor
  check agree. Use the diff image against `v0.0.1` to catch black/translucent
  vertices, broken UVs, texture/color drift, and cbuf-identity artifacts, while
  reserving raw pixel percentages from time-based screenshots for triage only.
  `--probe-fragmentless-depth-only-row SEQ/ENC` is a diagnostic-only
  backend-shape route smoke for depth-only rows; it still requires equality and
  Xcode counter proof before any promotion. Run
  `summarize_fragmentless_depth_route_gate.py` on the smoke output before
  scheduling Xcode/gputrace.
  Color attachment after-draw dumps are also diagnostic-only because they force
  render-encoder splits and can materialize tile/pass intermediates that the
  normal pass-end store does not preserve. Directory-mode color histories record
  `commandIndex`, `commandDrawIndex`, and `commandDrawCount`; use those fields
  together before interpreting multiple sidecars from one draw-run command.
  If the target is an adjacent effect draw sequence, avoid over-constraining
  with `--dump-color-attachment-enc`: the first matching after-draw dump ends
  the current render encoder, so the next draw can move to `enc+1` while the
  command index remains the more useful local identifier.
  For gputrace runs, the wrapper now sets `DXMT_METAL_CAPTURE_FRAME/PATH`
  without `MTL_CAPTURE_ENABLED=1` by default because that Apple capture-layer
  env has reproduced 3DMark05 black-screen startup with draw/present counters
  at zero. Use `DXMT_3DMARK05_SET_MTL_CAPTURE_ENABLED=1` only for deliberate
  capture-layer experiments. If normal rendering works but file capture reports
  `Capture layer is not inserted`, do not automatically escalate to
  `run_with_wine_metal_capture_layer.sh` for 3DMark05: that wrapper works for a
  synthetic Wine/D3D9 `.gputrace` but currently black-screens 3DMark05 before
  draw/present. Use `run_3dmark05_system_trace_sidecar.sh -- ...` for the
  normal-rendering xctrace Metal System Trace path, or an explicitly validated
  Xcode attach-after-normal-start route instead. The sidecar runner refuses
  locked sessions before recording xctrace; pass `--wait-unlocked-sec N` if the
  run should wait for unlock and then launch automatically. Manual runs can
  still use the wrapper-printed `xctrace_system_trace_export_cmd` and
  `xctrace_system_trace_summary_cmd` for traces recorded as
  `traces/<run>/metal-system.trace`; add `--measure-index-reuse` only when
  per-draw route/index detail is needed.
- `compare_experiment_images.py` — compare before/after screenshots or
  deterministic capture PNGs for semantic gates. Use repeatable
  `--roi L,T,R,B[:name]` regions when full-frame changes are too broad, for
  example force-white bloom/effect probes where a local muzzle or glow region
  needs to be scored separately from global overlay/tint movement.
- `summarize_effect_geometry_roi.py` — join `dxmt9-effect-geometry` logs to
  one or more screen-space ROIs. It uses projected `screen_min/screen_max` when
  available and falls back to pretransformed/screen-space `pos_min/pos_max` for
  fullscreen glow quads, then emits Markdown and CSV overlap summaries. Treat
  the result as geometry/bbox candidate evidence only; it does not prove the
  final-color writer for a pixel. The default effect trace is alpha-blended and
  textured only; add `run_3dmark05_perf_probe.sh
  --effect-draw-trace-include-non-alpha` and/or
  `--effect-draw-trace-include-untextured` when debugging a missing visual
  effect that may have fallen onto the wrong render-state path. It can also
  consume `summarize_capture_rois.py` connected-component CSVs with
  `--component-roi-csv`; add `--component-roi-match-seq` when the capture frame
  number is expected to match the effect-geometry `seq`, so false-positive
  bright components can be classified by draw/texture family without mixing
  other frames into the ROI join. Use `--min-bbox-coverage-pct` to reject huge
  projected/fullscreen quads that merely contain a small bright component; for a
  local muzzle sprite, high ROI coverage with near-zero bbox coverage is broad
  overlap evidence, not final-writer proof.
- `summarize_color_attachment_dumps.py` — summarize raw color attachment
  sidecars and their `.bin.json` metadata into ROI max/average/bright/white/warm
  pixel tables. Use it for pass-end versus after-draw color histories instead
  of hand-written one-off scripts; the CSV preserves `commandIndex`,
  `commandDrawIndex`, and `commandDrawCount` when newer sidecars include them.
  For muzzle-flash work, treat `bright_pixels` as a broad signal only because it
  also catches cyan beam/post false positives. Prefer `warm_pixels` and
  `warm_hot_*` for the white/yellow muzzle-bloom oracle; the default warm test is
  `red >= 180`, `green >= 110`, and `blue <= red + 32`. Anchor the ROI with a
  visible positive sample first: YouTube/demo infantry muzzle flashes and the
  local working machine-gun path show a short-lived circular white/yellow bloom
  attached to the muzzle before the post chain, whereas cyan tracer/post rows
  can be bright but should remain `warm=0`. Public-video frames are a
  shape/event oracle only: accept a muzzle candidate only when it is
  barrel-attached, roughly circular, and short-lived; reject long tracers,
  impact sparks, broad haze, engine lights, and warm background panels even when
  scalar ROI thresholds look promising.
- `summarize_capture_rois.py` — summarize ordinary screenshot/capture images
  into the same ROI warm/white/bright style table. Use it on deterministic
  `DXMT_CAPTURE_FRAMES` / `DXMT_CAPTURE_RANGE` PNGs before launching a
  draw-owner or Xcode/gputrace probe, so the target internal frame is first
  proven to contain the expected visual shape. For muzzle work, sort by
  `--sort signal` and compare weapon-attached ROIs against glare/control ROIs;
  high warm pixels in a broad control ROI is beam/glare evidence, not a
  final-writer oracle. Use `--candidate-roi`, `--control-roi`,
  `--frame-score-output`, and `--frame-score-csv-output` to rank frames whose
  local candidate ROI dominates the control/glare ROI before spending Xcode
  time. Add `--frame-score-montage-output` to write a visual audit image with
  full-frame thumbnails plus candidate/control ROI crops for the top-ranked
  frames. Use `--component-output`, `--component-csv-output`, and
  `--component-montage-output` to search ordinary captures for warm/white
  connected components before hand-picking a new ROI. For the current rifle
  oracle, do not use the old tiny-sprite gate as the final decision; the
  `JbKmFz6v9uk` `01:05` reference shows a simple circular bloom disc. The
  current broad defaults (`--component-max-area 2000`,
  `--component-max-width 220`, `--component-max-height 180`) are a better
  prefilter. Add `--component-max-aspect-ratio` and
  `--component-min-fill-pct` when the oracle is a compact round bloom rather
  than a long tracer; then barrel attachment and candidate/control ROI
  dominance must decide whether a component deserves a draw-owner or
  Xcode/gputrace probe.
- `plan_effect_roi_forcewhite_probes.py` — consume
  `summarize_effect_geometry_roi.py` CSV output and build draw-local
  `run_3dmark05_perf_probe.sh` force-white queues. It converts the 1-based
  `dxmt9-effect-geometry` `encoder_draw_index` to the probe range's 0-based
  `DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MIN/MAX` value and includes
  `--encoder-breakdown-seq <candidate-seq>` because the draw-index selector
  depends on encoder-breakdown draw counters even for no-gputrace probes. Use
  this rather than hand-transcribing ROI geometry rows into wrapper commands.
  When the CSV has `command_index`, the queue uses
  `--probe-force-texture-white-command-index` and collapses duplicate ordinals
  that map to the same command-local draw selector; this is more useful than
  ordinal-only targeting for replayed chunks. Pair component-derived ROI queues
  with `--min-bbox-coverage-pct` so huge projected/fullscreen bboxes are not
  promoted into draw-local force-white probes merely because they contain a
  small bright component. A local rifle muzzle/fire-atlas candidate should
  survive both high ROI coverage and non-negligible bbox coverage; if
  `0x7f`/`0x75` queues are empty under that gate, hold Xcode/gputrace spend
  until a better same-frame visual target exists.
  This is still a replay selector, not a final-writer oracle. If a generated
  command reports
  `encode_draw_pso_prefetch_bypass_probe=0` or the perf summary keeps
  `probe_force_texture_white_draws=0`, treat the row/command slot as unstable
  across independent runs. A valid replay also needs the captured scene/HUD
  frame to match the baseline target; otherwise image differences are frame
  drift, not A/B evidence. Switch to same-run instrumentation or direct
  gputrace draw inspection when either gate fails.
- `summarize_3dmark05_visual_target_gate.py` — combine component-scan,
  local effect-geometry overlap, and force-white queue CSVs into a conservative
  Xcode/gputrace readiness verdict. For rifle muzzle work, pass the expected
  source textures (`0x7f` fire atlas and `0x75` beam/tracer family); a
  `blocked-local-non-source` verdict means bright components exist, but the
  surviving local writers are material/shadow/post classes rather than the
  expected source draw, so gputrace should wait for a better frame or same-run
  final-writer proof.
- `summarize_3dmark05_cleanup_candidates.py` — non-destructive disk-space audit
  for 3DMark05 run ids under `traces/` and `experiments/output/`. It scans
  `docs/perfomance/**/*.md` for run-id references, reports referenced versus
  unreferenced storage, and should be used before deleting large `.gputrace` or
  log trees needed by current proof gates.
- `summarize_visibility_scout.py` — summarize
  `frame<N>-visibility-scout.csv` into row/class buckets and an optional
  `metal_draw_index` window. The Markdown report includes a no-sample draw
  table, and `run_3dmark05_perf_probe.sh` runs it automatically when
  `--visibility-scout*` is enabled. Join `--probe-draws` when indexed-probe
  cache telemetry is available; use zero `visible_samples` as no-sample
  evidence, but keep positive rows behind final-color/final-writer proof.
- `summarize_primitive_conflict_selectors.py` — summarize primitive-owner
  conflict metrics and report whether runtime-shaped selectors separate exact
  pass/fail mini-replay rows.
- `analyze_mini_replay_semantics.py` — join mini-replay semantic bisection
  rows with draw state, shader, geometry, payload hashes, primitive-owner
  risk, final-color visibility, runtime-field sweep, and final-color
  runtime-selector/runtime-blocker sweep CSV output. Use
  `--selector-max-fields N` to widen runtime/geometry/shader selector
  combinations; 3-field-and-above sweeps skip trace-local payload/constant hash
  combinations to avoid debug-key overfit. Use `--runtime-probe-csv
  <3dmark05-perf-indexed-probe-draws.csv>` to attach runtime
  VS/PS-constant and uniform-payload hashes by `seq/encoder/encoder_draw_index`;
  full uniform payload hash remains draw-local and is excluded from the
  production-shaped all-runtime-visible blocker key. A blocker row means all
  currently exported runtime-visible fields fail to separate visible exact-pass
  draws from a semantic failure, so the next proof needs final-color/final-writer
  data, Metal-visibility-backed no-sample evidence, or a non-reorder mechanism.
  The current D3D9 occlusion query path is primitive-count compatible and is not
  this oracle.
  `run_3dmark05_perf_probe.sh --visibility-scout-row SEQ/ENC` now wires a
  diagnostic Metal visibility buffer and writes per-Metal-draw
  `visible_samples` after GPU completion; use zero counts as no-sample evidence,
  but do not treat positive counts as final-color proof. The Markdown/CSV output
  also includes a
  Final-Writer Oracle bucket that separates real final-writer color hazards
  from exact-pass primitive-owner changes whose final color remains stable, and
  a final-writer runtime selector sweep that explicitly marks full runtime
  uniform-payload separation as draw-local overfit.
- `summarize_semantic_payload_candidates.py` — summarize ranked 3DMark05
  semantic payload mini-replays and emit rank-level plus bucket-level
  final-color oracle queues for order-sensitive locality candidates. Optional
  `--visibility-csv` joins a Metal visibility scout by `seq/encoder` and
  encoder-local draw index so sample-positive and no-sample evidence can be
  compared against final-color replay verdicts; positive visibility is still
  not final-color proof.
- `sync_corpus.sh` — sync a vkd3d upstream corpus into this tree via
  `shader_corpus_tool.py sync`.
- `run_dx9_present_policy_ab.py` — A/B compare present policies across apps;
  emits boundary-counter JSON consumed by `specs/benchmarks/`.
