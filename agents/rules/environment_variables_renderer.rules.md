# dxmt9 Environment Variables - Renderer / Frame Graph

Part of the [`environment_variables.rules.md`](environment_variables.rules.md)
index. This file covers backend selection, modern-renderer feature gates, and
Frame Graph DAG debug export. A flag is "set" when its value is a non-empty
string that is not `0`, unless documented otherwise.

## Backend selection

| Var | Purpose | Default |
|---|---|---|
| `DXMT9_RENDER_MODE` | Select the unix-side render backend. Unset, empty, `0`, `traditional`, and unknown values resolve to `TraditionalBackend`; `framegraph` selects `FrameGraphBackend`. `scripts/run_apps/run_experiment.py` also maps catalogue `render_mode` to this env before launching Wine. | traditional |
| `DXMT9_RENDERER_FEATURES` | Comma/space/semicolon-separated modern-renderer feature list for `FrameGraphBackend`. In the current strict L1 state, any token is rejected with one warning and ignored; the DAG observe/export side-channel is independent of this feature list. | unset |
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

## Current frontier

- The DAG dump is accepted tooling for render-pass-store H6: it makes
  same-attachment re-entry, RAW/WAR/WAW ordering, and no-intervening-writer
  safety machine-decidable on real GT1 frames.
- Production encode still uses the traditional `encodeChunk` path in the
  current strict L1 state. `DXMT9_RENDERER_DUMP_DAG_OPTIMIZE=passcoalesce`
  changes only the exported post-opt DAG and observe counters, not rendered
  pixels or GPU commands.
- Device-gated work remains: driving Metal encode through
  `fg_linearizer::executeLinearization`, proving byte-identical passcoalesce,
  then measuring whether tile preservation and Xcode store/load traffic drop.
