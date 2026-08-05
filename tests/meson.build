dxmt9_test_link_args = []
dxmt9_native_test_env = environment()
# Golden regeneration must never happen inside a test run: update mode
# asserts nothing. Pinning this off means a leaked shell/CI variable cannot
# silently re-baseline pe_producer_differential_spec's certification.
dxmt9_native_test_env.set('DXMT9_UPDATE_PE_PRODUCER_GOLDEN', '0')
dxmt9_native_test_env_vars = ['DXMT9_UPDATE_PE_PRODUCER_GOLDEN=0']
if host_machine.system() == 'darwin'
  dxmt9_test_link_args += meson.get_compiler('cpp').get_supported_link_arguments([
    '-Wl,-no_warn_duplicate_libraries',
  ])
  dxmt9_wine_unix_runtime_subdir = host_machine.cpu_family() == 'x86_64' ? 'x86_64-unix' : host_machine.cpu_family() + '-unix'
  dxmt9_c_args = get_option('c_args')
  if dxmt9_c_args.length() >= 2
    foreach i : range(dxmt9_c_args.length() - 1)
      if dxmt9_c_args[i] == '-arch'
        dxmt9_arch_name = dxmt9_c_args[i + 1]
        if dxmt9_arch_name == 'x86_64'
          dxmt9_wine_unix_runtime_subdir = 'x86_64-unix'
        elif dxmt9_arch_name == 'arm64'
          dxmt9_wine_unix_runtime_subdir = 'aarch64-unix'
        endif
      endif
    endforeach
  endif
  dxmt9_wine_install_path = get_option('wine_install_path')
  if dxmt9_wine_install_path != ''
    if not dxmt9_wine_install_path.startswith('/')
      dxmt9_wine_install_path = join_paths(meson.project_source_root(), dxmt9_wine_install_path)
    endif
    dxmt9_wine_unix_runtime_path = join_paths(dxmt9_wine_install_path, 'lib/wine', dxmt9_wine_unix_runtime_subdir)
    dxmt9_native_test_env.prepend(
      'DYLD_LIBRARY_PATH',
      dxmt9_wine_unix_runtime_path,
      separator: ':')
    dxmt9_native_test_env_vars += 'DYLD_LIBRARY_PATH=' + dxmt9_wine_unix_runtime_path
  endif
endif

python3 = find_program('python3', required: true)
bash = find_program('bash', required: true)
assert_perf_counters = find_program(
  join_paths(meson.project_source_root(), 'scripts', 'check', 'assert_perf_counters.py'),
  required: true,
)

smoke = executable(
  'dxmt9-smoke',
  'native/smoke/smoke.cpp',
  include_directories: dxmt9_inc,
  dependencies: dxmt9_dep,
  link_args: dxmt9_test_link_args,
)

test('dxmt9-smoke', smoke)

subdir('native/core')
subdir('native/shader')
subdir('native/backend')
subdir('native/render')
subdir('native/framegraph')
subdir('native/bridge')

shader_runner = executable(
  'shader_runner_dxmt9',
  'shader_runner/shader_runner_dxmt9.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

if host_machine.system() == 'darwin'
  shader_corpus_root = join_paths(meson.project_source_root(), 'tests/shader_runner/corpus')
  shader_manifest = join_paths(shader_corpus_root, 'MANIFEST.toml')
  # shader_runner_dxmt9 accepts individual .shader_test files; shard the
  # manifest so Meson applies timeout per case instead of to the full corpus.
  shader_corpus_tool = join_paths(meson.project_source_root(), 'scripts/tools/shader_corpus_tool.py')
  shader_corpus_files = run_command(
    python3,
    shader_corpus_tool,
    'list-files',
    '--manifest',
    shader_manifest,
    '--status',
    'passing',
    check: true,
  ).stdout().strip().split('\n')

  # R-BACK-12.27 — Stage 2 argbuf hybrid A/B equivalence harness. The default
  # lane inherits the runtime selector (Stage 2 wherever the Apple3+Tier2 gate
  # holds); the Stage 1 lane forces the gate off via the runtime-side escape
  # hatch `DXMT9_DISABLE_ARGBUF_HYBRID=1` (see
  # `src/dxmt9/dxmt9_command_queue.cpp` around the `setArgbufHybridEnabled`
  # call). Both lanes execute the same `.shader_test` and assert the same
  # `[probe]` lines, so passing both *is* the pixel-equality proof — no
  # separate diff engine is needed. A regression in either lane fails the case
  # and is a Stage 2 release blocker per `specs/backend/draw-uniforms/requirements.md`
  # §7.6.
  shader_corpus_stage1_env = environment()
  if host_machine.system() == 'darwin' and dxmt9_wine_install_path != ''
    shader_corpus_stage1_env.prepend(
      'DYLD_LIBRARY_PATH',
      dxmt9_wine_unix_runtime_path,
      separator: ':')
  endif
  shader_corpus_stage1_env.set('DXMT9_DISABLE_ARGBUF_HYBRID', '1')

  foreach shader_corpus_file : shader_corpus_files
    if shader_corpus_file == ''
      continue
    endif
    shader_corpus_test_name = 'dxmt9-shader-corpus-' + shader_corpus_file.replace('/', '-').replace('.shader_test', '')
    test(shader_corpus_test_name, shader_runner,
         args: [join_paths(shader_corpus_root, shader_corpus_file)],
         env: dxmt9_native_test_env,
         is_parallel: false,
         suite: ['shader-corpus'],
         timeout: 60)
    shader_corpus_stage1_test_name = 'dxmt9-shader-corpus-stage1-' + shader_corpus_file.replace('/', '-').replace('.shader_test', '')
    test(shader_corpus_stage1_test_name, shader_runner,
         args: [join_paths(shader_corpus_root, shader_corpus_file)],
         env: shader_corpus_stage1_env,
         is_parallel: false,
         suite: ['shader-corpus-stage1'],
         timeout: 60)
  endforeach
endif

verify_tla_script = join_paths(meson.project_source_root(), 'scripts', 'check', 'verify_tla.sh')
test('dxmt9-verify-tla', bash, args: [verify_tla_script],
     is_parallel: false,
     timeout: 300)
test('dxmt9-manifest-check', bash, args: [join_paths(meson.project_source_root(), 'scripts/check/check_manifest.sh')],
     is_parallel: false)
test('dxmt9-shader-corpus-gaps', python3,
     args: [
       join_paths(meson.project_source_root(), 'scripts/tools/shader_corpus_tool.py'),
       'gaps',
       '--fail-on-metadata-gaps',
     ],
     is_parallel: false)
test('dxmt9-d3d9-conformance-manifest-check', bash,
     args: [join_paths(meson.project_source_root(), 'scripts/check/check_d3d9_conformance_manifest.sh')],
     is_parallel: false)
test('dxmt9-d3d9-conformance-status-report', python3,
     args: [join_paths(meson.project_source_root(), 'scripts/check/check_d3d9_conformance_status.py')],
     is_parallel: false)
test('dxmt9-drift-report', bash, args: [join_paths(meson.project_source_root(), 'scripts/check/check_drift.sh')],
     is_parallel: false)
test('dxmt9-perf-counter-table-audit', python3,
     args: [join_paths(meson.project_source_root(), 'scripts/check/audit_perf_counter_table.py')],
     is_parallel: false)
test('dxmt9-perf-counter-callsite-audit', python3,
     args: [join_paths(meson.project_source_root(), 'scripts/check/audit_perf_counter_callsites.py')],
     is_parallel: false)
test('dxmt9-perf-docs-source-audit', python3,
     args: [join_paths(meson.project_source_root(), 'scripts/check/audit_perf_docs_sources.py')],
     is_parallel: false)
# winemetal.so install_name fixup audit — see
# scripts/check/audit_winemetal_install_names.py for the rationale. Catches
# a partial-build regression where the post-link `winemetal_unix_install_name_fixup`
# custom_target did not run and the .so kept bare `winemac.so`/`ntdll.so`
# deps that silently break the PE/unix bridge lookup.
test('dxmt9-winemetal-install-name-audit', python3,
     args: [join_paths(meson.project_source_root(), 'scripts/check/audit_winemetal_install_names.py')],
     depends: dxmt9_winemetal_unix_install_name_fixup,
     is_parallel: false)
test('dxmt9-corpus-sync-smoke', python3,
     args: [join_paths(meson.project_source_root(), 'tests/shader_runner/corpus_sync_smoke.py')],
     is_parallel: false)
test('dxmt9-wine-resolve', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_wine_resolve.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-run-d3d9-conformance', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_run_d3d9_conformance.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-cpu-ready-tape-release-contract', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_cpu_ready_tape_release_contract.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'])
test('dxmt9-run-experiment-wsi-acquisition', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_wsi_layer_acquisition_extract.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-run-experiment-renderer-defaults', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_run_experiment_renderer_defaults.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-experiment-profile-defaults', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_experiment_profile_defaults.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-mini-replay-vertex-order', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_mini_replay_vertex_order.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-mini-replay-draw-state', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_mini_replay_draw_state.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-mini-replay-fragment-override', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_mini_replay_fragment_override.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-mini-replay-attachment-format', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_mini_replay_attachment_format.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-mini-replay-fs-volatile', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_mini_replay_fs_volatile.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-mini-replay-coverage-validity', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_mini_replay_coverage_validity.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-mini-replay-execution-proof', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_mini_replay_execution_proof.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)

# The remaining tests/scripts/*.py files. They existed but were never
# registered, so `meson test --suite scripts` reported green while 456
# assertions never ran (and 7 of them were already failing). Names are derived
# from the filename: test_<stem>.py -> dxmt9-<stem with dashes>.
test('dxmt9-3dmark05-probe-scripts', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_3dmark05_probe_scripts.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     timeout: 120,
     is_parallel: false)
test('dxmt9-analyze-alpha-backend-candidates', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_analyze_alpha_backend_candidates.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-analyze-encoder-row-shape', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_analyze_encoder_row_shape.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-analyze-index-merge-scout', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_analyze_index_merge_scout.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-analyze-indexed-probe-classes', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_analyze_indexed_probe_classes.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-analyze-mini-replay-primitive-conflicts', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_analyze_mini_replay_primitive_conflicts.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-analyze-mini-replay-semantics', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_analyze_mini_replay_semantics.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-analyze-primitive-id-replay', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_analyze_primitive_id_replay.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-analyze-programmable-route-feasibility', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_analyze_programmable_route_feasibility.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-analyze-pso-backend-churn', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_analyze_pso_backend_churn.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-analyze-shader-dumps', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_analyze_shader_dumps.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-analyze-stream-ib-backend-churn', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_analyze_stream_ib_backend_churn.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-analyze-stream-ib-staging-feasibility', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_analyze_stream_ib_staging_feasibility.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-analyze-tile-ffp-coverage', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_analyze_tile_ffp_coverage.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-analyze-tile-ffp-expansion', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_analyze_tile_ffp_expansion.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-analyze-vs-buffer-scaling', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_analyze_vs_buffer_scaling.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-analyze-xcode-dxmt-encoder-attribution', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_analyze_xcode_dxmt_encoder_attribution.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-analyze-xcode-replay-variance', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_analyze_xcode_replay_variance.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-analyze-xcode-row-drift', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_analyze_xcode_row_drift.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-audit-backend-escape-surface', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_audit_backend_escape_surface.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-audit-perf-docs-sources', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_audit_perf_docs_sources.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-build-3dmark05-mini-replay-manifest', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_build_3dmark05_mini_replay_manifest.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-compare-3dmark05-perf-counters', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_compare_3dmark05_perf_counters.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-compare-attachment-dumps', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_compare_attachment_dumps.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-compare-experiment-images', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_compare_experiment_images.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-compare-xcode-dxmt-bottlenecks', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_compare_xcode_dxmt_bottlenecks.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-metal-shader-codegen', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_metal_shader_codegen.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-plan-3dmark05-mini-replay', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_plan_3dmark05_mini_replay.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-plan-backend-escape-reduced-ab', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_plan_backend_escape_reduced_ab.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-plan-effect-roi-forcewhite-probes', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_plan_effect_roi_forcewhite_probes.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-run-3dmark05-mini-replay', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_run_3dmark05_mini_replay.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-run-3dmark05-semantic-replay-gate', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_run_3dmark05_semantic_replay_gate.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-select-3dmark05-payload-window', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_select_3dmark05_payload_window.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-summarize-3dmark05-cleanup-candidates', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_summarize_3dmark05_cleanup_candidates.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-summarize-3dmark05-perf', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_summarize_3dmark05_perf.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-summarize-3dmark05-perf-gates', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_summarize_3dmark05_perf_gates.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-summarize-3dmark05-visual-target-gate', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_summarize_3dmark05_visual_target_gate.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-summarize-capture-rois', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_summarize_capture_rois.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-summarize-color-attachment-dumps', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_summarize_color_attachment_dumps.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-summarize-effect-geometry-roi', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_summarize_effect_geometry_roi.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-summarize-fragmentless-depth-route-gate', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_summarize_fragmentless_depth_route_gate.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-summarize-framegraph-dag', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_summarize_framegraph_dag.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-summarize-index-cache-runtime', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_summarize_index_cache_runtime.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-summarize-primitive-conflict-selectors', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_summarize_primitive_conflict_selectors.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-summarize-semantic-payload-candidates', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_summarize_semantic_payload_candidates.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-summarize-visibility-scout', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_summarize_visibility_scout.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-summarize-xcode-encoder-counters', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_summarize_xcode_encoder_counters.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-summarize-xctrace-cpu-threads', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_summarize_xctrace_cpu_threads.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
test('dxmt9-summarize-xctrace-metal-intervals', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_summarize_xctrace_metal_intervals.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
