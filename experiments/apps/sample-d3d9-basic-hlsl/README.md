# sample-d3d9-basic-hlsl

This experiment app is a small D3D9 host that exercises a `vs_2_0` / `ps_2_0`
textured shader path derived from the legacy DirectX SDK `sample-d3d9-basic-hlsl` sample,
without depending on DXUT.

- `sample-d3d9-basic-hlsl.cpp`: repo-local Win32 + D3D9 host used for experiments
- `sample-d3d9-basic-hlsl.fx`: preserved reference effect derived from the Microsoft sample
- `sample-d3d9-basic-hlsl.hlsl`: runtime shader used by the experiment binary

The executable is built locally by `scripts/build_apps/build_sample-d3d9-basic-hlsl.sh` and written to
this directory as `sample-d3d9-basic-hlsl.exe`.
