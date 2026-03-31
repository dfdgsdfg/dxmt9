# BasicHLSL

This experiment app is a small D3D9 host that exercises a `vs_2_0` / `ps_2_0`
textured shader path derived from the legacy DirectX SDK `BasicHLSL` sample,
without depending on DXUT.

- `BasicHLSL.cpp`: repo-local Win32 + D3D9 host used for experiments
- `BasicHLSL.fx`: preserved reference effect derived from the Microsoft sample
- `BasicHLSL.hlsl`: runtime shader used by the experiment binary

The executable is built locally by `scripts/build_basic_hlsl.sh` and written to
this directory as `BasicHLSL.exe`.
