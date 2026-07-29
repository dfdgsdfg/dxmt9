---
domain: baselines
workload: 3DMark05 GT1
subcategory: visual-capture
order: 02
title: v0.0.1 Historical Screenshot Diff Triage
date: 2026-06-06
type: correctness-triage
status: superseded-by-v0.0.3
outdated: evidence-missing
source: experiments/output/app-d3d9-3dmark05-visual-v001-capture40-r1/actual.png, experiments/output/app-d3d9-3dmark05-visual-current-capture40-r2/actual.png, experiments/output/app-d3d9-3dmark05-visual-current-capture40-r2/image-comparison-vs-v001-capture40.md, experiments/output/app-d3d9-3dmark05-visual-current-capture40-r2/image-diff-vs-v001-capture40.png
---

# Baselines / Visual Capture 02 - `v0.0.1` historical screenshot diff triage

> **Outdated — every artifact this leaf cites in `source:` is gone from disk.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Date.** 2026-06-06.

**Correction.** As of 2026-06-18, `v0.0.3` is the last known visual-safe GT1
code point. This note remains useful as an earlier screenshot-diff triage
record, but `v0.0.1` is not the current correctness/alignment anchor. New
visual-safety decisions should compare against `v0.0.3`.

**Question.** At the time, could the `v0.0.1` tag be used as an early coherent
screenshot-diff reference when GT1 changes looked suspicious, and how should
diff images be interpreted without treating that tag as the last safe code
point?

**Method.**

- Added a detached worktree at `/Users/dididi/workspaces/dxmt9-v0.0.1` from
  tag `v0.0.1` (`297997a`).
- Built the tag runtime with the current build lanes:
  `build-x86_64-builtin`, `build-win32-x64-builtin`, and
  `build-win32-x86-builtin`. The unix provider was configured with
  `-Dbuild_tests=false` because the old test manifest blocks configure, but
  the runtime target builds cleanly.
- Ran GT1 through the current `run_experiment.py` harness so timeout,
  screenshot, prefix staging, and Wine selection stayed identical:
  `DXMT_EXPERIMENT_PROFILE=perf ... run app-d3d9-3dmark05 --timeout 180 --capture-delay-sec 40 --output-suffix visual-v001-capture40-r1`.
- Re-staged current HEAD and ran the same capture:
  `--output-suffix visual-current-capture40-r2`.
- Compared the two screenshots with:
  `python3 scripts/tools/compare_experiment_images.py --crop-bottom 96 --active-threshold 5`.

**Artifacts.**

- `experiments/output/app-d3d9-3dmark05-visual-v001-capture40-r1/actual.png`
  (`v0.0.1`; overlay: `FPS 7`, `Time 0:25.11`, `Frame 351`).
- `experiments/output/app-d3d9-3dmark05-visual-current-capture40-r2/actual.png`
  (current HEAD; overlay: `FPS 11`, `Time 0:25.63`, `Frame 483`).
- `experiments/output/app-d3d9-3dmark05-visual-current-capture40-r2/image-comparison-vs-v001-capture40.md`.
- `experiments/output/app-d3d9-3dmark05-visual-current-capture40-r2/image-diff-vs-v001-capture40.png`.

**Observation.**

The `v0.0.1` tag was a useful early coherent screenshot reference for GT1. Its
textures, foreground silhouettes, floor/wall mapping, and lighting composition
are coherent, so the tag remains useful when checking broad
texture/color/geometry drift. It is not the last known visual-safe code point:
that role belongs to `v0.0.3`. The current capture also looks coherent and does
not show the recent texture-coordinate/cbuf-identity corruption class.

The screenshot diff is useful because it makes broad texture, color, and
geometry drift visible against that early coherent anchor. Use the diff image as a
triage tool for obvious classes such as black/translucent vertices, broken UVs,
missing textures, wrong fog/color, and cbuf-identity artifacts. The numeric diff
from a time-based screenshot is intentionally not a standalone pass/fail gate:
same `capture-delay-sec=40` does not mean same animation frame. `v0.0.1`
captured frame `351`, current captured frame `483`, and the camera/foreground
geometry moved. The comparison therefore reports large expected changes:
`changed_pct=95.123545%`, cropped `changed_pct=98.417155%`, full-frame
`SSIM=0.549463`, cropped `SSIM=0.283971`.

**Verdict.** `v0.0.3` is the GT1 visual correctness / alignment anchor and the
last known visual-safe code point. Keep the `v0.0.1` screenshots and generated
heatmap diffs only as historical broad-corruption triage artifacts; the diff is
useful because it highlights broad texture, color, and geometry drift against a
coherent image. A candidate that changes resource identity, constant uploads,
vertex layout, primitive order, or batching should now survive qualitative
inspection against `v0.0.3` before being treated as visually safe. Require
same-frame replay, same-input mini replay, or a draw/window-level proof only
before treating raw pixel-difference percentages from time-based screenshots as
exact semantic failures.

```mermaid
flowchart TD
  Anchor["v0.0.1 tag\nhistorical coherent screenshot\ndiff triage only"] --> Build["build tag runtime\nunix + x64 PE + x86 PE"]
  V003["v0.0.3 tag\nlast known visual-safe code point"] --> CurrentGate["new visual-safety decisions"]
  Current["current HEAD"] --> StageCurrent["stage current runtime"]

  Build --> RunOld["run_experiment.py\ncapture-delay-sec=40\nFrame 351"]
  StageCurrent --> RunNew["run_experiment.py\ncapture-delay-sec=40\nFrame 483"]

  RunOld --> Compare["compare_experiment_images.py\nfull + crop-bottom-96"]
  RunNew --> Compare

  Compare --> Diff["heatmap diff\nblack/translucent vertices\nUV/color/fog/cbuf drift triage\nlarge raw delta from camera/frame drift"]
  Compare --> Smoke["visual smoke\ntexture/geometry gross corruption check"]

  Smoke --> Verdict["v0.0.3 is the safety anchor\nv0.0.1 diff retained for triage"]
  Diff --> Verdict
  Verdict --> CurrentGate

  Verdict --> StrongProof["pixel-exact proof path\nsame-frame gputrace\nmini replay\nor native/reference same input"]

  classDef good fill:#d6f5d6,stroke:#2b7a2b,color:#063
  classDef warn fill:#fff3cd,stroke:#a80,color:#640
  classDef bad fill:#f8d7da,stroke:#a33,color:#600
  class Anchor,Smoke,Verdict good
  class Build,Current,StageCurrent,RunOld,RunNew,Compare,Diff warn
  class StrongProof bad
```
