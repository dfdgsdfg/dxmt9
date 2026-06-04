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
  marking run ids referenced by `specs/perfomance.plan.md`.
- `summarize_index_cache_runtime.py` — summarize 3DMark05 encoder-only
  reordered-index-cache lookup/apply/reject telemetry for opt-in proof runs.
- `summarize_3dmark05_perf.py` — parse dxmt9 per-run perf logs into encoder,
  stream, and indexed-probe CSVs. Indexed probe rows include runtime
  VS/PS-constant and uniform-payload hashes so mini-replay
  `vsconsts_hash`/`psconsts_hash` blocker evidence can be compared against
  runtime-visible draw telemetry before spending another `.gputrace`.
- `analyze_vs_buffer_scaling.py` — compare joined Xcode/dxmt summaries across
  3DMark05 captures. The optional `--delta-output` CSV decomposes VS write
  movement into invocation-count and bytes/invocation effects, matching
  `compare_xcode_dxmt_bottlenecks.py`, so non-reorder backend-shape candidates
  can be preflighted before another Xcode replay.
- `summarize_3dmark05_perf_gates.py` — combine Xcode VS-buffer scaling,
  optional VS delta attribution, semantic payload, primitive-conflict selector,
  semantic selector sweep, and indexed state/class proxy summaries into a
  current optimization gate decision plus semantic final-color,
  final-color/final-writer runtime-selector, final-color runtime-blocker,
  implementation-track, and gate-aware next experiment queues in Markdown and
  optional CSV form.
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
  draws from a semantic failure, so the next proof needs final-color/occlusion
  data or a non-reorder mechanism. The Markdown/CSV output also includes a
  Final-Writer Oracle bucket that separates real final-writer color hazards
  from exact-pass primitive-owner changes whose final color remains stable, and
  a final-writer runtime selector sweep that explicitly marks full runtime
  uniform-payload separation as draw-local overfit.
- `summarize_semantic_payload_candidates.py` — summarize ranked 3DMark05
  semantic payload mini-replays and emit rank-level plus bucket-level
  final-color oracle queues for order-sensitive locality candidates.
- `sync_corpus.sh` — sync a vkd3d upstream corpus into this tree via
  `shader_corpus_tool.py sync`.
- `run_dx9_present_policy_ab.py` — A/B compare present policies across apps;
  emits boundary-counter JSON consumed by `specs/benchmarks/`.
