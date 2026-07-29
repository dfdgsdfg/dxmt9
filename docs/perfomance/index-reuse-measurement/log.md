---
domain: index-reuse-measurement
workload: 3DMark05 GT1
title: "Index-Reuse Measurement — instrumentation that established VS-inv = post-transform cache-miss - Historical Log"
type: domain-log
status: historical
updated: 2026-07-08
source: docs/perfomance/index-reuse-measurement/index.md
related: docs/perfomance/index-reuse-measurement/index.md; docs/perfomance/index-reuse-measurement/overview.md
---

# Index-Reuse Measurement — instrumentation that established VS-inv = post-transform cache-miss - Historical Log

> Full historical detail moved from the former top-level `index-reuse-measurement.md` overview.
> Keep [overview](overview.md) current and compact; append long-running chronology,
> rejected paths, and detailed synthesis here only when it is not already captured in
> one-experiment leaf documents.

---

# Index-Reuse Measurement — instrumentation that established VS-inv = post-transform cache-miss

> Part of the 3DMark05 GT1 GPU-bottleneck investigation. Root map: [overview-3dmark05-gt1](../overview-3dmark05-gt1.md).

## Scope & question

This domain owns the **measurement / instrumentation** that characterized the
*shape* of the hot indexed geometry without changing rendering: how Xcode
`VS Invocations` relates to submitted references vs draw-local unique vertices vs
finite post-transform vertex-cache misses; whether the ~1.6 GiB VS-buffer-write
bucket is caused by tiny-draw replay or redundant geometry replay; and how the
hot indexed triangle-list traffic splits by backend-relevant state class. Its
job was mostly to **rule out** cheap explanations (geometry expansion, dedup,
tiny draws, payload canonicalization) and to **pinpoint** the one correlation
that made a lever exist: VS invocations track the post-transform cache-miss
estimate, not raw references.

## Hypotheses & verdicts

| # | Hypothesis | Verdict | Evidence |
|---|-----------|---------|----------|
| H1 | VS invocations follow raw indexed references (`0.549x`) | rejected | index-reuse-measurement-reuse.01 |
| H2 | VS invocations follow draw-local unique vertices exactly | rejected (`1.18x` gap = finite cache) | index-reuse-measurement-reuse.01 |
| H3 | VS invocations ≈ finite 64-entry post-transform cache misses (`0.976x`) | **accepted** | index-reuse-measurement-reuse.01 |
| H4 | Order-preserving payload canonicalization can cut VS invocations | rejected (0 duplicate payloads, LRU32 delta 0) | index-reuse-measurement-reuse.02 |
| H5 | dxmt indexed-expansion is inflating GT1 geometry | rejected (`draw_expanded_indexed=0`) | index-reuse-measurement-geometry.01 |
| H6 | Redundant replay of the same geometry shape owns the bucket | rejected (dup ratio `0.143x`) | index-reuse-measurement-geometry.02 |
| H7 | Bucket is driven by many tiny repeated draws | rejected; real large indexed pressure (`22,622` prim/draw) | index-reuse-measurement-geometry.03 |
| H8 | Hot frame is one homogeneous material class | rejected; splits opaque-dw / depth-read-textured / mixed | index-reuse-measurement-classattr.01 |
| H9 | The positive `60/4` large-draw signal is production-safe | rejected; `60/4` large4096 is 0 opaque / all depth-read | index-reuse-measurement-classattr.02 |

## Verification methods

- **`DXMT9_MEASURE_INDEX_REUSE=1`** (wrapper `--measure-index-reuse`) — diagnostic
  scan of indexed draw buffers: raw references, draw-local unique estimate, and
  LRU 16/32/64-entry cache-miss estimates. Proves VS invocations track cache64
  (`0.976x`), not references (`0.549x`).
- **`DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE`** — builds a cache-aware LRU32
  reordered candidate (no submission) and reports original-vs-candidate cache-miss
  estimates; the lever validation that follows from H3.
- **`DXMT9_PERF_ENCODER_BREAKDOWN=1` geometry signature counters** —
  `draw_geometry_signature_{samples,unique,duplicates,consecutive_duplicates}`;
  proves the bucket is not redundant-shape replay (H6).
- **Encoder breakdown draw-size buckets** — `draw_primitive_*` / `draw_vertex_*`
  min/max + bucketed counts; proves large-draw (not tiny-draw) pressure (H7).
- **Encoder breakdown state-class counters** — non-exclusive opaque-depth-write /
  depth-read / alpha-blend / scissor / textured / large4096 (and `large4096 ×
  state` cross buckets); splits the hot frame into targetable material classes
  (H8, H9). All behavior-neutral instrumentation, validated by finalizer coverage
  + 5% drift gates.

## Experiment dependency graph

```mermaid
flowchart TD
  reuse01["index-reuse probe\nVS-inv = cache64 (0.976x)"]
  reuse02["payload canonicalization\n0 dup payloads, LRU32 delta 0"]
  geo01["geometry-expansion audit +\nsignature instrumentation"]
  geo02["signature dedup result\ndup ratio 0.143x"]
  geo03["draw-size histogram\nmax 22,622 prim/draw"]
  class01["state-class attribution\nopaque 874.78 / depth-read 370.35 MiB"]
  class02["large4096 cross-bucket\n60/4=0 opaque, 18+5 opaque safe"]
  feeds"feeds [primitive-reorder-diagnostics\nand index-cache-locality"]

  reuse01 -->|"rejected non-reorder locality"| reuse02
  reuse01 -->|"motivates"| feeds

  geo01 -->|"signature -> dedup"| geo02
  geo02 -->|"dedup -> size"| geo03
  geo03 -->|"narrowed to"| class01
  class01 -->|"split safe vs diagnostic"| class02
  class02 -->|"safe opaque-large set"| feeds
  reuse02 -->|"only lever left"| feeds

  classDef accepted fill:#d6f5d6,stroke:#2b7a2b,color:#063
  classDef rejected fill:#f8d7da,stroke:#a33,color:#600
  classDef open fill:#fff3cd,stroke:#a80,color:#640
  class reuse01,geo03,class01,class02 accepted
  class reuse02,geo02 rejected
  class geo01,feeds open
```

## Results synthesis

**Settled.** The central result is that **Xcode `VS Invocations` track the
post-transform finite vertex-cache-miss estimate** (`VS-inv/cache64 ≈ 0.976x`),
not raw indexed references (`0.549x`) and not raw draw-local unique vertices
(`~1.18x` gap, which is itself finite-cache locality). This is *why* index-cache
locality became the promising lever in [index-cache-locality](../index-cache-locality/index.md): reducing
post-transform cache misses is the only measured way to reduce VS invocations
without changing geometry semantics. The domain also conclusively *ruled out*
the cheap explanations — dxmt geometry expansion (`draw_expanded_indexed=0`),
redundant geometry-shape replay (dup ratio `0.143x`), tiny-draw replay (hot rows
reach `22,622` prim / `67,866` vert per draw), and order-preserving payload
canonicalization (0 mergeable payloads, LRU32 delta 0). State-class attribution
then split the hot frame into an opaque depth-write class (`60/3 + 60/1`,
`874.78MiB`), a depth-read/textured/alpha class (`60/4`, `370.35MiB`), and a
mixed row (`60/0`, `227.67MiB`), and the large4096 cross-bucket isolated the
`23` production-safe opaque-large draws from the visibility-sensitive `60/4`
target.

**Still open within this domain.** Nothing about the **write width** is resolved
here: even after normalizing by unique vertices or cache misses, the bucket stays
`~836–879B`/invocation versus the visible `184B` `VSOut`. That residual is owned
by [hidden-backend-storage](../hidden-backend-storage/index.md), not by this measurement domain. Whether the safe
opaque-large set actually moves the hidden write under a correctness-preserving
reorder is handed to [index-cache-locality](../index-cache-locality/index.md) / [primitive-reorder-diagnostics](../primitive-reorder-diagnostics/index.md)
for Xcode proof.

## How to run
Every experiment here is a 3DMark05 GT1 run via the standard wrapper. These are
behavior-neutral diagnostic scans, so a cheap `--no-gputrace` scout with the
measurement flags is the canonical run:

```sh
DXMT9_PERF_ENCODER_BREAKDOWN=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh --suffix idx-reuse --frame 60 \
  --measure-index-reuse --no-gputrace --timeout 180
# candidate (no-submit) reordered-ceiling scan: add --measure-index-cache-opt-candidate

# To cross-check VS-invocation tracking against Xcode, capture + finalize:
bash scripts/tools/finalize_3dmark05_perf_probe.sh --suffix idx-reuse --frame 60 \
  --require-xcode-counter-coverage --require-dxmt-join-coverage --require-top-pso-attribution
```

The exact per-experiment flags live in each leaf's `**Method.**` field. See
`agents/rules/environment_variables.rules.md` for env-var meanings and
`agents/rules/metal_debugging.rules.md` for the full workflow.

## Cross-references

- [primitive-reorder-diagnostics](../primitive-reorder-diagnostics/index.md) — consumes the state classes and large4096
  splits; reverse-triangle probes that produced the first positive signal.
- [index-cache-locality](../index-cache-locality/index.md) — the accepted production lever motivated by the
  VS-inv ≈ cache-miss correlation and the safe opaque-large candidate set.
- [hidden-backend-storage](../hidden-backend-storage/index.md) — owns the unexplained per-invocation write width
  that this domain measured but did not explain.
- [vsout-layout](../vsout-layout/index.md) — refuted as the width owner here: the bucket is far wider
  than the visible `184B` `VSOut`.
- [overview-3dmark05-gt1](../overview-3dmark05-gt1.md) — root priority DAG and synthesis.
