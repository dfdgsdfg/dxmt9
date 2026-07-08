---
domain: root
workload: dxmt9 performance
title: "DXMT9 Performance Documentation Log"
type: root-log
status: historical
updated: 2026-07-08
source: docs/perfomance/index.md
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
