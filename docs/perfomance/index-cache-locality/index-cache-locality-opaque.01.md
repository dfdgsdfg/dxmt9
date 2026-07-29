---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: opaque
order: 01
title: Layout-Stride Indexed Candidate Preflight
date: 2026-06-04
type: scout
status: model
outdated: retired-journal
source: specs/perfomance.plan.md#L1000-L1106
---

# Layout-Stride Indexed Candidate Preflight

> **Outdated — this leaf's only `source:` is the retired `specs/perfomance.plan.md` journal, which was deleted.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** On the stable layout-stride frame50 baseline, how much
LRU32 post-transform-cache locality is *available* on the hot indexed rows before
any mutating apply path is used? This sets the ceiling for the opaque-depth opt-in.

**Method.** `run_3dmark05_perf_probe.sh --suffix layoutstride-index-candidate-frame50-nogputrace-r1
--frame 50 --encoder-breakdown-seq 50 --no-gputrace --measure-index-cache-opt-candidate
--timeout 240 --top 5 --hot-gpu-share 95`, then `analyze_indexed_probe_classes.py
--group row-state-class --xcode-proxy-weight effective-miss32`. No-mutate
(`applied=0`, `reorder bytes=0`), so it is a planning signal, not a GPU result.

**Result.** Stable hot rows `50/0=42`, `50/1=156`, `50/2=187` draws;
`2,146,185` submitted vertices / `715,395` triangles across the top three.
Candidate LRU32 ceiling per row: `50/0` `168,951→125,159` (`-25.92%`);
`50/1` `413,707→325,648` (`-21.29%`); `50/2` `675,973→500,805` (`-25.91%`).
Top-3 total `1,258,631→951,612` (`-307,019` / `-24.39%`). Xcode proxy ranking
keeps `50/1` opaque-depth-write and `50/2` depth-read families as the largest
hidden-backend buckets (`~290MiB` / `~128MiB`).

**Verdict.** Model / ceiling. The production-safe subset is narrower than the
ceiling: `50/0` and `50/1` opaque depth-write classes are eligible for the
accepted opt-in; `50/2` is split across screen-blend tolerance, depth-read
structural hazards, and standard-alpha order dependence (semantic risk).
Confirms post-transform misses are the strongest reducible GPU-write axis.

**Related.** [index-cache-locality](index.md) · next: [index-cache-locality-opaque.02](index-cache-locality-opaque.02.md)
· [index-reuse-measurement](../index-reuse-measurement/index.md) (LRU32/cache-miss model) ·
[hidden-backend-storage](../hidden-backend-storage/index.md) (the bucket this reduces) · [snapshot-cache](../snapshot-cache/index.md) (layout-stride baseline).
