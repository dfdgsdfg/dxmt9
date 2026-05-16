# scripts/build_apps

D3D9 sample-app build helpers. Each script invokes the appropriate Meson
configure + ninja target (or downloads SDK assets) for one experiment app.
None of these are wired to Meson tests; they are launched manually or by
`scripts/run_apps/`.

- `build_dx-sdk-basichlsl.sh` — DirectX SDK BasicHLSL sample.
- `build_dx9_fast_sanity_apps.sh` — bundle build for the fast-sanity suite.
- `build_dx-sdk-hdrformats.sh` — DirectX SDK HDRFormats sample.
- `build_irrlicht-managed-lights.sh` — Irrlicht managed-lights demo.
- `build_dxmt9-multitexture-terrain.sh` — dxmt9 multitexture terrain demo.
- `build_performance_probe.sh` — internal perf probe app (shared by dxmt9-perf-* entries).
- `build_dxut-simple-sample.sh` — DXUT simple sample.
- `build_dx-sdk-tutorial07.sh` — DirectX SDK Tutorial07.
- `build_dxmt9-water-rt.sh` — dxmt9 water render-target demo.
- `build_d3d9_intent_probe.sh` — shared D3D9 intent probe app for FFP,
  render-state, blit/copy, stateblock, query, and vertex-blend catalogue
  entries.
- `build_dxmt9-perf-bridge-empty.sh` — boundary B2 bridge-throughput probe.
- `build_dxmt9-perf-chain-parametric.sh` — boundary B3+B4 sub-CB chain probe.
- `build_dxmt9-perf-encode-replay.sh` — boundary-isolated encode-only probe.
- `build_dxmt9-perf-present-loop.sh` — boundary B6 drawable-acquire probe.
