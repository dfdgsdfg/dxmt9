# DXUT SimpleSample

Repo-local D3D9/HLSL experiment binary used for the `dxut-simple-sample` entry.

This is a dxmt9-targeted harness, not a verbatim import of the original DXUT
sample. It keeps the relevant experiment shape:

- `vs_2_0`
- `ps_2_0`
- two-pass state changes
- alpha blending
- textured present under Wine builtin `d3d9.dll`

The purpose is to expand the real-app experiment surface without stepping
outside the currently validated fullscreen-triangle shader route.
