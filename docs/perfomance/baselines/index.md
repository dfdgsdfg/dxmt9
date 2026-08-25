---
domain: baselines
workload: 3DMark05 GT1
title: "Baselines — the reference captures every other experiment compares against"
type: domain-index
status: current
updated: 2026-08-25
source: docs/perfomance/overview-3dmark05-gt1.md; docs/perfomance/baselines/baselines-wild-fps-refresh.04.md
related: docs/perfomance/baselines/overview.md; docs/perfomance/baselines/log.md
---

# Baselines — the reference captures every other experiment compares against

Latest tracked result: the 2026-08-25 current-cap sweep preserves the post-PE-boundary throughput range at GT1 `30.6`, GT2 `28.3`, GT3 `64.9`, and SFIV `43.3` sampled FPS.

## Start Here

- [Current overview](overview.md) - latest conclusion and active gates only.
- [Historical log](log.md) - long-form chronology moved from the old domain root.
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [baselines-wild-fps-refresh.04](baselines-wild-fps-refresh.04.md) — 2026-08-25 current-cap single-run sweep at `e32da591`: GT1 `30.646`, GT2 `28.311`, GT3 `64.875`, SFIV `43.252`; normal captures, production CB/pass locality, and zero GPU errors.
- [baselines-wild-fps-refresh.03](baselines-wild-fps-refresh.03.md) — 2026-08-23 post-PE-boundary single-run sweep: GT1 `30.91`, GT2 `29.04`, GT3 `65.86`, SFIV `44.22`; all deltas to the current sweep remain inside the approximately `+/-3%` ambient band.
- [baselines-serial-partition-ab.02](baselines-serial-partition-ab.02.md) —
  2026-08-04 matched pre-partition/HEAD regression gate: GT1 `-0.42%`, GT2
  `+1.11%`, GT3 `-0.11%`, and cooled SFIV `+0.67%`; all perf-neutral with
  zero GPU errors and bridge rejects.
- [baselines-wild-fps-refresh.02](baselines-wild-fps-refresh.02.md) — 2026-08-21 README refresh at `8ddfe5fa`, single-run sweep: GT1 `30.1`, GT2 `28.2`, GT3 `64.4`, SFIV `43.6` (90 s window); +11-18% on the 3DMark scenes since 08-04, SFIV flat.
- [baselines-wild-fps-refresh.01](baselines-wild-fps-refresh.01.md) — 2026-08-03 post-encode-session-merge refresh: GT1 `27.99-29.07`, GT2 `25.94-26.81`, GT3 `61.13-61.14`, SFIV avg `43.26`/median `59.87`; GT1 `+18-22%` and GT3 `+64%` since 07-31 attributed to the SWVP hoist + debug-group gating, refactor itself flat on GT2.
