# dxmt9 Environment Variables - Renderer / Frame Graph

Part of the [`environment_variables.rules.md`](environment_variables.rules.md)
index. This file covers backend selection, modern-renderer feature gates, and
Frame Graph DAG debug export. A flag is "set" when its value is a non-empty
string that is not `0`, unless documented otherwise.

## Backend selection

`DXMT9_RENDER_MODE`, the compatibility profile, and the feature-token names are
stable provider contracts under `specs/backend/render-provider/spec.md`.
`DXMT9_RENDER_PARTITION_MODE` is the stable partition-axis contract under
`specs/backend/encode-scheduling/spec.md`.
Activation state is separate: `passcoalesce` is default, `dce` is implemented
opt-in, and the remaining named feature tokens are planned/unavailable. Debug
export and divergence flags below are diagnostic surfaces, not provider modes.

| Var | Purpose | Default |
|---|---|---|
| `DXMT9_RENDER_MODE` | Select the unix-side render backend. Unset and `framegraph` resolve to `FrameGraphBackend`. Empty, `0`, `traditional`, and unknown values resolve to the conservative `TraditionalBackend` rollback. `scripts/run_apps/run_experiment.py` maps an omitted catalogue `render_mode` to `framegraph` unless the process environment explicitly overrides it for a diagnostic run. | framegraph |
| `DXMT9_RENDER_PARTITION_MODE` | Queue-immutable partition execution selector. Unset or `identity` uses the allocation-free identity cursor. `serial` runs the bounded production planner after final Traditional/FrameGraph replay order and DCE selection, subdividing only large DrawRuns at indexed-merge-preserving edges before complete validation and serial consumption. `parallel` resolves to `ExplicitParallel`, extracts sealed source-local passes from the final replay order, and partitions them into two to 16 children. Extraction is per pass interval: `Clear`, `Present`, and the non-child coordinator helpers (`SurfaceCopy`, `StretchRect`, `Readback`, `ColorFill`, `DepthResolve`) each terminate an interval and stay at their exact serial position, so one helper no longer rejects a whole source. A helper that a same-attachment draw resumes across fails both fragments closed. A single DrawRun is divided evenly with every child at least 64 draws; a multi-command pass preserves whole commands and order, closes each non-final child at the earliest qualifying 64-draw prefix while retaining a complete 64-draw suffix, and absorbs a thin final suffix. It then revalidates every source-qualified locator, draw, ABI, PSO, and uniform value, routes the candidate through the policy proof core (value-owned semantic certificate, then checked fixed-point selection), and enforces the pure economics gate, which rejects child-size imbalance greater than 64; any rejection returns to exact serial replay before render-pass preparation or parent Metal effects. Accepted passes create a real WMT `MTLParallelRenderCommandEncoder` and dispatch ordered child encoders on the bounded executor. Clear, Present, actions, sidecars, completion, and command-buffer ownership stay on the coordinator. One immutable pass proof selects either Stage 1 direct binding or Stage 2b direct constant buffers at slots 0/3; each child has an independent binding shadow, while slot-30 tables, resource arrays, mixed ABIs, PSO-rebuilding overrides, UP, tile FFP, carried/incomplete passes, active sidecars, and unresolved late Store actions fall back before effects. Empty and unknown values fail closed to identity. Partition edges do not create render-pass, command-buffer, action, or completion boundaries. The 2026-08-18 calibrated matched-pair evidence measures the provider as performance-neutral on GT2 (true average fps within 1.5% of identity, medians within 1%, zero GPU errors) because the encode stage is not on the critical path there — the encode thread waits 10-11ms p50 per source with a never-exceeded ready depth of 1, so the frame is producer-paced. The default remains `identity` on absence of benefit, not presence of harm; the reopen trigger is a workload where the encode stage becomes the pacer (watch `completion_no_enqueue_wait_to_encode_dequeue` collapsing toward zero and the encode stage wall approaching the frame wall). | identity |
| `DXMT9_RENDERER_COMPAT_PROFILE` | Runtime compatibility-profile override. Unset and `progressive` enable the promoted optimizer set; `strict`, empty, `0`, and unknown values resolve to the feature-empty strict rollback. Catalogue `compat_profile` forwarding is still pending, so per-app rollback currently requires a process-environment override. | progressive |
| `DXMT9_RENDERER_FEATURES` | Comma/space/semicolon-separated modern-renderer feature list for `FrameGraphBackend`. Under the default progressive profile, unset enables only `passcoalesce`; empty or `0` disables every optimizer feature. Explicit tokens accept `passcoalesce` and opt-in `dce`; unsupported tokens are warned and ignored. Strict rejects every token. `dce` may encode a proof-independent prefix of one dequeued chunk, then selects its FIFO successor only when that source is already ready; it never waits for proof. Without a ready successor it immediately completes the current optimized permutation without cross-chunk omission. A selected successor may omit only passes whose every output has a full-overwrite proof. The production lane replays a validated full permutation or DCE ordered subset through the existing canonical `encodeChunk` path. | passcoalesce |
| `DXMT9_RENDERER_LOG_DIVERGENCE` | Enable renderer decision-divergence logging when the parity/divergence harness compares modern decisions against the traditional reference stream. | `0` |

## Frame Graph DAG debug export

The DAG dump is an observation-only channel owned by both the traditional and
framegraph backends. It reads the imported `ChunkSlot`, writes debug files, and
must not alter the Metal command stream. For 3DMark05, always scope the dump to
a narrow frame window; unscoped dumps can create thousands of files.

Use the dxmt9 trace artifact convention:

```sh
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix <tag> --frame 50 --no-gputrace --timeout 120 \
  --dump-framegraph-dag \
  --framegraph-dag-frame 50 \
  --framegraph-dag-frame-radius 2 \
  --framegraph-dag-formats json,mermaid
```

| Var | Purpose | Default |
|---|---|---|
| `DXMT9_RENDERER_DUMP_DAG` | Directory where DAG files are written as `dag-frame<N>-chunk<seq>-{pre-opt,post-opt}.<fmt>`. When unset, the observer returns early and does not build the Frame Graph. | unset |
| `DXMT9_RENDERER_DUMP_DAG_FORMATS` | Comma list of dump formats: `json`, `dot`, `mermaid`. Unknown tokens are ignored and duplicates are de-duplicated in first-seen order. | `json` |
| `DXMT9_RENDERER_DUMP_DAG_FRAME` | Optional 1-based inter-present frame filter. Positive `N` dumps only frame `N` unless a radius is also set. Unset, empty, `0`, or non-numeric values mean unfiltered. | unset |
| `DXMT9_RENDERER_DUMP_DAG_FRAME_RADIUS` | Non-negative radius around `DXMT9_RENDERER_DUMP_DAG_FRAME`; `N=50,R=2` dumps frames `48..52` with the low end clamped to `1`. | `0` |
| `DXMT9_RENDERER_DUMP_DAG_OPTIMIZE` | Analysis-only post-opt override. Tokens select optimizer passes used only for the serialized `post-opt` DAG and `framegraph_*` observe counters: `passcoalesce`, `reorder`, `dce`, `memoryless`. It does not change production Metal encoding. Unset uses the owning backend's resolved options. | unset |
| `DXMT9_RENDERER_DUMP_DAG_DRAWS` | Debug-only JSON extension. When set and the observer has the source `ChunkSlot`, each pass gains bounded `draws_detail` rows with command index, draw ordinal, primitive count, VS/PS hashes, texture mask, key render states, and stream0 stride. Dot/Mermaid output is unaffected. | `0` |

## Observe counters

These counters are emitted through the normal perf counter system when the DAG
observer runs:

| Counter | Meaning |
|---|---|
| `framegraph_dag_dumps_written` | Number of chunks whose DAG observer emitted a pre/post dump pair. |
| `framegraph_passes_built` | Number of pre-opt passes built from imported chunks. |
| `framegraph_passes_coalesced` | Number of passes merged by analysis/feature-gated passcoalesce in the post-opt observer path. |
| `framegraph_passes_dead` | Number of DCE-dropped passes in the post-opt observer path. |
| `framegraph_resources_memoryless` | Number of resources marked memoryless by the observer's post-opt classifier. |
| `framegraph_dce_dropped` | Production passes omitted by opt-in DCE. |
| `framegraph_dce_preserved_unprovable` | Production write-bearing passes retained because at least one proof gate failed. |
| `framegraph_dce_cross_chunk_proof_resources` | Canonical resources whose selected successor begins with a full Clear. |
| `framegraph_dce_replay_commands_omitted` | Validated canonical source commands omitted because their owning pass is dead. |
| `framegraph_dce_lookahead_prefixes` | Sources whose proof-independent optimized prefix was encoded before successor selection. |
| `framegraph_dce_lookahead_prefix_commands` | Commands encoded through those optimized prefixes. |
| `framegraph_dce_lookahead_selected` | Held sources that obtained a FIFO successor. |
| `framegraph_dce_lookahead_fail_open` | Held sources completed without successor proof because no FIFO successor was ready after prefix encode. |

Parallel mode adds a perf-gated production-decision counter family:

| Counter | Meaning |
|---|---|
| `parallel_pass_shadow_attempts` | Producer invocations over a validated effective replay stream. |
| `parallel_pass_shadow_candidates`, `parallel_pass_shadow_candidates_max` | Complete or rejected logical-pass candidates and the maximum candidates in one source-local batch. |
| `parallel_pass_shadow_sealed`, `parallel_pass_shadow_eligible`, `parallel_pass_shadow_eligible_max` | Boundary-complete candidates, candidates satisfying every explicit static proof, and maximum eligible passes in one batch. |
| `parallel_pass_shadow_children`, `parallel_pass_shadow_children_max` | Eligible DrawRun child-range volume and maximum children in one pass. |
| `parallel_pass_shadow_draws`, `parallel_pass_shadow_draws_max` | Eligible draw volume and maximum draws in one pass. |
| `parallel_pass_shadow_reject_{plan,boundary,command,attachment,hazard,snapshot}` | Grouped fail-closed pass-local rejection counts outside child planning and fixed storage. |
| `parallel_pass_shadow_reject_{no_two_child_work,planner_invariant,child_capacity,pass_capacity}` | Exact child-planning/storage rejections: no genuine ordered two-child grouping, malformed/overflowing or coverage-invalid planner input/output, child storage exhaustion, and pass-batch storage exhaustion. The former `reject_capacity` aggregate is derived as the sum of these four counters and is no longer emitted. |
| `parallel_pass_shadow_coordinator_boundaries`, `parallel_pass_shadow_coordinator_splits` | Non-child coordinator commands (`SurfaceCopy`, `StretchRect`, `Readback`, `ColorFill`, `DepthResolve`) met in a source, and the subset that failed a pass closed because a same-attachment draw resumed immediately after them. Every one of them stays at its serial position; only `splits` contributes to `reject_command`. |
| `parallel_pass_shadow_sealed_draws_{under8,8_15,16_31,32_63,64_127,128_255,256plus}` | Always-on draw-size histogram of every sealed pass candidate's total draw count, bucketed at the same `sealCandidate` boundary-complete site that increments `parallel_pass_shadow_sealed`, regardless of whether the candidate is later accepted or rejected. Bucket sum equals `parallel_pass_shadow_sealed` (`dxmt9-parallel-render-pass-spec` pins the conservation). Use it to calibrate `DXMT9_PARALLEL_PASS_DRAW_QUANTUM` — it shows the draw-size distribution the economics floor is being tuned against without changing any behavior. |
| `parallel_pass_worker_batches`, `parallel_pass_worker_tasks`, `parallel_pass_worker_active_peak` | Joined bounded-dispatch volume and observed peak concurrent child tasks. |
| `parallel_pass_worker_cpu_ms`, `parallel_pass_worker_wall_ms` | Summed child task CPU duration and joined batch wall duration. |
| `parallel_pass_adapter_selection_{empty,invalid_plan,invalid_economics,non_positive_benefit,arithmetic,invalid_candidate_ordinal}` | Typed selection-failure breakdown for certificate-valid adapter candidates that the proof-core selector rejected; exactly one increments per such candidate and their sum equals certificate-valid minus adapter-selected. |
| `parallel_child_{setup,body,end}_cpu_ms` / `parallel_child_{setup,body,end}_samples` | Heavy opt-in (`DXMT9_PERF_PARALLEL_CHILD_SPLIT`) per-child phase timers for calibrating the Q16.16 per-child setup term; zero when the split env is unset. |
| `parallel_pass_binding_stage1_selected`, `parallel_pass_binding_stage2b_selected` | Mutually exclusive completed parallel-pass binding selection; their sum equals `parallel_pass_selected`. |
| `parallel_pass_stage2b_children`, `parallel_pass_stage2b_draws` | Child and draw volume executed through direct-cbuf Stage 2b. |
| `parallel_pass_binding_reject_{pso_missing,stage2_table,resource_array,mixed_abi,override_rebuild}` | Mutually exclusive binding-proof rejections before parent effects. |
| `parallel_pass_economics_{considered,accepted,serial_fallback}` | Exact production decisions with `considered = accepted + serial_fallback`. |
| `parallel_pass_economics_reject_{forced_stage1,thin_child,unbalanced_child,pso_first_bind,uniform_first_bind,invalid_overflow}` | Mutually exclusive production fallback reason; their sum equals `serial_fallback`. `unbalanced_child` means maximum minus minimum child draws exceeds the existing 64-draw quantum. `forced_stage1` is retained as a conservation guard and should remain zero now that Stage 2b is preserved. |
| `parallel_pass_economics_{accepted,serial_fallback}_{draws,children}` | Draw and child volume grouped by the enforced result. |
| `parallel_pass_economics_{stage1_draws,stage2b_draws,forced_stage1_draws}` | Conserving ABI draw volume. |
| `parallel_pass_economics_{pso_boundary_transitions,uniform_boundary_transitions}` | State changes only across produced child boundaries, from the prior child last draw to the next child first draw; internal serial-order churn is excluded. |
| `parallel_pass_economics_{min_child_draws,max_child_draws,child_draw_imbalance}` | Sums of per-pass minimum, maximum, and `max - min` child draws for considered production decisions. |
| `parallel_pass_economics_min_child_{under32,32_63,64_127,128plus}` | Minimum child-size distribution. |
| `parallel_pass_economics_children_{2,3_4,5_8,9_16}` | Pass child-count distribution. |
| `parallel_pass_adapter_{considered,certificate_valid,certificate_invalid,selected,serial_fallback}` | Policy proof-core adapter outcomes at the single production call site. Conserves `considered = certificate_valid + certificate_invalid` and `considered = selected + serial_fallback`, with `selected <= certificate_valid`. A certificate-invalid candidate never reaches selection and an unselected candidate never reaches child creation. |
| `parallel_pass_adapter_certificate_invalid_{missing_snapshot,source_identity,pass_identity,coordinator_proof,attachment_proof,resource_proof,first_draw_proof,child_capacity,child_plan,coverage,arithmetic}` | Typed one-to-one breakdown of `parallel_pass_adapter_certificate_invalid`, keyed by the `ParallelPassSemanticPlanFailure` checkpoint in `validateParallelPassSemanticPlan` (`src/dxmt9/dxmt9_parallel_render_pass.hpp`) that rejected the candidate. Exactly one reason counter increments per invalid certificate, so the eleven fields sum to the aggregate (`dxmt9-parallel-render-pass-spec` pins the conservation). None are merged: each checkpoint is a distinct proof stage — missing input, owner-resolved source identity, pass/epoch identity, coordinator proof, attachment shape, exact resource-set shape, per-child first-draw provenance, child-count/array capacity, child-plan (binding/ABI) shape, and exact coverage fold — plus a reserved `arithmetic` slot for the enum's fixed-point overflow checkpoint, which is not reachable from the current validator body. Added to attribute the 46-49 untyped GT2 invalids and 1,630 untyped SFIV quantum-8 invalids observed before this breakdown existed. The typed re-measurement (2026-08-17, `23430351`) put **all** of both residuals on `pass_identity` alone — GT2 47/47, SFIV quantum-8 1,387/1,387, every other reason zero — and that checkpoint owned two false negatives, both since fixed: a zero epoch seed published for sources carrying an open encode session, and a pass sealed by an attachment change rather than by a coordinator command. A wild re-measurement against the fix is outstanding, so expect these counters to move; see `specs/backend/encode-scheduling/gap.md`. |

When perf counters are disabled, counter clocks and atomics are skipped, but an
explicit `parallel` request still performs the proof, economics policy, and
accepted execution work. These
counters describe an opt-in production provider; they do not claim default
promotion or a performance improvement.

## Current frontier

- The DAG dump is accepted tooling for render-pass-store H6: it makes
  same-attachment re-entry, RAW/WAR/WAW ordering, and no-intervening-writer
  safety machine-decidable on real GT1 frames.
- Strict L1 remains byte-identical: production encode delegates to the
  source-order `encodeChunk` path. `DXMT9_RENDERER_DUMP_DAG_OPTIMIZE` remains
  observation-only regardless of backend/profile.
- The default `framegraph + progressive + passcoalesce` lane changes only
  source-command order. It rejects a merge whose second pass begins with a
  Clear/helper boundary, requires a complete duplicate-free command
  permutation, evaluates load/store lookahead in that validated replay order,
  and falls back to source order on any planning/session mismatch.
- The alias-aware GT1/GT2/GT3 wild runs, exact GT3 glitch-window rerun, and
  env-clean SFIV rendered-scene/stability runs are the promotion evidence.
  Device-backed pixel parity remains evidence debt, not a reason to enable any
  additional optimizer. DCE is implemented only as an explicit token and still
  lacks wild/pixel/performance promotion evidence. Memoryless, reorder, mesh,
  and GPU-driven execution remain unimplemented production features.
