# scripts/build_apps

D3D9 sample-app build helpers. Each script invokes the appropriate Meson
configure + ninja target (or downloads SDK assets) for one experiment app.
None of these are wired to Meson tests; they are launched manually or by
`scripts/run_apps/`.

- `build_basic_hlsl.sh` — DirectX SDK BasicHLSL sample.
- `build_dx9_fast_sanity_apps.sh` — bundle build for the fast-sanity suite.
- `build_hdrformats.sh` — DirectX SDK HDRFormats sample.
- `build_irrlicht_managed_lights.sh` — Irrlicht managed-lights demo.
- `build_multitextureterrain.sh` — dxmt9 multitexture terrain demo.
- `build_performance_probe.sh` — internal perf probe app.
- `build_simple_sample.sh` — DXUT simple sample.
- `build_tutorial07.sh` — DirectX SDK Tutorial07.
- `build_waterrt.sh` — dxmt9 water render-target demo.
