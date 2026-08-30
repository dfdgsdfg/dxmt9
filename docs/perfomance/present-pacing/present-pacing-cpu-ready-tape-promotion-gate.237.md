---
domain: present-pacing
workload: 3DMark05 GT2
title: "Present-Pacing #237 - CPU-Ready Tape Restores Progress but Misses the Locality Gate"
type: leaf
status: current
updated: 2026-08-30
source: experiments/output/conf-d3d9-triangle-cpu-pipeline-promotion-tape-on-final-20260830/result.json; experiments/output/app-d3d9-3dmark05-cpu-pipeline-promotion-tape-off-gt2-20260830/result.json; experiments/output/app-d3d9-3dmark05-cpu-pipeline-promotion-tape-on-gt2-20260830/result.json
related: specs/backend/encode-scheduling/gap.md; specs/backend/encode-scheduling/requirements.md; docs/perfomance/present-pacing/present-pacing-cpu-ready-multisource-planner.234.md
---

# Present-Pacing #237 - CPU-Ready Tape Restores Progress but Misses the Locality Gate

## Result

The clean Apple-clang build passed the focused native suite, the composed TLA+
suite, all three canonical Wine builds, install-name audit, and a Tape-on
triangle smoke. A same-build no-gputrace GT2 pair then completed normally with
zero command-chunk rejects, skipped-no-pipeline draws, or post-effect fatal
counters.

| Metric | Tape off | Tape on | Delta |
|---|---:|---:|---:|
| Presents | 1,859 | 1,871 | — |
| Sampled average FPS | 28.646 | 28.869 | +0.78% |
| Command buffers / Present | 3.999 | 4.012 | +0.31% |
| Render passes / Present | 15.777 | 15.892 | +0.73% |
| Tile preservation MiB / Present | 103.651 | 106.690 | +2.93% |
| GPU command-buffer ms / Present | 2.033 | 2.096 | +3.12% |
| Ready-depth average | 1.000 | 2.200 | +120.03% |

The FPS movement is inside the one-run noise band. The three strict locality
gates fail, so `DXMT9_CPU_READY_TAPE` remains default off and GT1, GT3, and SFIV
promotion runs are intentionally not started.

## Residual shape

Tape-on creates real producer/encode overlap, but the retained replay suffix is
still incomplete at some cross-source pass decisions:

- `render_pass_no_lookahead_suffix_exhausted=10,854`;
- `render_pass_natural_short_cross_close_matched=337`, split into 178 Clear and
  159 render-target-change closes;
- `cpu_ready_session_legacy_rollback=21,414`;
- Arena residency peaks at 638 pages, with eight admission waits totaling
  53.073 ms; and
- 1,660 retained heads find their successor, so the remaining problem is not
  the original coordinator wedge.

These counters do not yet separate a genuinely final current-source fragment
from an incomplete retained cross-source suffix. The next bounded change is
that observability split, followed by extending only the proven incomplete
suffix. Capacity tuning and FPS claims stay blocked until CB/pass/tile shape is
flat.
