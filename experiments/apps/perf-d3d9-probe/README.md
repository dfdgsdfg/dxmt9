# perf-d3d9-probe

Repo-local D3D9 micro-benchmark used to separate dxmt9 performance bottlenecks:

- `present-only`: clear + immediate present, no draw stress.
- `offscreen-heavy`: many fixed-function draws into an offscreen render target, no per-frame present.
- `many-draw`: many fixed-function draw calls to the backbuffer followed by immediate present.
- `ffp-only`: explicit FFP path with `Set*Shader(NULL)` and a 4x4 default texture
  bound to stage 0; isolates FFP-vertex draw cost from any VS/PS-bound draws.
- `multi-rt`: ping-pongs across 4 render targets per frame, exercising
  render-pass-begin / RT-change paths used by deferred and post-process workloads.
- `depth-heavy`: per-frame depth-only pass followed by depth-as-texture sampling
  for shadow-map-style cycles, exercising depth load/store actions.
- `skeletal`: 64 draws per frame with a 184-float `SetVertexShaderConstantF`
  block (vsFloatConst[16..200]) refreshed before every draw to simulate
  46-bone skeletal animation uniform churn.

The probe is shader-free (FFP draws) so present pacing, draw-call submission,
RT-change cost, depth-pass cost, and uniform-update traffic can be compared
without shader compilation or pipeline-cache noise.

Mode selection: pass the mode name as `argv[1]` (the launcher scripts under
`experiments/launchers/dxmt9-perf-*.sh` do this) or set
`DXMT9_PROBE_MODE`. Frame and draw counts can be overridden via
`DXMT9_PROBE_FRAMES` and `DXMT9_PROBE_DRAWS`.
