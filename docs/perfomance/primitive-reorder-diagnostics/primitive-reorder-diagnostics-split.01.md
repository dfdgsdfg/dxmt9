---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: split
order: 01
title: Bounded Split-Large Indexed Probe (tooling + row 60/3 opaque)
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L10244-L10434
---

# Bounded Split-Large Indexed Probe (tooling + row 60/3 opaque)

**Question / hypothesis.** Does an *order-preserving* draw partition — splitting
one hot opaque depth-writing row's large indexed draws into smaller Metal draws,
without changing primitive order — move the hidden VS-buffer-write bucket? This
also lands the row/state-scoped tooling for the bounded split probe.

**Method.** New scope controls on the existing `DXMT9_SPLIT_LARGE_INDEXED_DRAWS`
diagnostic: `_ROW=SEQ/ENC`, `_ROWS=SEQ/ENC,...`,
`_CLASS=any|opaque-depth-write|nonopaque|depth-read|alpha-blend|scissor|textured|large4096`,
`_CLASSES=large4096,alpha-blend` (AND-list). Run:
`run_3dmark05_perf_probe.sh --suffix split-row-60-3-opaque-4096-gputrace-r1
--split-large-indexed-draws 4096 --split-large-indexed-draws-row 60/3
--split-large-indexed-draws-class opaque-depth-write --measure-index-reuse
--baseline-joined <measure-index-cache-gputrace-r1> --require-top-row-key-match
--require-top-pso-attribution --require-xcode-counter-coverage
--require-dxmt-join-coverage --max-top-{draw-call,vertex-count,triangle}-delta-ratio 0.05`.
No-gputrace smoke first confirmed only row `60/3` had split counters.

**Result.** Selector scoped correctly: only `60/3` split — `9` source draws →
`23` Metal draws (`+14` extra), `72,305` split primitives, all
opaque-depth-write. Top-3 GPU `27.944 → 27.520ms` (`-1.52%`); top-3 VS buffer
write `1245.082 → 1245.373MiB` (`+0.02%`); unexplained write `+0.02%`; VS
bytes/inv `-0.01%`. Hot-set (`60/3,60/4,60/1,60/0`) GPU `33.741 → 33.580ms`
(`-0.48%`); hot-set VS write `1472.747 → 1473.046MiB` (`+0.02%`). All strict
top-row/geometry/PSO/Xcode-counter gates passed.

**Verdict.** Rejected as a first-order GPU fix. Splitting the 9 large opaque
draws into 23 Metal draws does not reduce VS Buffer Device Memory Bytes Written,
unexplained hidden backend write, or bytes/invocation; the small GPU-time change
is below the stable memory-traffic signal (noise). A naive partition that
preserves order and only changes draw granularity does not touch the hidden
vertex/backend write bucket — the real reverse-probe signal needs order/locality
change, not partition.

**Related.** [[primitive-reorder-diagnostics]] · prior reverse signal:
[[primitive-reorder-diagnostics-reverse.01]] · next: [[primitive-reorder-diagnostics-split.02]]
· [[hidden-backend-storage]] (split does not move the TVB bucket) ·
[[index-cache-locality]] (the semantic-safe order/locality successor) ·
[[index-reuse-measurement]] (cache/reuse model) · [[baselines]] (measure-index-cache).
