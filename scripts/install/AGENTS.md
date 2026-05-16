# scripts/install

Wine and Heroic prefix setup helpers. Stage dxmt9 builds into a target Wine
prefix or build native stubs for problematic upstream DLLs. Not wired to
Meson tests.

- `install_heroic_wine.sh` — copies dxmt9 d3d9.dll, winemetal bridge, and
  unix provider into a Heroic Wine prefix.
- `install_heroic_experiment_prefix.sh` — orchestrates a full experiment
  prefix bootstrap (build apps, install Wine bits, run smoke launchers).
