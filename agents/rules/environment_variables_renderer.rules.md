# dxmt9 Environment Variables - Renderer / Frame Graph

Part of the [`environment_variables.rules.md`](environment_variables.rules.md)
index. This file covers backend selection, modern-renderer feature gates, and
Frame Graph DAG debug export. A flag is "set" when its value is a non-empty
string that is not `0`, unless documented otherwise.

## Backend selection

`DXMT9_RENDER_MODE`, the compatibility profile, and the feature-token names are
stable provider contracts under `specs/backend/render-provider/spec.md`.
Activation state is separate: `passcoalesce` is default, `dce` is implemented
opt-in, and the remaining named feature tokens are planned/unavailable. Debug
export and divergence flags below are diagnostic surfaces, not provider modes.

| Var | Purpose | Default |
|---|---|---|
| `DXMT9_RENDER_MODE` | Select the unix-side render backend. Unset and `framegraph` resolve to `FrameGraphBackend`. Empty, `0`, `traditional`, and unknown values resolve to the conservative `TraditionalBackend` rollback. `scripts/run_apps/run_experiment.py` maps an omitted catalogue `render_mode` to `framegraph` unless the process environment explicitly overrides it for a diagnostic run. | framegraph |
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
