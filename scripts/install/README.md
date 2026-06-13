# scripts/install

Wine and Heroic prefix setup helpers. Stage dxmt9 builds into a target Wine
prefix or build native stubs for problematic upstream DLLs. Not wired to
Meson tests.

- `install_heroic_wine.sh` — builds Meson-owned artifacts when possible, then
  copies dxmt9 d3d9.dll, winemetal bridge, and unix provider into a Heroic Wine
  prefix. This includes the `winemetal.so` unix provider target; perf env
  filters and Metal-side fixes live there, so stale copies can invalidate
  3DMark05 sidecar runs.
- `install_heroic_experiment_prefix.sh` — orchestrates a full experiment
  prefix bootstrap (build apps, install Wine bits, run smoke launchers).
- `install_anno1701_bink_stub.sh` — builds and installs a 32-bit binkw32.dll
  stub into the Anno 1701 game directory.
- `install_anno1701_ddraw_stub.sh` — builds and installs a 32-bit ddraw.dll
  stub into the Anno 1701 game directory.
