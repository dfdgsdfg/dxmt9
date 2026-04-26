# PerformanceProbe

Repo-local D3D9 micro-benchmark used to separate dxmt9 performance bottlenecks:

- `present-only`: clear + immediate present, no draw stress.
- `offscreen-heavy`: many fixed-function draws into an offscreen render target, no per-frame present.
- `many-draw`: many fixed-function draw calls to the backbuffer followed by immediate present.

The binary is intentionally fixed-function and shader-free so present pacing,
draw-call submission, and offscreen encoding costs can be compared without
shader compilation or pipeline-cache noise.
