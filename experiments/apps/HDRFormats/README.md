# HDRFormats

Repo-local D3D9/HLSL experiment binary used for the `dx-sdk-hdrformats` entry.

This harness focuses on the currently relevant HDR path for dxmt9:

- `ps_3_0`
- `A16B16G16R16F` render target
- offscreen HDR scene pass
- tonemap pass back to the window backbuffer

It is intentionally narrower than the original SDK sample but exercises the
core render-target and sampling path needed for HDR-style experiments.
