---
domain: baselines
workload: 3DMark05 GT1
title: "Baselines — the reference captures every other experiment compares against"
type: domain-index
status: current
updated: 2026-08-04
source: docs/perfomance/overview-3dmark05-gt1.md; docs/perfomance/baselines/baselines-serial-partition-ab.02.md
related: docs/perfomance/baselines/overview.md; docs/perfomance/baselines/log.md
---

# Baselines — the reference captures every other experiment compares against

Latest tracked row: `H15` - A latest black-geometry / transparent-weapon report invalidates the current performance direction (rejected as a wall; accepted as a baseline gate).

## Start Here

- [Current overview](overview.md) - latest conclusion and active gates only.
- [Historical log](log.md) - long-form chronology moved from the old domain root.
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [baselines-serial-partition-ab.02](baselines-serial-partition-ab.02.md) —
  2026-08-04 matched pre-partition/HEAD regression gate: GT1 `-0.42%`, GT2
  `+1.11%`, GT3 `-0.11%`, and cooled SFIV `+0.67%`; all perf-neutral with
  zero GPU errors and bridge rejects.
- [baselines-wild-fps-refresh.01](baselines-wild-fps-refresh.01.md) — 2026-08-03 post-encode-session-merge refresh: GT1 `27.99-29.07`, GT2 `25.94-26.81`, GT3 `61.13-61.14`, SFIV avg `43.26`/median `59.87`; GT1 `+18-22%` and GT3 `+64%` since 07-31 attributed to the SWVP hoist + debug-group gating, refactor itself flat on GT2.
