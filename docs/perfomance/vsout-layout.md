# VSOut Layout — visible varying-width attempts to explain the VS-write bucket

> Part of the 3DMark05 GT1 GPU-bottleneck investigation. Root map: [[overview-3dmark05-gt1]].

## Scope & question

This domain owns every attempt to explain or reduce the dominant Xcode
"VS Buffer Device Memory Bytes Written" bucket (~1.6 GiB across the top-3 render
encoders) by changing the **visible** per-vertex shader-stage-out shape — the MSL
`VSOut` struct width / field set. The hypotheses span blanket varying trimming,
exact FS-read liveness, dropping a single field (point-size), the extreme
position-only lower bound (plus its fragment-only control), and half-precision
varyings. Almost every one was **rejected** as not the first-order owner; the one
useful result is a semantically-*safe* liveness trim that nonetheless does not move
the bucket.

## Hypotheses & verdicts

| # | Hypothesis | Verdict | Evidence |
|---|-----------|---------|----------|
| H1 | Trimming unused varyings (`DXMT9_TRIM_UNUSED_VARYINGS`) shrinks the VS-write bucket | rejected | [[vsout-layout-varying.01]] |
| H2 | Exact FS-read liveness trim (keep only fields the FS reads) moves it where blanket trim did not | rejected | [[vsout-layout-varying.02]] |
| H3 | A liveness trim can at least be done safely (no pixel change) | rejected (perf), but **semantically safe** (0 changed px, SSIM 1.000) | [[vsout-layout-varying.03]] |
| H4 | Dropping only `VSOut.pointSize` (184B→180B) moves the bucket | rejected | [[vsout-layout-pointsize.01]] |
| H5 | Extreme position-only VSOut (184B→16B) drops the bucket proportionally | rejected (non-proportional; correctness-invalid diagnostic) | [[vsout-layout-position.01]] |
| H6 | Control: constant-fragment alone (184B VSOut unchanged) reproduces the same delta → mover is fragment/raster, not width | rejected as width owner (control confirms) | [[vsout-layout-position.02]] |
| H7 | Half-precision varyings reduce hidden TVB/parameter storage | rejected (fails GPU-time TVB mechanism gate) | [[vsout-layout-half.01]] |

## Verification methods

- **`DXMT9_TRIM_UNUSED_VARYINGS=1`** — collapse ordinary MSL VSOut to declared-used
  fields; proves blanket width reduction does not move the bucket.
- **`--trim-vsout-to-fs-reads`** (mini-replay) — exact FS-read liveness manifest;
  proves liveness trim is pixel-safe but perf-inert.
- **`DXMT9_PROBE_DROP_VSOUT_POINT_SIZE=1`** (`--drop-vsout-point-size`) — single-field
  drop (key `0xfff→0x7ff`); isolates the point-size/`[[point_size]]` path.
- **`DXMT9_PROBE_POSITION_ONLY_VSOUT=1`** — extreme `16B` lower bound; **correctness-invalid
  diagnostic** (forces constant fragment), used only as a bandwidth classifier.
- **`DXMT_DEBUG_FORCE_FRAGMENT_COLOR=1`** (`--force-fragment-color`) — the control that
  separates fragment/raster effect from VSOut width; also a diagnostic, not a fix.
- **`DXMT9_PROBE_HALF_VSOUT=1`** (`--probe-half-vsout`) — half4/half precision stage-out;
  the non-reorder backend-shape axis.
- **`--dump-shaders` + finalizer `--require-shader-dump-matches`** — proves the top
  rows actually used the modified VSOut sources.
- **`--require-top-vs-buffer-write-decrease`** / **`--require-tvb-mechanism-proof`** —
  the pass/fail gates these probes are measured against (VS write, hidden backend,
  VS invocations, and GPU time must all strictly decrease).

## Experiment dependency graph

```mermaid
flowchart TD
  V1["vsout-layout-varying.01<br/>trim-varyings recheck"]:::rejected
  V2["vsout-layout-varying.02<br/>FS-read liveness trim"]:::rejected
  V3["vsout-layout-varying.03<br/>dump-first liveness replay<br/>(semantic-safe)"]:::safe
  P1["vsout-layout-pointsize.01<br/>drop point_size 184B→180B"]:::rejected
  POS["vsout-layout-position.01<br/>position-only 184B→16B<br/>(correctness-invalid)"]:::rejected
  FRAG["vsout-layout-position.02<br/>fragment-only control<br/>(184B unchanged)"]:::rejected
  HALF["vsout-layout-half.01<br/>half VSOut precision"]:::rejected
  CONC["visible VSOut width is<br/>NOT the owner<br/>→ hidden-backend-storage"]:::concl

  V1 -->|"narrowed-from"| V2
  V2 -.->|"pixel-safety check"| V3
  V2 -->|"narrowed-from"| P1
  P1 -->|"escalated-to lower bound"| POS
  POS <-->|"control isolates mover"| FRAG
  POS -->|"rejected→next axis"| HALF
  V1 --> CONC
  V3 --> CONC
  P1 --> CONC
  FRAG --> CONC
  HALF --> CONC

  classDef rejected fill:#f8d7da,stroke:#a33,color:#600
  classDef safe fill:#fff3cd,stroke:#a80,color:#640
  classDef concl fill:#d6f5d6,stroke:#2b7a2b,color:#063
```

## Results synthesis

The recurring lesson across this domain is unambiguous: **changing the visible MSL
`VSOut` / varying width changes the generated MSL/IR shape but does not move the
Xcode VS-write bucket.** A blanket trim, an exact FS-read liveness trim, dropping
point-size, and even an `88.4x` collapse to position-only (`184B→16B`) all left the
bucket essentially intact (`≤ -0.016MiB` for the non-destructive trims; the `-79MiB`
seen under position-only was reproduced by the fragment-only control with VSOut
unchanged, proving the mover was fragment/raster/backend interaction, not width).
Half-precision varyings shifted only `-2.44%` of top VS write while GPU time
regressed `+3.40%`, failing the TVB mechanism gate. The single positive finding is
correctness, not performance: the dump-first liveness trim is semantically safe
(0 changed pixels, SSIM 1.000) — safe to do, not worth doing for perf.

Two of these probes are **correctness-invalid diagnostics**, usable only as
classifiers: `DXMT9_PROBE_POSITION_ONLY_VSOUT` (forces position-only + constant
fragment) and `DXMT_DEBUG_FORCE_FRAGMENT_COLOR` (strips fragment work). They must
never be treated as optimization candidates. The remaining `[[clip_distance]]` axis
was audited and is already absent from the hot frame50 rows, so it offers no further
width to remove.

Nothing in this domain is still open: visible VSOut width is closed as the owner.
The surviving owner is hidden Apple GPU vertex-stage / tiler / parameter (TVB)
backend storage that scales with VS-invocation count × per-vertex VSOut bytes — see
[[hidden-backend-storage]].

## Cross-references
- [[hidden-backend-storage]] — the surviving owner this whole domain points to (hidden TVB/parameter storage, VS-write density model).
- [[tvb-mechanism-proof]] — the `--require-tvb-mechanism-proof` gate that half-VSOut failed; the accepted row-local mechanism proof.
- [[shader-codegen]] — sibling axis testing vertex temp/scratch trim and offline Metal codegen (also below visible stage-out).
- [[backend-shape-classifiers]] — where the correctness-invalid position-only / fragment-only diagnostics live as state-shape classifiers.
- [[overview-3dmark05-gt1]] — root map and priority DAG.
