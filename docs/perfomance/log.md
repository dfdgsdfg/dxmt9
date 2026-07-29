---
domain: root
workload: dxmt9 performance
title: "DXMT9 Performance Documentation Log"
type: root-log
status: historical
updated: 2026-07-29
source: docs/perfomance/index.md; docs/perfomance/overview.md; 2; 3}-r1-20260725; experiments/output/app-d3d9-sfiv-benchmark-final-release-r1-20260725; experiments/output/app-d3d9-3dmark05-gt2-phase-latency2-r1-20260719; experiments/output/app-d3d9-3dmark05-gt2-immediate-default-latency-r1-20260719; traces/app-d3d9-3dmark05-gt2-phase-latency1-systemtrace-20260719; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.41.md
related: docs/perfomance/overview.md; docs/perfomance/overview-3dmark05-gt1.md
---

# DXMT9 Performance Documentation Log

Shared root-level log for [overview](overview.md) and
[overview-3dmark05-gt1](overview-3dmark05-gt1.md). Keep long-lived structure
changes and root-document maintenance notes here; experiment details belong in
domain leaf documents or domain `log.md` files.

## 2026-07-08

- Added root [index](index.md) as the entry point for the performance
  documentation tree.
- Kept [overview](overview.md) as the general dxmt9 performance bottleneck
  model.
- Kept [overview-3dmark05-gt1](overview-3dmark05-gt1.md) as the 3DMark05 GT1
  investigation map.
- Added shared root [log](log.md) rather than separate logs for the two root
  overview documents.
- Domain documentation uses `<domain>/index.md`, `<domain>/overview.md`, and
  `<domain>/log.md`, with one experiment per leaf document under each domain.
- Slimmed [overview-3dmark05-gt1](overview-3dmark05-gt1.md) to a cross-domain
  map and migrated its detailed synthesis blocks into the owning domain logs.

## Root 3DMark05 Map Detail Migration - 2026-07-08

Cross-domain structure and root-only reading-guide detail moved out of the live 3DMark05 overview during the root slimming pass. Domain-owned experiment detail was migrated into the corresponding domain logs.

### From The root question

**Why is 3DMark05 GT1 GPU-bound under dxmt9 (a D3D9→Metal translation
layer on Apple Silicon), and what owns the cost?**

Target: `app-d3d9-3dmark05`, GT1 path under `DXMT_EXPERIMENT_PROFILE=perf`.


### From Central finding (read this first)

A follow-up same-run after-draw color-history probe confirms the local writer:


### From Domain map

```mermaid
flowchart TD
  Root["GT1 perf run\n~1260 presents / 913714 draws\nframe120 33.611ms GPU, top-3 = 98.4%"]

  Root --> Base"[baselines<br/>frame120 / frame50 / frame60 reference captures"]

  %% GPU side
  Base --> GPU{{"GPU limiter:\ntop-3 encoders, memory/write bound\n(not ALU / texture-read)"}}
  GPU --> HBS"[hidden-backend-storage<br/>TVB / parameter scaling model (ACCEPTED)"]
  HBS --> HBD["hidden denominator<br/>stage-out vs binning/PB vs spill (OPEN)"]
  HBS --> TVB"[tvb-mechanism-proof<br/>VS-inv reduction -> TVB write reduction (ACCEPTED)"]

  %% rejected GPU ownership hunts
  HBS -.rejected owner.-> VSO"[vsout-layout<br/>visible varying width"]
  HBS -.rejected owner.-> SCG"[shader-codegen<br/>temp/scratch/offline IR"]
  HBS -.rejected/secondary.-> BSC"[backend-shape-classifiers<br/>alpha/depth/cull/scissor/fog/texture/expand"]
  HBS -.secondary.-> APF"[attachment-pixelformat<br/>R32F / X8 PixelFormatView"]

  %% measurement → reorder → the win
  HBS --> IRM"[index-reuse-measurement<br/>VS-inv tracks post-transform cache miss"]
  IRM --> PRD"[primitive-reorder-diagnostics<br/>reorder owns order? (frame-shape artifacts)"]
  IRM --> MRB"[mini-replay-bisection<br/>row-local reproduction + bisection"]
  MRB --> TVB
  PRD --> ICL"[index-cache-locality<br/>opaque-depth cache (THE WIN)"]
  TVB --> ICL
  IRM --> ICL

  %% Open backend mechanisms not yet proved by current probes
  HBD --> PBIN["Apple position/binning pass<br/>not tested by visible position-only VSOut"]
  HBD --> MESH["Metal 3 mesh/object path<br/>untried GT1 backend escape hatch"]
  HBD --> PSPILL["PSO/state churn backend spill<br/>current per-draw gate not isolated"]

  %% P1 GPU memory
  GPU --> RPS"[render-pass-store<br/>same RT/depth re-entry, store DontCare (P1)"]
  GPU --> TFFP["DXMT9_TILE_FFP<br/>implemented but narrow/default-off FFP tile path"]

  %% CPU side
  Base --> CPU{{"CPU / pacing cost\ncurrent low-overhead: completion_wait 44.8s,\ncommit_chunk replay 19.5s,\nencode_draw 16.5s,\nsnapshot 6.8s\n(hard under-pipelined P4)"}}
  CPU --> SNAP"[snapshot-cache<br/>D3D9 draw-state rebuild\n(historical owner, current P2/P3 residual;\ndirect-cbuf leaves lookup 2.859ms/present;\npure stream/IB and redundant const rejected)"]
  CPU --> SCE"[state-churn-encode<br/>stream/IB churn and commit_chunk replay"]
  CPU --> CU"[const-upload<br/>cbuf/argbuf traffic (CPU amplifier)"]
  CPU --> PP"[present-pacing<br/>completion_wait dominated by present completion<br/>current direct path already immediate<br/>no next-CB enqueue during wait<br/>BeginScene immediate<br/>SetRT/Clear share higher app frame 0x88760<br/>command dispatcher 0x4886E0 gates Clear dispatch<br/>dxmt9 completion-signal delay does not move Clear/first chunk<br/>deferred prototype opens P4 but fails CB/pass locality"]
  SCE --> SNAP
  PP --> SCE

  classDef win fill:#d6f5d6,stroke:#2b7a2b,color:#063
  classDef open fill:#fff3cd,stroke:#a80,color:#640
  classDef rej fill:#f8d7da,stroke:#a33,color:#600
  classDef base fill:#e8eefc,stroke:#3559a8,color:#0b2239
  class TVB,ICL win
  class HBS,HBD,IRM,MRB,RPS,PBIN,MESH,PSPILL,TFFP open
  class VSO,SCG,BSC,APF rej
  class Base,SNAP,SCE,CU base
```


### From Priority DAG

The remaining work is organized into priority levels. P0/P1 are active
GPU-frame targets; P2/P3 are CPU encode/submit cadence tracks; P4 is the
wallclock/present-completion wait bucket that must move when P2/P3 work becomes
small enough. Keep this average-FPS lane separate from the hot-frame
hidden-backend GPU limiter.

```mermaid
flowchart LR
  Start["Current evidence\nframe Counters + perf log"] --> P0["P0: GPU memory / write pressure"]
  Start --> P1["P1: pass split / store traffic"]
  Start --> P2["P2: recover draw-run / reduce per-draw encode"]
  Start --> P3["P3: reduce transient / const payload"]
  Start --> P4["P4: present pacing / wallclock sync"]

  P0 --> P0r"→ [hidden-backend-storage → tvb-mechanism-proof\n→ index-cache-locality (accepted numerator lever)\n→ hidden denominator mechanisms still open"]
  P1 --> P1r"→ [render-pass-store (re-entry real; A/B/A immediate target reuse; coalescing open)"]
  P2 --> P2r"→ [state-churn-encode + snapshot-cache (CPU wins, replay split, GPU flat)"]
  P3 --> P3r"→ [const-upload (CPU bytes ↓ 4.6GB→1GB, GPU flat)"]
  P4 --> P4r["completion_wait is present-completion paced\ncurrent direct path already immediate\nwatcher backlog rejected\nno next-CB enqueue during wait\nPE early calls immediate\n3DMark05 command dispatcher gates Clear dispatch\nSetRT return → Clear p50 17.4ms\ndxmt9 completed-seq/waterline dependency rejected\ndeferred prototype recovers overlap but not locality"]

  classDef p0 fill:#ffe8e8,stroke:#b64242,color:#2b0d0d
  classDef p1 fill:#fff0d6,stroke:#b26b00,color:#2b1900
  classDef act fill:#e8ffe8,stroke:#3c8f3c,color:#0d2b0d
  class P0,P1 p0
  class P2,P3,P4 p1
  class P0r,P1r,P2r,P3r,P4r act
```


### From What is settled vs open

**Accepted**

**Rejected as first-order GPU owner**

**Open**


### From Domain index

Related CPU-side counter design doc: [overview](overview.md).


### From How to read this graph

- **Domain landing** = `<domain>/index.md` (e.g.
  [index-cache-locality](index-cache-locality/index.md)). It points to the
  current overview, historical log, and recent leaf nodes.

- **Current overview** = `<domain>/overview.md`. Keep only the current compact
  conclusion, latest verdict rows, and active next gate here.

- **Historical log** = `<domain>/log.md`. Move older synthesis and long-running
  chronology here when it outgrows the current overview.
Layout: the top level of `docs/perfomance/` holds only the global roots; every
domain owns its landing, compact overview, log, and experiment leaves in its
subdirectory.

```
docs/perfomance/
  index.md                             # root entry point
  overview.md                          # CPU-side counter design root
  overview-3dmark05-gt1.md             # this file
  log.md                               # shared root maintenance log
  <domain>/index.md                    # domain landing
  <domain>/overview.md                 # current compact conclusion
  <domain>/log.md                      # older rolled-up detail
  <domain>/<domain>-<subcat>.<NN>.md   # leaf nodes (one experiment each)
```

- **Leaf node** = `<domain>/<domain>-<subcategory>.<NN>.md`, one experiment per
  file, numbered by execution order within its subcategory. Frontmatter carries
  `workload: 3DMark05 GT1`, `status`
  (accepted/rejected/inconclusive/model/tooling), and `source:` provenance. New
  entries should point at the actual `experiments/output/...` result, `traces/...`
  analysis, exported Xcode counters, or other concrete artifact rather than the
  deleted/retired spec journal. Every experiment is a 3DMark05 GT1 run.

- Links use standard Markdown with explicit relative `.md` targets, e.g.
  `[index-cache-locality](index-cache-locality/index.md)`.

## 2026-07-29 - Root Overview Re-Centring

[overview](overview.md) is the general dxmt9 performance model. Four blocks in
its "Current Multi-Workload Baseline" section were chronology, per-experiment
verdict detail, or workload-owned synthesis, which
`agents/rules/documentation.rules.md` assigns to this log or to the owning
domain. They were moved out here, verbatim, so nothing is lost. The overview now
carries the bottleneck model, the current multi-workload baseline, the bound
legend, the current default policy, and the open areas.

A fifth block, the direct-cbuf generality gate, went to its owning domain log
instead: [state-churn-encode/log.md](state-churn-encode/log.md).

### Moved: 2026-07-25 Release-Default Spot Check

Single-run release/O3 sanity set. It was never a baseline; the repeated medians
in the overview's table remain the reference. Its GT1 row is also carried in
[overview-3dmark05-gt1](overview-3dmark05-gt1.md). Every cited artifact
(`experiments/output/app-d3d9-3dmark05-release-default-gt{1,2,3}-r1-20260725`,
`experiments/output/app-d3d9-sfiv-benchmark-final-release-r1-20260725`) is still
on disk, so these numbers remain re-checkable.

> Commit `5dc7ca0160241da1b45e8d96950fa0ed7be9647f` was rebuilt from all three
> release/O3 staging directories and measured with the current engine defaults.
> Only perf counters and frame sampling were enabled; Metal capture, encoder
> breakdown, renderer experiment features, cross-chunk DCE, and the standard
> probe's `DXMT_DISABLE_AUTO_EXPAND_INDEXED` override were disabled.
> The GT runs used frontmost supervision, and SFIV retained the duration-matched
> 25-second capture delay plus 110-second timeout.
> The repeated 3DMark references used the standard probe's auto-expand override,
> so their deltas below are context, not same-policy A/B regressions.
>
> | Workload | Closest reference FPS | Release FPS | Delta | Wall p50 / p95 | GPU CB p50 / p95 |
> |---|---:|---:|---:|---:|---:|
> | GT1 | `21.009` | `20.540` | `-2.23%` | `44.426 / 67.520ms` | `1.163 / 1.240ms` |
> | GT2 | `8.150` | `8.319` | `+2.07%` | `103.046 / 155.840ms` | `2.971 / 3.405ms` |
> | GT3 | `27.858` | `27.981` | `+0.44%` | `29.355 / 82.801ms` | `7.729 / 11.071ms` |
> | SFIV | `44.668` | `45.416` | `+1.67%` | `16.702 / 47.793ms` | `3.810 / 8.633ms` |
>
> All four runs passed with zero chunk/V2 rejects, GPU command-buffer errors,
> pipeline-build failures, missing-pipeline draws, and DCE activity. GT1 shows
> the muzzle/bloom path, GT2 the forest scene, GT3 a coherent full frame without
> the quadrant rectangle at its ordinary capture point, and SFIV the expected
> Ryu lighting/post-effect frame with a `46.06` benchmark overlay average. This
> is a single-run release sanity set, not a replacement for the repeated
> baselines above. GT2's original `7.868` promotion baseline is `5.73%` below
> this release sample; `8.150` is the closer post-policy reference.

### Moved: GT2 MANAGED Versioning, Incremental Hash, and Immediate Boundary

Workload-owned synthesis. The live version is
[overview-3dmark05-gt2](overview-3dmark05-gt2.md), which carries the same
`235.871ms`, `307.194 -> 155.734ms`, `12.164 -> 10.248ms`, and `5.92%` figures
in their own tables. Note that
`experiments/output/app-d3d9-3dmark05-gt2-phase-latency1-r1-20260719` and the
GT1 phase-latency pair are gone from disk; the phase-aligned numbers below are
last measurements, and the surviving provenance is
`traces/app-d3d9-3dmark05-gt2-phase-latency1-systemtrace-20260719` plus
`experiments/output/app-d3d9-3dmark05-gt2-phase-latency2-r1-20260719`.

> GT2 is the one row newer than the original `153cacb14f2f` baseline: MANAGED
> buffer backing versioning removes `42.4ms/present` of writable-map sequence
> wait, and the default range-incremental shader-constant content index reduces
> snapshot CPU `15.8%` (`12.164 -> 10.248ms/present`) while preserving payload
> deduplication. Together they improve the earlier `current-v2` median by
> `5.92%`. Phase-aligned tracing places the former drawable stall in queued-GPU
> run-ahead: request-to-first-GPU-work is p50 `235.871ms`, while GPU-end to
> completion is only `0.205ms`. An Immediate one-frame completion boundary
> eliminates drawable waits (`146 -> 0`) and cuts CPU-present-to-display
> `307.194 -> 155.734ms` without moving GT2 or GT1 throughput beyond `1%`.
> The engine now applies that stricter boundary when an Immediate present still
> uses the default maximum of four; synchronized and explicit non-default values
> retain their windows. Publication-to-dequeue remains only about
> `0.003ms/present`, so more bulk chunk streaming is not the next lever; the GT2
> throughput frontier remains current-frame GPU vertex/pass work and residual
> CPU snapshot/encode cost.

### Moved: GT2 R32F Whole-Run Liveness Closure

Workload-owned synthesis. The live version is
[hidden-backend-storage-shape.41](hidden-backend-storage/hidden-backend-storage-shape.41.md),
which is unmarked and keeps its artifacts.

> GT2 is not yet at a demonstrated GPU-work ceiling. A 2026-07-25 whole-run
> liveness closure finds the final dominant `2048x2048 R32F` pass dead in all
> `503` measured target frames; three consecutive alias-aware DAGs also prove
> its shared depth dead. The opportunity is structural but not reachable from
> the current one-chunk optimizer window: all `531` encode dequeues observed
> ready depth exactly one. The next GPU-work experiment is therefore a
> fail-open, TLA-backed cross-chunk proof window, not another publication or
> per-draw locality tweak. See
> [hidden-backend-storage-shape.41](hidden-backend-storage/hidden-backend-storage-shape.41.md).

### Moved: V1 and Historical Comparison Boundary

Historical comparison scope. **The GT3 row is no longer re-checkable:** it cites
`experiments/output/app-d3d9-3dmark05-command-chunk-v2-final2-pair*` and
`experiments/output/app-d3d9-3dmark05-gt3-quadrant-glitch-{v1,v2}-exact`, both
gone from disk. Its `-1.1%`, `-30.8%`, and `+5.46%` figures are last
measurements. The GT1/GT2/SFIV rows state the absence of a defensible delta
rather than a number, and remain valid as scope statements.

> V1 is no longer runnable at current HEAD, so comparisons use preserved
> artifacts and must retain their original scope.
>
> | Workload | Available comparison | Readout |
> |---|---|---|
> | GT1 | No completed frame-sampled same-build V1/V2 pair | No defensible wire-only delta. The older `1,800 -> 2,220-2,293` presents/120s comparison includes other engine-default changes. |
> | GT2 | Only incomplete/timed-out V1-era diagnostic runs | No defensible V1/V2 performance delta. The current V2-only baseline is the first completed frame-sampled reference. |
> | GT3 | Five same-build V1/V2 promotion pairs plus one exact-window pair | The five-pair median showed V2 process throughput `-1.1%` and offload replay CPU/present `-30.8%`, passing the no-worse-than-`-3%` gate. The exact pair measured `+5.46%` throughput. Historical captures contain the then-open quadrant artifact, so they are performance evidence only; current captures are whole-run sanity evidence and the targeted 66-68-second heuristic remains the visual gate. |
> | SFIV | Duration-matched V1-era and current V2-only runs, but not a single-change A/B | Presents improved `1,500 -> 5,610` (`+274%`), GPU CB p50 `110.117 -> 2.725ms` (`-97.5%`), and p95 `126.235 -> 8.005ms` (`-93.7%`). This is a cumulative renderer improvement; a 2026-07-14 pre-V2 run had already reached `40.34` presents/s and `3.167/8.932ms`, so the gain must not be attributed to command-chunk V2 alone. |
