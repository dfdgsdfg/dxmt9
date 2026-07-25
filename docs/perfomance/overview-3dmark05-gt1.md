---
domain: root
workload: 3DMark05 GT1
title: "3DMark05 GT1 Performance — Investigation Map"
type: root-overview
status: current
updated: 2026-07-25
source: experiments/output/app-d3d9-3dmark05-current-v2-gt1-r{1,2,3}-20260719; experiments/output/app-d3d9-3dmark05-release-default-gt1-r1-20260725; docs/perfomance/state-churn-encode/state-churn-encode-encode-phase.202.md; docs/perfomance/index-cache-locality/index-cache-locality-scope-merge.21.md
related: docs/perfomance/log.md; docs/perfomance/overview.md; specs/backend/gap.md
---

# 3DMark05 GT1 Performance — Investigation Map

> Root node for the `docs/perfomance/` 3DMark05 GT1 knowledge graph.
> Keep this file as the cross-domain readout: whole-experiment axes,
> current gates, and where to find domain-owned detail.
>
> Root navigation: [index](index.md). Shared log: [log](log.md).
> Historical detail that used to live here was migrated into domain `log.md`
> files on 2026-07-08.

## Root Question

**Why is 3DMark05 GT1 slow under dxmt9, and which owner class should the next
experiment target?**

Target workload: `app-d3d9-3dmark05`, GT1 path under
`DXMT_EXPERIMENT_PROFILE=perf`.

## Current Measured Baseline

Three completed current-runtime runs on 2026-07-19 used the V2-only command
wire, `-gt1 -nosplash -nosysteminfo -noscreens`, no Metal frame capture,
frame sampling, frontmost supervision, and the engine-default offload plus
opaque-depth index-cache policy. All three captures are visually normal and
all GPU error counters are zero.

| Metric | Run median | Run range |
|---|---:|---:|
| sampled average FPS | `21.009` | `20.919-21.189` |
| sampled frames | `2,297` | `2,289-2,316` |
| sampled wall time | `109.332s` | `109.302-109.422s` |
| wall p50 | `43.188ms` | `42.930-43.611ms` |
| wall p95 | `64.966ms` | `64.669-65.303ms` |
| GPU CB p50 | `4.684ms` | `4.624-4.697ms` |
| GPU CB p95 | `21.749ms` | `20.498-22.634ms` |
| encoded presents | `2,298` | `2,290-2,317` |

This supersedes the older `~16-17` sampled-FPS calibration as the current
whole-run reference. The narrow `1.3%` max/min FPS spread is small enough to
use this baseline for future A/B gates without first increasing the repeat
count.

The 2026-07-25 release-default spot check rebuilt commit `5dc7ca01` as
release/O3 and ran with counters/frame sampling only. It records `2,242`
positive samples over `109.155s`, or `20.540` sampled FPS, with wall
p50/p95 `44.426/67.520ms` and GPU-CB p50/p95 `1.163/1.240ms`. FPS is
`2.23%` below the repeated baseline median and remains a release sanity result,
not a new baseline. The capture shows the robot, weapon, muzzle flash, bloom,
lighting, and floor reflection intact. Chunk rejects, V2 rejects, GPU errors,
pipeline failures, missing-pipeline draws, and DCE activity are all zero.

The 2026-07-20 [direct-cbuf generality gate](state-churn-encode/state-churn-encode-encode-phase.202.md)
adds two OFF and two ON current-build runs. Direct-cbuf changes sampled FPS
`20.908 -> 20.974` (`+0.32%`) while draw/chunk CPU falls `20.72%/17.05%`,
argbuf setup and table binds become zero, and visuals/errors remain clean. It
is a proven local CPU cleanup, not a GT1 FPS owner. The cross-workload result
and deterministic dirty-rebind regression subsequently support its default-on
promotion with explicit value `0` as the rollback lane.

The 2026-07-21 [index-cache scope/merge gate](index-cache-locality/index-cache-locality-scope-merge.21.md)
is also GT1 despite `gt2` output-directory and result-file suffixes: every run
explicitly passed `-gt1`. Extending the reorder safety gate adds no candidate
(`125` unique candidates, `143` misses, and `67` created buffers in every
lane), while strict adjacent compatible indexed-draw merging eliminates zero
draws. Candidate FPS `20.945-21.056` is inside the current reference range and
has no mechanism movement; both experiment flags remain default OFF.

### V1 Comparison Status

There is no completed, frame-sampled, same-build V1 GT1 reference. The earlier
`1,800 -> 2,220-2,293` presents/120s result is a cumulative engine-default
comparison with other policy changes, not an isolated V1/V2 wire A/B. The five
`command-chunk-v2-final2` promotion pairs are GT3 artifacts and belong in the
[GT3 baseline](overview-3dmark05-gt3.md). Treat `21.009` sampled FPS as the
first current V2-only GT1 baseline rather than assigning its full improvement
to the command wire.

The investigation currently separates three concerns that used to be easy to
mix together:

| Axis | Current readout | Detail owner |
|---|---|---|
| Hot-frame GPU cost | Top render encoders are dominated by hidden Apple vertex/tiler/parameter-buffer storage, not dxmt CPU upload bytes or visible `VSOut` width. | [hidden-backend-storage](hidden-backend-storage/overview.md), [tvb-mechanism-proof](tvb-mechanism-proof/overview.md) |
| Proven GPU lever | Reducing VS invocations through semantic-safe opaque-depth index-cache locality moves the hidden-write bucket and passed the full promotion proof (H195). Since `d45af067` (H216) the flag is engine-default ON, coupled to the commit-replay offload. | [index-cache-locality](index-cache-locality/overview.md) |
| Wallclock / FPS cost | The engine-default trio (commit-replay offload, coupled index-cache, ungated PE readonly managed-buffer lock cache) is promoted and cumulative: GT1 `1,800 -> 2,220-2,293` presents/120s (`~+26%`, H211/H216). The residual wall is the game's own Rosetta CPU + Wine thunking (H212): dxmt9 unix app-thread cost is `~1.8%` of the frame; PE recording is `~8.5ms/present` (H213). | [present-pacing](present-pacing/overview.md), [state-churn-encode](state-churn-encode/overview.md), [snapshot-cache](snapshot-cache/overview.md) |
| Correctness gate | Visual parity is a promotion gate, not a proof of a hardware wall. Weapon/muzzle/bloom reports require same-frame or draw-local final-writer proof before redirecting the performance plan. | [snapshot-cache](snapshot-cache/overview.md), [backend-shape-classifiers](backend-shape-classifiers/overview.md) |
| Pass/store and backend-shape alternatives | Render-pass store traffic, Tile-FFP, mesh/object, programmable tile routes, PSO/state shape, and visible-width probes remain either rejected-current or reduced-A/B prerequisites. | [render-pass-store](render-pass-store/overview.md), [hidden-backend-storage](hidden-backend-storage/overview.md), [vsout-layout](vsout-layout/overview.md), [shader-codegen](shader-codegen/overview.md) |

## Current Whole-Experiment View

The strongest global conclusion is that **one number cannot explain the run**.
A GT1 candidate must say which axis it moves:

- **GPU-frame axis:** move the hidden backend storage bucket with a proof that
  VS invocations, VS buffer writes, and GPU time move together.
- **Average-FPS axis:** move producer/replay/encode/present-pacing counters in
  a no-gputrace run without increasing command-buffer, render-pass, or tile
  preservation cost.
- **Correctness axis:** preserve the `v0.0.3` visual gate, then use final-writer
  or same-frame evidence for object-specific reports before promoting FPS or
  Xcode-counter deltas.

The accepted GPU mechanism is narrow but real: opaque-depth index-cache locality
reduces post-transform cache misses, VS invocations, hidden VS writes, and target
GPU time. The accepted wallclock win is the engine-default trio: the
commit-replay offload (`DXMT9_OFFLOAD_COMMIT_REPLAY`, engine-default ON since
`d45af067`, explicit `0` opts out), the coupled index-cache default, and the
ungated PE readonly managed-buffer lock cache — cumulatively
`1,800 -> 2,220-2,293` presents/120s (`~+26%`). Keep the GPU-mechanism and
wallclock conclusions separate. All rejected experiment lanes from the
2025-26 carrier chains were removed from the tree in the H217-H221 cleanup
arc (see [present-pacing](present-pacing/index.md)); every remaining opt-in
env is either a diagnostic A/B switch for a live default path or an open
frontier (tile-FFP two-stage encode, argbuf direct-cbuf scout, unpublished-slot
PSO prefetch, sparse const records, `DXMT9_PE_INLINE_CONST_DELTA` — mechanism
proven, FPS-null, kept opt-in per H214).

## Promotion Gates

| Gate | Required evidence | Where details live |
|---|---|---|
| Visual safety | Normal GT1 output under the current `v0.0.3` visual gate; object-specific reports need same-window or draw-local proof. | [snapshot-cache](snapshot-cache/overview.md), [backend-shape-classifiers](backend-shape-classifiers/overview.md) |
| Runtime no-gputrace | Clean run with no fatal/assert/GPU/queue errors, stable frame sampling, and movement in the owning no-gputrace counters. | [present-pacing](present-pacing/overview.md), [state-churn-encode](state-churn-encode/overview.md), [baselines](baselines/overview.md) |
| Xcode / `.gputrace` | Only spend capture budget after the candidate has a semantic or reduced-A/B reason to move hidden backend storage. | [hidden-backend-storage](hidden-backend-storage/overview.md), [index-cache-locality](index-cache-locality/overview.md) |
| Promotion/default decision | Runtime win, correctness proof, and non-regressing locality/pass/tile shape must all agree. | Domain overview plus the concrete leaf documents cited there. |

## Investigation Axes

```mermaid
flowchart TD
  Root["3DMark05 GT1"] --> Correctness["visual / correctness gate"]
  Root --> GPU["hot-frame GPU owner"]
  Root --> CPU["CPU producer + encode owner"]
  Root --> Sync["wallclock / present pacing"]

  Correctness --> Snapshot["snapshot-cache"]
  Correctness --> Shape["backend-shape-classifiers"]

  GPU --> Hidden["hidden-backend-storage"]
  Hidden --> TVB["tvb-mechanism-proof"]
  TVB --> Locality["index-cache-locality"]
  Hidden --> Backend["backend escape / reduced A/B"]

  CPU --> State["state-churn-encode"]
  CPU --> SnapCPU["snapshot-cache"]
  CPU --> Const["const-upload"]

  Sync --> Present["present-pacing"]
  Present --> State

  GPU --> Store["render-pass-store"]
```

## Domain Map

| Domain | Owns | Current root perspective |
|---|---|---|
| [baselines](baselines/index.md) | reference captures and frame anchors | Use for canonical frame/run shape, not as a mutable experiment journal. |
| [hidden-backend-storage](hidden-backend-storage/index.md) | hidden VS/TVB/parameter-buffer attribution | Central GPU explanation; detailed gate history and rejected backend-shape paths live in its overview/log. |
| [tvb-mechanism-proof](tvb-mechanism-proof/index.md) | proof that VS-invocation reduction moves TVB writes | Load-bearing mechanism proof behind index-cache locality. |
| [index-cache-locality](index-cache-locality/index.md) | opaque-depth and screen-blend locality | Only accepted production-shaped GPU win, engine-default ON since `d45af067` (coupled to the offload); screen-blend remains policy/oracle-bound. |
| [index-reuse-measurement](index-reuse-measurement/index.md) | cache-miss and geometry reuse measurement | Supports locality attribution and candidate ranking. |
| [primitive-reorder-diagnostics](primitive-reorder-diagnostics/index.md) | reverse/min-index/split reorder probes | Keeps frame-shape artifacts out of promotion claims. |
| [mini-replay-bisection](mini-replay-bisection/index.md) | row-local replay and final-writer bisection | Supplies correctness/oracle evidence before Xcode spend. |
| [vsout-layout](vsout-layout/index.md) | visible varying / `VSOut` layout probes | Rejected as first-order hidden-write owner; keep as evidence, not next budget. |
| [shader-codegen](shader-codegen/index.md) | MSL/AIR/temp/scratch probes | Rejected above-AIR explanations; owner is below source-visible shader shape. |
| [backend-shape-classifiers](backend-shape-classifiers/index.md) | alpha/depth/cull/scissor/fog/texture classifiers | Mostly rejected or secondary; still relevant to visual/perf coupling. |
| [attachment-pixelformat](attachment-pixelformat/index.md) | R32F/X8 attachment format probes | Secondary texture/write path, not the central VS owner. |
| [const-upload](const-upload/index.md) | cbuf/argbuf upload and constant traffic | CPU amplifier; accepted cleanups do not by themselves prove GPU/FPS promotion. |
| [state-churn-encode](state-churn-encode/index.md) | replay, draw-run, PSO/bind/state encode cost | Large CPU track; the engine-default offload moved the replay cost class onto a worker (idle `~39.4ms/present`), and the rejected carrier lanes were removed (H217-H220). Remaining local wins are FPS-flat; the frontier is encode-side P4 overlap and pass-streaming (R-BACK-2.39/2.40/2.43). |
| [snapshot-cache](snapshot-cache/index.md) | D3D9 snapshot rebuild and visual-gate history | CPU track plus current visual gate owner for reported correctness artifacts. |
| [render-pass-store](render-pass-store/index.md) | pass re-entry, load/store, tile preservation | P1 locality/pass-shape track; must not regress while chasing overlap. |
| [present-pacing](present-pacing/index.md) | completion wait, producer cadence, bridge/lock attribution | Average-FPS/pacing owner; owns the engine-default trio promotion (H211/H216), the H217-H221 dead-lane cleanup arc, and the H212/H213 residual attribution (game's own CPU; PE recording `~8.5ms/present`). |

## Reading Rules

- Start with this file for the global axis and gate discipline.
- Use each domain [index](index.md) page as the landing point; read its
  `overview.md` for current conclusion and `log.md` for older rolled-up detail.
- Leaf documents remain one experiment per file and carry the concrete artifact
  provenance in YAML `source:`.
- Do not add long experiment verdict tables here. Move detailed synthesis into
  the domain that owns the evidence, then link that domain overview from this
  root map.

## Layout

```text
docs/perfomance/
  index.md                             # root entry point
  overview.md                          # general dxmt9 performance model
  overview-3dmark05-gt1.md             # this cross-domain GT1 map
  overview-3dmark05-gt2.md             # GT2 measured baseline
  overview-3dmark05-gt3.md             # GT3 measured baseline + V1/V2 history
  overview-sfiv.md                     # SFIV D3D9Ex map and baseline
  log.md                               # shared root maintenance log
  <domain>/index.md                    # domain landing
  <domain>/overview.md                 # current compact conclusion
  <domain>/log.md                      # older rolled-up detail
  <domain>/<domain>-<subcat>.<NN>.md   # leaf nodes, one experiment each
```
