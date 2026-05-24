# scripts/build_apps

D3D9 sample-app build helpers. Each script invokes the appropriate Meson
configure + ninja target (or downloads SDK assets) for one experiment app.
None of these are wired to Meson tests; they are launched manually or by
`scripts/run_apps/`.

- `build_sample-d3d9-basic-hlsl.sh` — DirectX SDK sample-d3d9-basic-hlsl sample.
- `build_conf-d3d9-fast-sanity.sh` — bundle build for the fast-sanity suite.
- `build_sample-d3d9-hdr-formats.sh` — DirectX SDK sample-d3d9-hdr-formats sample.
- `build_sample-d3d9-irrlicht-lights.sh` — Irrlicht managed-lights demo.
- `build_sample-d3d9-multitexture-terrain.sh` — dxmt9 multitexture terrain demo.
- `build_perf-d3d9-probe.sh` — internal perf probe app (shared by perf-d3d9-* entries).
- `build_sample-d3d9-dxut-simple.sh` — DXUT simple sample.
- `build_sample-d3d9-tutorial07.sh` — DirectX SDK sample-d3d9-tutorial07.
- `build_sample-d3d9-water-rt.sh` — dxmt9 water render-target demo.
- `build_perf-d3d9-bridge-empty.sh` — boundary B2 bridge-throughput probe.
- `build_perf-d3d9-chain-parametric.sh` — boundary B3+B4 sub-CB chain probe.
- `build_perf-d3d9-encode-replay.sh` — boundary-isolated encode-only probe.
- `build_perf-d3d9-present-loop.sh` — boundary B6 drawable-acquire probe.
