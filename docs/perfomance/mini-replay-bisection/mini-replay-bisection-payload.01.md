---
domain: mini-replay-bisection
workload: 3DMark05 GT1
subcategory: payload
order: 01
title: Geometry Payload Smoke Scout
date: undated
type: scout
status: tooling
source: specs/perfomance.plan.md#L13723-L14202
---

# Geometry Payload Smoke Scout

**Question / hypothesis.** Close the geometry-bytes gap from
[mini-replay-bisection-harness.01](mini-replay-bisection-harness.01.md): can the runtime dump raw index + referenced
stream bytes (and cbufs) for a selected hot draw window without mutating render
state, producing a replayable payload?

**Method.** `scripts/tools/run_3dmark05_perf_probe.sh --no-gputrace
--encoder-breakdown-seq 60 --dump-indexed-geometry [--dump-indexed-geometry-cbufs]
--dump-indexed-geometry-max-draws N` with reverse-indexed row/class/draw-window
filters. `--dump-indexed-geometry` implies `DXMT9_MEASURE_INDEX_REUSE=1`, does not
reorder primitives, and writes `seq<seq>-enc<enc>-draw<draw>-slot<n>.{index,stream0}.bin`
plus `.meta` under `traces/<run-id>/analysis/geometry/`. Targeted the `60/2`
alpha/depth-read/textured group (VS `0x7836c3b4c98a465b`, PS `0x11cc89f85cc54054`)
and later the screen-blend window `71..188`.

**Result.** Smoke runs wrote valid triplets (`index_range_valid=1`,
`stream0_range_valid=1`, `wrote_index=1`, `wrote_stream0=1`); stream0 payloads
`74,208B..142,320B`. A 16-draw screen-blend scout (`60/2` draw `71..86`) dumped
16 triplets + cbufs (112 files), `172,932B` index + `1,030,032B` stream0. Shader
filters (`--dump-indexed-geometry-vs/-ps`) and `select_3dmark05_payload_window.py`
were added because row-local draw windows are **not** robust cross-run selectors
(the target group migrated `60/2`→`60/4` between runs).

**Verdict.** TOOLING. Payload capture works. Two fidelity gaps surfaced: (1) some
VS bind stream1 at Metal `buffer(6)` colliding with the replay cbuf slots; (2) the
16-draw window spans 6 VS/PS pairs, so a single-PSO runner cannot replay it. Both
fed [mini-replay-bisection-harness.02](mini-replay-bisection-harness.02.md).

**Related.** [mini-replay-bisection](index.md) · [mini-replay-bisection-harness.01](mini-replay-bisection-harness.01.md) ·
[mini-replay-bisection-harness.02](mini-replay-bisection-harness.02.md) · [index-reuse-measurement](../index-reuse-measurement/index.md) · [overview-3dmark05-gt1](../overview-3dmark05-gt1.md)
