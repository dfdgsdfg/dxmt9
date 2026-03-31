# Tutorial07

Repo-local D3D9/HLSL experiment binary used for the `dx-sdk-tutorial07` entry.

This is not a verbatim port of the original DirectX SDK sample. It is a
dxmt9-targeted harness that keeps the same broad feature envelope:

- `vs_2_0`
- `ps_2_0`
- textured rendering
- windowed present through Wine builtin `d3d9.dll`

The implementation intentionally stays on the validated fullscreen-triangle
shader route so it can serve as a stable second real-app experiment on Heroic
Wine 11.5.
