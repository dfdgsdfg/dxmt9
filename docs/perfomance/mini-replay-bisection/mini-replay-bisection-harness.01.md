---
domain: mini-replay-bisection
workload: 3DMark05 GT1
subcategory: harness
order: 01
title: Mini-Replay Readiness Planner
date: undated
type: tooling
status: tooling
outdated: retired-journal
source: specs/perfomance.plan.md#L13687-L14202
---

# Mini-Replay Readiness Planner

> **Outdated — this leaf's only `source:` is the retired `specs/perfomance.plan.md` journal, which was deleted.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** Before building a standalone row-local replay, what
inputs already exist for the GT1 hot encoders, and what is missing? The goal was
to certify that hot-row attribution, shader sources, and draw identity were in
hand — and to discover the one gap that blocked replay construction.

**Method.** `python3 scripts/tools/plan_3dmark05_mini_replay.py` joining
`frame60-xcode-dxmt-joined-summary.csv`,
`frame60-shader-dump-summary.csv`, and the indexed `3dmark05-perf-indexed-probe-draws.csv`
probe CSV, with `--top 5 --top-groups 3`. Output:
`frame60-mini-replay-readiness.md`.

**Result.** Readiness table:

| Item | Status | Evidence |
|---|---|---|
| Hot row attribution | ready | 5 Xcode/dxmt rows selected |
| Shader sources | partial | 5/5 hot rows have shader dump rows |
| Draw identity/index locality | partial | 5/5 hot rows have indexed probe rows |
| Raw vertex/index payload | **missing** | no artifact contained replayable geometry bytes |

Top replay target groups: `60/2` (71 draws, 87,499 tris, alpha/depth-read/textured),
`60/1` (189 draws, opaque depth-write), `60/0` (71 draws, opaque depth-write textured).

**Verdict.** TOOLING. The planner confirmed row/state/shader/index identity were
ready and isolated the **geometry-bytes gap**: no reduced artifact carried raw
index + referenced stream bytes. This motivated the geometry payload dumper
([mini-replay-bisection-payload.01](mini-replay-bisection-payload.01.md)). The planner later gained `--geometry-dir`
to validate payload triplets.

**Related.** [mini-replay-bisection](index.md) · [mini-replay-bisection-payload.01](mini-replay-bisection-payload.01.md) ·
[index-reuse-measurement](../index-reuse-measurement/index.md) (probe CSV source) · [hidden-backend-storage](../hidden-backend-storage/index.md) ·
[overview-3dmark05-gt1](../overview-3dmark05-gt1.md)
