---
domain: mini-replay-bisection
workload: 3DMark05 GT1
title: "Mini-Replay Bisection — the apparatus that isolated per-draw vertex-stage write amplification - Historical Log"
type: domain-log
status: historical
updated: 2026-07-08
source: docs/perfomance/mini-replay-bisection/index.md
related: docs/perfomance/mini-replay-bisection/index.md; docs/perfomance/mini-replay-bisection/overview.md
---

# Mini-Replay Bisection — the apparatus that isolated per-draw vertex-stage write amplification - Historical Log

> Full historical detail moved from the former top-level `mini-replay-bisection.md` overview.
> Keep [overview](overview.md) current and compact; append long-running chronology,
> rejected paths, and detailed synthesis here only when it is not already captured in
> one-experiment leaf documents.

---

# Mini-Replay Bisection — the apparatus that isolated per-draw vertex-stage write amplification

> Part of the 3DMark05 GT1 GPU-bottleneck investigation. Root map: [overview-3dmark05-gt1](../overview-3dmark05-gt1.md).

## Scope & question

This domain owns the **methodology**: build a standalone, row-local mini-replay
harness that consumes real captured index/stream/cbuf/depth payloads plus dumped
shaders, reproduce the original hot-encoder `VS Buffer Device Memory Bytes Written`
pressure at encoder scale outside Wine/D3D9, then bisect it down to individual
shader-pair / draw windows. It answers: *which concrete inputs reproduce the GT1
hidden vertex-stage write bucket, and at what granularity is it owned?* This is
the apparatus that made [tvb-mechanism-proof](../tvb-mechanism-proof/index.md) possible and that backs the
[index-cache-locality](../index-cache-locality/index.md) win.

## Hypotheses & verdicts

| # | Hypothesis | Verdict | Evidence |
|---|-----------|---------|----------|
| H1 | Hot-row attribution / shaders / draw identity are ready; only geometry bytes are missing | tooling (gap found) | [mini-replay-bisection-harness.01](mini-replay-bisection-harness.01.md) |
| H2 | Runtime can dump replayable index/stream/cbuf bytes for a hot window without mutating state | tooling | [mini-replay-bisection-payload.01](mini-replay-bisection-payload.01.md) |
| H3 | Runner can replay a multi-PSO slice with stream1 + dynamic cbuf slots | tooling | [mini-replay-bisection-harness.02](mini-replay-bisection-harness.02.md) |
| H4 | Captured payloads alone reproduce the original ~1 GiB VS-write scale/shape | inconclusive (class yes, scale no) | [mini-replay-bisection-replay.01](mini-replay-bisection-replay.01.md) |
| H5 | Fragment overdraw (missing per-draw scissor) is the gap | rejected (real fix, but VS write unmoved) | [mini-replay-bisection-replay.02](mini-replay-bisection-replay.02.md) |
| H6 | Depth attachment content (clear scalar or real D24X8) owns the amplification | rejected | [mini-replay-bisection-depth.01](mini-replay-bisection-depth.01.md) |
| H7 | The wider encoder2 (113-draw) sequence reproduces vertex-stage dominance | accepted | [mini-replay-bisection-replay.03](mini-replay-bisection-replay.03.md) |
| H8 | The pressure is a single late draw/state transition | rejected (additive, independent windows) | [mini-replay-bisection-bisect.01](mini-replay-bisection-bisect.01.md) |
| H9 | The cost is per-draw geometry/shader-pair amplification, not alpha/scissor | accepted | [mini-replay-bisection-pair.01](mini-replay-bisection-pair.01.md) |
| H10 | Broad depth-read reorder can be made production-shaped with current runtime selectors | rejected | [mini-replay-bisection-semantic.01](mini-replay-bisection-semantic.01.md) |
| H11 | A selected `60/2 depth-read + no-alpha-blend` cache-opt window can preserve same-input final color | accepted (scoped) | [mini-replay-bisection-semantic.02](mini-replay-bisection-semantic.02.md) |
| H12 | Draw-time texture sidecars can remove the current white-texture replay caveat | tooling | [mini-replay-bisection-texture.01](mini-replay-bisection-texture.01.md) |
| H13 | The same selected window remains exact after real texture inputs are supplied | rejected (tiny delta, final-writer hazard) | [mini-replay-bisection-texture.02](mini-replay-bisection-texture.02.md) |
| H14 | Lower-ranked same-shape windows can decide whether rank 1 was an isolated hazard or the whole reorder class is unsafe | accepted: rank 1 is visible-fail; ranks 2-4 are color-exact owner-masked | [mini-replay-bisection-texture.03](mini-replay-bisection-texture.03.md), [mini-replay-bisection-texture.04](mini-replay-bisection-texture.04.md), [mini-replay-bisection-texture.05](mini-replay-bisection-texture.05.md), [mini-replay-bisection-texture.06](mini-replay-bisection-texture.06.md) |
| H15 | The rank-2 same-shape window remains exact with real depth and textures | accepted for color only; owner-masked | [mini-replay-bisection-texture.04](mini-replay-bisection-texture.04.md) |
| H16 | The rank-3 same-shape window remains exact with real depth and textures | accepted for color only; owner-masked | [mini-replay-bisection-texture.05](mini-replay-bisection-texture.05.md) |
| H17 | The rank-4 same-shape window remains exact with real depth and textures | accepted for color only; owner-masked | [mini-replay-bisection-texture.06](mini-replay-bisection-texture.06.md) |
| H18 | Primitive-conflict metrics can split rank-1 visible failure from rank2-4 exact owner-masked passes | rejected; only final-color metrics separate | [mini-replay-bisection-texture.07](mini-replay-bisection-texture.07.md) |
| H19 | Existing D3D9 occlusion query or winemetal visibility plumbing can supply the missing runtime oracle as-is | rejected as production oracle; diagnostic scout added separately | [mini-replay-bisection-texture.08](mini-replay-bisection-texture.08.md) |
| H20 | A diagnostic Metal visibility scout can supply per-Metal-draw sample-visible counts after GPU completion | implemented diagnostic; not final-color proof; old rank-1 `36..37` is sample-visible | [mini-replay-bisection-texture.09](mini-replay-bisection-texture.09.md) |
| H21 | No-sample visibility rows are the hidden-backend hot locality owner | rejected; zero rows are small and only `-2,016` of `-182,856` LRU32 delta | [mini-replay-bisection-texture.10](mini-replay-bisection-texture.10.md) |
| H22 | Positive Metal visibility samples can be used as the scoped depth-read final-color oracle | rejected; rank2 has `39,835` samples but `0` final-color pixels, and rank1/rank3 are both sample-positive but fail/pass diverge | [mini-replay-bisection-texture.11](mini-replay-bisection-texture.11.md) |

## Verification methods

- **`plan_3dmark05_mini_replay.py`** — joins joined-summary + shader-dump +
  indexed probe CSVs into a readiness table; `--geometry-dir` validates payload
  triplets. Proves what inputs exist before a replay.
- **`--dump-indexed-geometry` / `--dump-indexed-geometry-cbufs`** (probe wrapper)
  — implies `DXMT9_MEASURE_INDEX_REUSE=1`, does not mutate primitive order,
  writes `.index.bin` / `.stream0.bin` / `.streamN.bin` / `.vsconsts.bin` /
  `.psconsts.bin` / `.ffpvs.bin` / `.ffpps.bin` + `.meta` under
  `analysis/geometry/`. Shader filters `--dump-indexed-geometry-vs/-ps` select a
  shader pair (row-local draw windows are not robust cross-run selectors).
- **`DXMT9_DUMP_DEPTH_ATTACHMENT_HANDLE` / `_PATH`** (wrapper
  `--dump-depth-attachment-handle/-seq/-enc/-path`) — blits a live GPU-side
  depth/stencil target to a readback buffer with `.json` metadata, capturing
  D24X8 that the old BMP `DXMT_DUMP_GPU_TEXTURE_*` upload hook cannot.
- **`DXMT9_DUMP_DRAW_TEXTURE_HANDLES` / `_DIR`** (wrapper
  `--dump-draw-texture-handles/-seq/-enc/-dir`) — blits live shader-read
  texture views to raw sidecars under `analysis/textures/`, preserving
  per-level/per-slice `formatRowPitch` / `formatByteSize` metadata for
  compressed, luminance, float, sRGB, and cube inputs. This is the input path
  for removing the current mini-replay `whiteTexture` fallback.
- **`run_3dmark05_mini_replay.py`** — rewrites dumped MSL off `buffer(30)` argbuf
  into standalone cbuf slots, scans bindings to pick free slots, compiles an
  Obj-C++ Metal app, binds real payloads, multi-PSO, per-draw scissor,
  `--depth-clear`, `--depth-input`, `--texture-input-dir`,
  `--primitive-order`, `--draw-order`, `--color-output` PPM readback,
  `--capture-path`.
- **`run_3dmark05_semantic_replay_gate.py`** — standardized candidate gate that
  runs original/candidate color replays, primitive-id replays, exact/`lsb1`
  image comparison, canonical original-triangle owner comparison, and optional
  GT1 primitive-conflict analysis. This is the repeatable path for future
  payload windows before spending production Xcode budget.
- **`build_3dmark05_mini_replay_manifest.py` texture summary** — after a fresh
  geometry capture, `summary.texture_capture_handles_arg` and
  `summary.texture_capture_flags` provide the exact
  `--dump-draw-texture-handles/-seq/-enc` flags for the matching texture
  sidecar run. This keeps rank2+ gates out of manual `.meta` scraping.
- **`--encoder-draw-indices`** (manifest builder) — captures non-contiguous
  encoder-local draws (e.g. a shader pair `[14,15,18,19,21]`) without widening the
  geometry gate.
- **Software LRU cache estimate vs Xcode counters** — the runner's LRU16/32/64
  miss estimate is cross-checked against Xcode `VS Invocations` /
  `VS Buffer Device Memory Bytes Written` so reorder candidates are accepted only
  when software misses and Xcode counters drop together.

## Experiment dependency graph

```mermaid
flowchart TD
  Plan["readiness-planner\nhot-row ready, geometry bytes MISSING"] --> Pay["payload-scout\n--dump-indexed-geometry(+cbufs)\n60/2 win 71..188, 16 triplets"]
  Pay --> Multi["multi-PSO-harness\nstream1 + dynamic cbuf slots\n6-draw smoke OK"]
  Multi --> R16["16-draw-replay\nVS write 31.974MiB (class, not scale)\nfragment-dominated"]
  R16 --> Scissor["scissor-fix\nGPU 3.71->1.50ms, FS 22.1M->3.0M\nVS write unchanged 31.98MiB"]
  Scissor --> Depth["depth-probes\ndepth=0 + raw D24X8\nVS write ~31.98MiB"]
  Depth --> R113["wider-encoder2 (113-draw)\nGPU 18.115ms, VS write 1090.901MiB\n1710B/VS inv"]
  R113 --> Bisect["bisection\n0..13 cold, 14..27 hot 4026B/inv\nadditive independent sources"]
  Bisect --> Pair["pair-local\nfea7/a091 + dee2/2f20\n99.8% with 7/14 draws"]
  Pair --> TVB"feeds [tvb-mechanism-proof"]
  Pair --> Semantic["semantic.01\nfinal-color runtime blocker\nbroad depth-read rejected"]
  Semantic --> ScopedSemantic["semantic.02\n60/2 depth-read/no-blend\n2-draw exact replay\nscoped proof only"]
  ScopedSemantic --> TextureSidecar["texture.01\ndraw-time raw texture sidecars"]
  TextureSidecar --> TextureReplay["texture.02\nreal textures + real depth\n2 px color delta\n7 final-writer pixels changed"]
  TextureReplay --> RankQueue["texture.03\nrank2+ real-texture gate queue"]
  RankQueue --> Rank2["texture.04\nrank2 real texture\n0 color delta\n809 owner pixels changed"]
  RankQueue --> Rank3["texture.05\nrank3 real texture\n0 color delta\n52 owner pixels changed"]
  RankQueue --> Rank4["texture.06\nrank4 real texture\n0 color delta\n17 owner pixels changed"]
  TextureReplay --> SelectorScout["texture.07\nselector scout\nnon-color metrics overlap\nfinal-color oracle required"]
  Rank2 --> SelectorScout
  Rank3 --> SelectorScout
  Rank4 --> SelectorScout
  SelectorScout --> OcclusionGate["texture.08\nocclusion oracle feasibility\nD3D9 query primitive-count only"]
  OcclusionGate --> VisibilityScout["texture.09\nMetal visibility scout wired\nper-draw visible sample counts"]
  VisibilityScout --> VisibilityCache["texture.10\nvisibility + cache join\nno-sample rows are not hot"]
  VisibilityCache --> VisibilitySemantic["texture.11\nvisibility + semantic join\npositive samples are not final color"]
  ScopedSemantic --> ICL"feeds [index-cache-locality\nexplicit screen-blend + scoped depth-read candidates"]
  TextureReplay --> ICL
  Rank2 --> ICL
  Rank3 --> ICL
  Rank4 --> ICL

  classDef accepted fill:#d6f5d6,stroke:#2b7a2b,color:#063
  classDef rejected fill:#f8d7da,stroke:#a33,color:#600
  classDef open fill:#fff3cd,stroke:#a80,color:#640
  classDef tooling fill:#e8f0ff,stroke:#476cb6,color:#0d1833

  class Plan,Pay,Multi tooling
  class R16 open
  class Scissor,Depth rejected
  class R113,Bisect,Pair,TVB,ScopedSemantic accepted
  class Semantic,TextureReplay,OcclusionGate rejected
  class TextureSidecar,VisibilityScout tooling
  class RankQueue,Rank2,Rank3,Rank4,ICL,SelectorScout open
```

## Results synthesis

Settled: a standalone mini-replay reproduces the **class** of GT1 VS-write traffic
from captured payloads, but the **scale and shape** require the wider encoder2
sequence. The 3-draw / 16-draw slices stayed at ~32 MiB and fragment-dominated;
fixing per-draw scissor and supplying real D24X8 depth corrected fragment work but
left VS buffer write fixed at ~31.98 MiB — cleanly **ruling out fragment overdraw
and depth content as artifacts/owners**. The full 113-draw encoder2 replay finally
matched the hot row (18.115 ms, 1090.901 MiB, 1710 B/VS inv vs original 20.327 ms,
981.171 MiB, 1602.5 B/VS inv). Bisection showed the pressure is **additive across
independent draw windows** (not a single transition), and pair-local captures
localized 99.8% of the first hot window to two shader pairs / large indexed draws
with per-draw additivity within 0.44% — i.e. **per-draw vertex-stage write
amplification keyed by geometry × shader pair**, with named tiled counters far
below the VS-write bucket. An actual-read VSOut liveness trim moved VS write only
-0.01%, so visible varying width is not the owner ([vsout-layout](../vsout-layout/index.md)).

This is the apparatus that **isolated the per-draw vertex-stage write
amplification** and so enabled the [tvb-mechanism-proof](../tvb-mechanism-proof/index.md) (geometry-locked
original-vs-`cache-opt-lru32` replays) and the accepted
[index-cache-locality](../index-cache-locality/index.md) production win. **Firmware-TVB caveat:** small standalone
replays can legitimately report `0 MiB` for `VS Buffer Device Memory Bytes Written`
because the firmware Parameter Buffer never spills at that scale (the `0..13`
prefix is the local example) — this is why scale, not just class, had to be
reproduced before the mechanism could be proven; see [tvb-mechanism-proof](../tvb-mechanism-proof/index.md).

The same apparatus now also supplies the negative semantic proof for broad
depth-read reorder. [mini-replay-bisection-semantic.01](mini-replay-bisection-semantic.01.md) shows that useful
visible exact movement and a real final-color hazard share the same current
runtime-visible state/geometry/shader fields. That keeps `50/2` screen-blend in
the explicit exact/`lsb1` bucket and blocks broad depth-read promotion until a
real final-color/final-writer policy exists; Metal visibility can only triage
no-sample cases unless paired with that policy. The
post-visualfix follow-up [mini-replay-bisection-semantic.02](mini-replay-bisection-semantic.02.md) adds a narrower
positive result:
a selected `60/2 depth-read + no-alpha-blend` two-draw window is exact under the
standalone same-input replay while cutting LRU32 misses by `-14,593` (`-27.6%`).
The captured D24X8 depth input stayed exact, so depth content is not the caveat
for this selected window. The real-texture follow-up then supplied the selected
window's actual `R32F` / `L8` / `L16` / `DXT1` / `DXT5` / cube inputs and
rejected exact promotion: only `2 / 786,432` pixels changed, but max delta `5`
fails both exact and `lsb1`. A canonical primitive-id replay showed `7` pixels
with a changed original-triangle final writer, so this is a
depth-read/depth-write-off order hazard rather than same-primitive texture
noise. Ranks 2-4 then kept final color exact with real textures while still
changing canonical primitive ownership (`809`, `52`, and `17` pixels). The
locality ceiling remains real, but the current runtime selector is not
production-safe; color-only exactness and owner-stable exactness are separate
proof levels. The primitive-conflict selector scout
([mini-replay-bisection-texture.07](mini-replay-bisection-texture.07.md)) then checked whether owner-count,
both-cover, depth, UV, or projected-texcoord metrics can split rank 1 from
ranks 2-4; all non-color ranges overlap. The only separating signal is final
color itself, so this reorder line now requires final-color/final-writer proof
before production promotion; the later visibility/cache join rejects current
no-sample rows as the hot owner.
[mini-replay-bisection-texture.08](mini-replay-bisection-texture.08.md) then audits the obvious runtime reuse path
and rejects it for the current
implementation: dxmt9's D3D9 occlusion query resolves submitted primitive count,
not framebuffer visibility, while the winemetal visibility buffer/mode ABI is
present but not wired into dxmt9 draw encoding or any delayed feedback loop.
[mini-replay-bisection-texture.09](mini-replay-bisection-texture.09.md) follows by wiring that missing diagnostic
path: selected render encoders can now allocate a shared Metal visibility buffer,
toggle `Counting` around each actual Metal draw, and append per-draw
`visible_samples` after GPU completion. This is still not a final-color gate:
zero counts are useful no-sample evidence, but positive counts need
final-color/final-writer or semantic replay proof before any production reorder.
The first `60/2` scout lowers the old rank-1 hypothesis because draw window
`36..37` is positive-positive and all `large4096` buckets are sample-visible.
[mini-replay-bisection-texture.10](mini-replay-bisection-texture.10.md) joins the scout with cache-candidate
measurement and closes the no-sample hotpath variant: zero rows account for only
`7,344` primitives and `-2,016` LRU32 delta, while sample-visible rows carry
`382,032` primitives and `-180,840` LRU32 delta.
[mini-replay-bisection-texture.11](mini-replay-bisection-texture.11.md) then joins those sample counts back to the
ranked final-color semantic payloads. It rejects positive visibility as the
missing oracle: rank2 is sample-positive (`39,835` samples) but has no final
color, while rank1 and rank3 are both sample-positive but split into visible
fail versus visible exact-pass. This turns the current experiment into a proof
gate rather than an optimization: it stops further locality Xcode spend unless a
real final-color/final-writer predicate or a non-reorder backend mechanism
appears.

## How to run
Every experiment here is a 3DMark05 GT1 run. The pipeline is three stages: dump
the raw indexed geometry from a frame, build a row-local manifest, then replay it
standalone with the order/depth variant under test:

```sh
# 1. Dump raw index + stream0 (+ cbufs) payloads from the hot encoder:
bash scripts/tools/run_3dmark05_perf_probe.sh --suffix dump --frame 60 --no-gputrace \
  --encoder-breakdown-seq 60 --dump-indexed-geometry --dump-indexed-geometry-cbufs \
  --dump-indexed-geometry-max-draws 16 --timeout 180

# 2. Build the mini-replay manifest from the reduced artifacts:
python3 scripts/tools/build_3dmark05_mini_replay_manifest.py \
  --geometry-dir traces/<run>/analysis/geometry --probe-draws <probe-draws.csv> \
  --shader-summary <shader-dump-summary.csv> --encoder-draw-indices 71,72,73 \
  --output traces/<run>/analysis/mini-replay-manifest.json

# 2b. If the replay needs real textures, use the manifest-generated flags:
jq -r '.summary.texture_capture_flags | @sh' \
  traces/<run>/analysis/mini-replay-manifest.json

# 3. Replay standalone with the order/depth variant + capture for Xcode counters:
python3 scripts/tools/run_3dmark05_mini_replay.py traces/<run>/analysis/mini-replay-manifest.json \
  --output-dir traces/<run>/mini --compile --run --primitive-order original \
  --depth-input traces/<run>/analysis/frame60-2-depth.bin \
  --capture-path traces/<run>/mini/mini.gputrace

# Optional: capture draw-time texture sidecars for the selected 60/2 window:
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix post-visualfix-frame60-60-2-textureinput-r1 \
  --frame 60 --encoder-breakdown-seq 60 --no-gputrace --timeout 120 --top 5 \
  --dump-draw-texture-handles \
    0x20000010000008d,0x200000100000072,0x200000100000001,0x200000100000074,0x200000100000007,0x200000100000003,0x200000100000070,0x20000010000007e,0x200000100000071 \
  --dump-draw-texture-seq 60 --dump-draw-texture-enc 2

# Optional: replay the same window with captured depth + real texture sidecars:
python3 scripts/tools/run_3dmark05_mini_replay.py traces/<run>/analysis/mini-replay-manifest.json \
  --output-dir traces/<run>/mini-real-texture --compile --run \
  --primitive-order cache-opt-lru32 \
  --depth-input traces/<depth-run>/analysis/frame60-depth.bin \
  --texture-input-dir traces/<texture-run>/analysis/textures \
  --color-output traces/<run>/mini-real-texture/cache-opt.ppm
```

The exact per-experiment flags (draw windows, `--primitive-order` choices,
`--depth-clear`) live in each leaf's `**Method.**` field. See
`agents/rules/environment_variables.rules.md` for env-var meanings and
`agents/rules/metal_debugging.rules.md` for the full workflow.

## Cross-references

- [tvb-mechanism-proof](../tvb-mechanism-proof/index.md) — the accepted mechanism this apparatus enabled; PB-spill / 0 MiB caveat.
- [index-cache-locality](../index-cache-locality/index.md) — the production win validated by geometry-locked mini-replays.
- [hidden-backend-storage](../hidden-backend-storage/index.md) — the TVB model the replays measure (named tiled << VS write).
- [primitive-reorder-diagnostics](../primitive-reorder-diagnostics/index.md) — sort-min-index / cache-opt order knobs tested under geometry-locked replay.
- [vsout-layout](../vsout-layout/index.md) — pair-liveness VSOut trim rejected (-0.01%) inside the hot window.
- [render-pass-store](../render-pass-store/index.md) — depth re-entry / attachment content rejected as owner.
- [mini-replay-bisection-semantic.01](mini-replay-bisection-semantic.01.md) — final-color/runtime blocker for broad depth-read reorder.
- [mini-replay-bisection-semantic.02](mini-replay-bisection-semantic.02.md) — scoped `60/2` depth-read/no-blend exact replay candidate.
- [mini-replay-bisection-texture.08](mini-replay-bisection-texture.08.md) — current occlusion/visibility path is not the missing runtime oracle.
- [mini-replay-bisection-texture.09](mini-replay-bisection-texture.09.md) — diagnostic Metal visibility scout wiring.
- [mini-replay-bisection-texture.10](mini-replay-bisection-texture.10.md) — visibility scout + cache join rejects no-sample rows as the hot locality owner.
- [mini-replay-bisection-texture.11](mini-replay-bisection-texture.11.md) — visibility-positive semantic join rejects positive samples as the final-color oracle.
- [mini-replay-bisection-texture.01](mini-replay-bisection-texture.01.md) — draw-time raw texture sidecar input for real-texture replay.
- [mini-replay-bisection-texture.02](mini-replay-bisection-texture.02.md) — real-texture replay rejects exact/`lsb1` promotion for the selected `60/2` window.
- [mini-replay-bisection-texture.03](mini-replay-bisection-texture.03.md) — ranked real-texture semantic gate queue after rank 1 rejects.
- [mini-replay-bisection-texture.04](mini-replay-bisection-texture.04.md) — rank-2 real-texture gate is color-exact but owner-masked.
- [mini-replay-bisection-texture.05](mini-replay-bisection-texture.05.md) — rank-3 real-texture gate is also color-exact but owner-masked.
- [mini-replay-bisection-texture.06](mini-replay-bisection-texture.06.md) — rank-4 real-texture gate completes the queued set as color-exact owner-masked.
- [mini-replay-bisection-texture.07](mini-replay-bisection-texture.07.md) — primitive-conflict selector scout rejects simple non-color thresholds.
- [overview-3dmark05-gt1](../overview-3dmark05-gt1.md) — root map and priority DAG.

## Root 3DMark05 Map Detail Migration - 2026-07-08

Detail migrated from the former long-form root [3DMark05 overview](../overview-3dmark05-gt1.md) so that `mini-replay-bisection` owns its detailed synthesis while the root overview stays cross-domain only.

### From Frame shape

The current canonical A/B baseline is frame50 normal-source
([baselines-frame50.01](../baselines/baselines-frame50.01.md)): **35.024 ms**, top-3 98.19%, rows 50/2
(56.9%) / 50/1 (24.5%) / 50/0 (16.8%), hidden backend estimate
≈1597.6 MiB. Mid-investigation probes A/B against frame60
([baselines-frame60.01](../baselines/baselines-frame60.01.md)). The current post-visualfix frame60 refresh
([baselines-frame60.02](../baselines/baselines-frame60.02.md)) keeps the same owner after the latest visual/cbuf
identity path: **33.614 ms**, top-3 **32.984 ms / 98.12%**, VS write
**1627.332 MiB**, hidden backend **1597.755 MiB**. Its no-mutate class proxy
([hidden-backend-storage-shape.04](../hidden-backend-storage/hidden-backend-storage-shape.04.md)) splits residual `60/2` into depth-read,
screen-blend, and standard-alpha classes with `~111-128 MiB` proxy hidden
backend each and `~25-28%` candidate LRU32 reduction. A scoped follow-up
([mini-replay-bisection-semantic.02](mini-replay-bisection-semantic.02.md)) proved one `60/2 depth-read +
no-alpha-blend` two-draw candidate exact under standalone same-input replay
(`0` changed pixels, replay LRU32 `-27.6%`) with clear depth and captured D24X8
depth. The rank-1 real-texture replay ([mini-replay-bisection-texture.02](mini-replay-bisection-texture.02.md))
then supplied the missing sampled inputs and rejected exact promotion:
`2 / 786,432` pixels changed, max delta `5`, so the candidate fails both exact
and `lsb1`. A canonical primitive-id replay shows `7` final-writer pixels
changed, confirming a depth-read/depth-write-off order hazard rather than
same-primitive texture noise. The rank-2 follow-up
([mini-replay-bisection-texture.04](mini-replay-bisection-texture.04.md)) keeps final color exact while cutting
LRU32 `19,131 -> 13,194` (`-31.0%`), but canonical primitive ownership still
changes at `809` pixels. Rank 3 ([mini-replay-bisection-texture.05](mini-replay-bisection-texture.05.md)) repeats
the color-exact owner-masked shape with LRU32 `11,398 -> 8,946` (`-21.5%`) and
`52` owner pixels changed. Rank 4 ([mini-replay-bisection-texture.06](mini-replay-bisection-texture.06.md)) also
keeps final color exact with LRU32 `4,237 -> 3,513` (`-17.1%`) and `17` owner
pixels changed. This keeps a stricter selector or Metal visibility scout path
alive for triage, while ruling out a broad same-state-class promotion. The
primitive-conflict selector
scout ([mini-replay-bisection-texture.07](mini-replay-bisection-texture.07.md)) then checks the cheap threshold
family directly: owner-count, depth, UV, and projected-texcoord ranges all
overlap between the visible rank-1 failure and the rank2-4 exact passes. Only
final-color metrics separate the rows, so a real final-color/final-writer policy
is required before any further reorder promotion; Metal visibility can only
triage no-sample cases unless paired with that policy.
The follow-up occlusion feasibility audit
([mini-replay-bisection-texture.08](mini-replay-bisection-texture.08.md)) rejects the current implementation as
that oracle: D3D9 occlusion query resolution is
primitive-count based. The diagnostic Metal visibility scout
([mini-replay-bisection-texture.09](mini-replay-bisection-texture.09.md)) is now wired into dxmt9 draw encoding and
can export per-Metal-draw sample counts after GPU completion, but its first
`60/2` pass shows the old rank-1 `36..37` window and all `large4096` buckets are
sample-visible. The follow-up cache join ([mini-replay-bisection-texture.10](mini-replay-bisection-texture.10.md))
shows zero-sample rows are small `596`-primitive buckets and account for only
`-2,016` of `-182,856` LRU32 delta. That makes visibility useful for no-sample
triage, not a final-color oracle or the hot hidden-backend owner. The semantic
visibility join ([mini-replay-bisection-texture.11](mini-replay-bisection-texture.11.md)) closes the positive side:
rank2 is sample-positive (`39,835` samples) but has no final color, and rank1
and rank3 are both sample-positive but split visible fail versus visible
exact-pass. The current
post-rank4 perf gate
([hidden-backend-storage-shape.05](../hidden-backend-storage/hidden-backend-storage-shape.05.md)) folds this into the Xcode spend policy:
depth-read reorder is blocked by oracle requirements, while non-reorder
backend-shape work needs a bytes-per-invocation preflight. The
first offline shader preflight ([hidden-backend-storage-shape.06](../hidden-backend-storage/hidden-backend-storage-shape.06.md)) says the
top `60/2` and `60/1` rows are not promising visible-VSOut-width retries:
`live-vsout` cuts their IR return to `36 B`, but leaves `128 B` scratch. The
rank3 `60/0` row is the only hot row where `live-vsout` also removes visible
scratch, so it became the narrow primitive-order-preserving smoke candidate.
The scoped Xcode follow-up ([hidden-backend-storage-shape.08](../hidden-backend-storage/hidden-backend-storage-shape.08.md)) rejects that
candidate as a bottleneck fix: `60/0` expected VSOut falls `184 B -> 68 B`, but
VS buffer write remains `224.947 MiB -> 224.990 MiB` and bytes/invocation
remains `1542.722 -> 1543.013`. This closes visible `VSOut` width as the
non-reorder denominator lever. [hidden-backend-storage-shape.09](../hidden-backend-storage/hidden-backend-storage-shape.09.md) makes the next
budget rule explicit: no more visible-width Xcode retries; spend only on a legal
below-AIR state/parameter-shape hypothesis, final-color/final-writer proof for
sample-visible locality, or a real backend escape path. The
automated gate refresh ([hidden-backend-storage-shape.13](../hidden-backend-storage/hidden-backend-storage-shape.13.md)) then closes the
stale `live-vsout` shader-smoke queue as `closed-by-xcode-gate`, so the
automation no longer schedules another visible-output smoke after the matching
Xcode rejection. The follow-up alpha static-equivalence gate
([hidden-backend-storage-shape.10](../hidden-backend-storage/hidden-backend-storage-shape.10.md)) rejects the naive legal shortcut:
current `60/2` large alpha rows are `15` draws / `154,761` primitives split
across screen and standard-alpha blend classes, and none can disable blending
with static color equivalence. The PSO/state churn preflight
([hidden-backend-storage-shape.11](../hidden-backend-storage/hidden-backend-storage-shape.11.md)) then rejects the current hot rows as an
isolated Xcode candidate: `60/2` has `47` PSO changes, but the same row has
`271` stream-handle changes and `160` IB-handle changes, so current evidence
points at stream/IB and geometry locality before PSO/backend spill. The
stream/IB preflight ([state-churn-encode-stream.04](../state-churn-encode/state-churn-encode-stream.04.md)) confirms that this
state-motion signal is handle-dominant (`60/2` combined handle changes/draw
`2.305`, binding tuple changes `160/187`, stream1 extra changes `111`) and not
offset/stride noise. The row-scoped staging A/B
([state-churn-encode-stream.08](../state-churn-encode/state-churn-encode-stream.08.md)) keeps the geometry and high-level encoder
shape fixed while dropping `60/2` stream/IB handle changes to `0`; it does not
yet prove a win because it adds explicit copy traffic and leaves offset churn.


### From What is settled vs open

- Residual row `50/2` / refreshed `60/2` locality: useful under explicit
  exact/`lsb1` semantic policy for screen-blend, with current rank-1 semantic
  input and target-row Xcode movement prepared but aggregate top-GPU proof
  failed; row follow-up shows the failure is not non-target reordered-cache
  mutation. Class proxy now shows depth-read/screen/alpha `60/2` classes all
  have real `~25-28%` LRU32 ceilings;
  selected depth-read/no-blend two-draw windows have real `-27.6%`, `-31.0%`,
  `-21.5%`, and `-17.1%` LRU32 miss reductions. Rank 1 rejects exact/`lsb1`
  promotion after real textures, while ranks 2-4 are color-exact but
  owner-masked. A primitive-conflict selector scout rejects simple non-color
  thresholds, and the existing D3D9 occlusion query path is primitive-count only.
  dxmt9 now has a diagnostic Metal visibility scout for per-draw sample counts;
  zero counts can triage no-sample work, but positive counts still do not prove
  final color. The `60/2` cache join shows current zero rows are too small to
  own the hot LRU/hidden-backend traffic, and the current semantic ceiling
  projection shows rank2-4 exact-color windows are too small to justify another
  Xcode pass by themselves (`-9,113` LRU32, estimated `-0.071ms`). The path
  remains blocked by final-color/final-writer proof, a stricter runtime-visible
  selector that separates visible failures from masked windows, or a
  non-reorder backend mechanism.
  [index-cache-locality](../index-cache-locality/index.md), [hidden-backend-storage-shape.04](../hidden-backend-storage/hidden-backend-storage-shape.04.md),
  [mini-replay-bisection-semantic.02](mini-replay-bisection-semantic.02.md), [mini-replay-bisection-texture.02](mini-replay-bisection-texture.02.md),
  [mini-replay-bisection-texture.04](mini-replay-bisection-texture.04.md), [mini-replay-bisection-texture.05](mini-replay-bisection-texture.05.md),
  [mini-replay-bisection-texture.08](mini-replay-bisection-texture.08.md), [mini-replay-bisection-texture.09](mini-replay-bisection-texture.09.md),
  [mini-replay-bisection-texture.10](mini-replay-bisection-texture.10.md)
