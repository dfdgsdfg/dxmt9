---
domain: index-reuse-measurement
workload: 3DMark05 GT1
subcategory: classattr
order: 01
title: Indexed Triangle State-Class Attribution
date: 2026-06-02
type: measurement
status: accepted
source: specs/perfomance.plan.md#L10125-L10240
---

# Indexed Triangle State-Class Attribution

**Question / hypothesis.** The hot frame is not one homogeneous material; split
the hot indexed triangle-list geometry by backend-relevant state class so the
next primitive/backend probes can target a stable material class instead of
perturbing the whole frame.

**Method.** Run `indexed-triangle-class-gputrace-r1` adds per-encoder, *non-mutually-exclusive*
indexed triangle-list state-class counters (opaque-depth-write, depth-read,
alpha-blend, scissor, textured, large4096), each as draws/primitives/vertices.
Instrumentation only — does not change draw submission. Matched Xcode export,
finalizer compared against `measure-index-cache-gputrace-r1` with full coverage
and 5% draw/vertex/triangle drift gates.

**Result.** Behavior-neutral: total GPU `34.391 → 34.617ms` (`+0.66%`), hot VS
buffer write `1472.747 → 1472.796MiB` (`+0.00%`), hot VS bytes/invocation
`856.265 → 856.161B` (`-0.01%`), hidden backend estimate `~1455.709MiB`
(`0.988x` VS write). Hot set `60/3,60/4,60/1,60/0` = `98.26%` GPU share. Per-row
split:

| seq/enc | GPU ms | VS write | opaque-dw d/p/v | depth-read d/p/v | alpha d/p/v | textured d/p/v |
|---|---:|---:|---:|---:|---:|---:|
| `60/3` | `10.942` | `437.378MiB` | `169/255,809/767,427` | `0/0/0` | `0/0/0` | `0/0/0` |
| `60/4` | `8.817` | `370.346MiB` | `0/0/0` | `265/370,367/1,111,101` | `243/340,364/1,021,092` | `265/370,367/1,111,101` |
| `60/1` | `8.397` | `437.402MiB` | `156/234,309/702,927` | `0/0/0` | `0/0/0` | `0/0/0` |
| `60/0` | `5.860` | `227.671MiB` | `74/105,169/315,507` | `52/75,166/225,498` | `0/0/0` | `126/180,335/541,005` |

Two opaque depth-writing rows alone (`60/3 + 60/1`) = `874.780MiB` VS write /
`490,118` triangles; depth-read/textured `60/4` = `370.346MiB` / `370,367`
triangles; `60/0` = `227.671MiB` mixed (opaque + depth-read/scissor/textured).

**Verdict.** Accepted as behavior-neutral instrumentation. It confirms the owner
rather than removing it and identifies the primary row/material classes for
bounded probes: the `60/3`/`60/1` opaque depth-write class and the `60/4`
depth-read/alpha/textured class, handled separately.

**Related.** [index-reuse-measurement](index.md) · follows
[index-reuse-measurement-geometry.03](index-reuse-measurement-geometry.03.md) · refined by
[index-reuse-measurement-classattr.02](index-reuse-measurement-classattr.02.md) · confirms
[hidden-backend-storage](../hidden-backend-storage/index.md) · classes feed
[primitive-reorder-diagnostics](../primitive-reorder-diagnostics/index.md) and [index-cache-locality](../index-cache-locality/index.md) ·
backend-state axes overlap [backend-shape-classifiers](../backend-shape-classifiers/index.md).
