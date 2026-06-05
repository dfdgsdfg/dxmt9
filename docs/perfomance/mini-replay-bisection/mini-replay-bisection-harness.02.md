---
domain: mini-replay-bisection
workload: 3DMark05 GT1
subcategory: harness
order: 02
title: Multi-PSO Mini-Replay Harness
date: undated
type: tooling
status: tooling
source: specs/perfomance.plan.md#L14235-L14360
---

# Multi-PSO Mini-Replay Harness

**Question / hypothesis.** Extend `run_3dmark05_mini_replay.py` so it can replay a
multi-shader screen-blend slice whose VS binds stream1 at Metal `buffer(6)` — the two
fidelity blockers found by [[mini-replay-bisection-payload.01]].

**Method.** The runner (`scripts/tools/run_3dmark05_mini_replay.py`) was changed to:
scan the original dumped MSL buffer bindings and pick free high slots for replay
cbufs (recording `vs_cbuf_slots` / `fs_cbuf_slots`); bind extra vertex stream
slots to a zero-filled dummy buffer when only stream0 was dumped; then add real
`.stream1.bin` support so the geometry dumper writes extra `.streamN.bin` payloads
with `streamN_*` metadata preserved as `geometry.streams`. A multi-PSO path was
added so one manifest can carry several VS/PS pairs.

**Result.** The dominant-shader slice (`60/2` draw `81..86`, VS `0xc949d543d4cd5f19`,
PS `0xcc5a988ed3599a6f`) compiled after dynamic slot allocation:

```text
mini replay draws=6 repeat=1
vs_cbuf_slots={vsconsts:29, ffpvs:28}
fs_cbuf_slots={psconsts:29, ffpps:28}
dummy_vertex_buffer_slots=[6]
```

A stream-aware rerun bound real `stream1` for all draws
(`actual_extra_vertex_buffer_slots=[6]`). The full 16-draw manifest then ran via
the multi-PSO path: `mini replay draws=16 repeat=1`, `shader_variant_count=6`.

**Verdict.** TOOLING. Replay-quality gap is no longer stream1 or single-PSO
coverage. The remaining gap is **performance evidence**: capture the slice with
Xcode counters and compare against the original hot encoder — done in
[[mini-replay-bisection-replay.01]].

**Related.** [[mini-replay-bisection]] · [[mini-replay-bisection-payload.01]] ·
[[mini-replay-bisection-replay.01]] · [[overview-3dmark05-gt1]]
