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
  stream, and indexed-probe CSVs. Indexed probe rows include runtime
  VS/PS-constant and uniform-payload hashes so mini-replay
  `vsconsts_hash`/`psconsts_hash` blocker evidence can be compared against
  runtime-visible draw telemetry before spending another `.gputrace`. Future
  rows also include `stream_extra_bindings` (`sN:0xhandle@offset/stride`) so
  stream1+ handle alternation can be joined to draw windows rather than only
  encoder-level totals.
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
  Xcode counter proof before any promotion.
  For gputrace runs, the wrapper now sets `DXMT_METAL_CAPTURE_FRAME/PATH`
  without `MTL_CAPTURE_ENABLED=1` by default because that Apple capture-layer
  env has reproduced 3DMark05 black-screen startup with draw/present counters
  at zero. Use `DXMT_3DMARK05_SET_MTL_CAPTURE_ENABLED=1` only for deliberate
  capture-layer experiments. If normal rendering works but file capture reports
  `Capture layer is not inserted`, attach Xcode and set
  `DXMT_3DMARK05_METAL_CAPTURE_DESTINATION=developerTools` for an Xcode-targeted
  capture route.
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
