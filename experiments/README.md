# Experiments

Experiments are end-to-end runs against real D3D9 applications under the
current `dxmt9` runtime.

Primary entrypoint:

```sh
python3 scripts/run_experiment.py list
python3 scripts/run_experiment.py run dxmt9-wsi-present-local
python3 scripts/run_experiment.py run dx-sdk-basichlsl --wine-root "$WINE_ROOT"
python3 scripts/run_experiment.py run dx-sdk-tutorial07 --wine-root "$WINE_ROOT"
python3 scripts/run_experiment.py run dx-sdk-hdrformats --wine-root "$WINE_ROOT"
python3 scripts/run_experiment.py run dxut-simple-sample --wine-root "$WINE_ROOT"
python3 scripts/run_experiment.py run irrlicht-managed-lights --wine-root "$WINE_ROOT"
python3 scripts/run_experiment.py run anno-1404-gold --wine-root "$WINE_ROOT" --prefix "$HOME/Games/_Prefixes/Anno 1404 Gold Edition"
python3 scripts/run_experiment.py run street-fighter-iv-benchmark --wine-root "$WINE_ROOT" --binary "/path/to/StreetFighterIV_Benchmark.exe"
python3 scripts/run_experiment.py run street-fighter-iv-benchmark-crossover-oracle --wine-root "$HOME/Applications/CrossOver.app/Contents/SharedSupport/CrossOver" --wine-bin "$HOME/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine" --prefix "$HOME/Library/Application Support/CrossOver/Bottles/Heroic" --binary "/path/to/StreetFighterIV_Benchmark.exe"
```

DX9 regression suite:

```sh
bash scripts/run_dx9_regression_suite.sh --wine-root "$WINE_ROOT"
```

Builtin-oracle compare suite for selected DX9 sample apps:

```sh
bash scripts/run_dx9_oracle_compare_suite.sh --wine-root "$WINE_ROOT"
```

One-shot wrappers for the verified real apps:

```sh
bash scripts/run_basic_experiment.sh --wine-root "$WINE_ROOT"
bash scripts/run_tutorial07_experiment.sh --wine-root "$WINE_ROOT"
bash scripts/run_hdrformats_experiment.sh --wine-root "$WINE_ROOT"
bash scripts/run_simple_sample_experiment.sh --wine-root "$WINE_ROOT"
bash scripts/run_irrlicht_managed_lights_experiment.sh --wine-root "$WINE_ROOT"
bash scripts/run_anno1404_experiment.sh
bash scripts/run_sfiv_benchmark_experiment.sh --binary "~/Downloads/StreetFighterIV_Benchmark.exe"
bash scripts/run_sfiv_benchmark_crossover_oracle.sh --binary "~/Downloads/StreetFighterIV_Benchmark.exe"
```

Permanent-prefix installer for Heroic:

```sh
bash scripts/install_heroic_experiment_prefix.sh --prefix "$HOME/.wine-dxmt9-heroic" --wine-root "$WINE_ROOT"
```

The runner:

- reads [`CATALOGUE.toml`](./CATALOGUE.toml)
- stages `d3d9.dll`, `winemetal.dll`, `dxmt9unix.dll`, `winemetal.so`, and the
  private `dxmt9unix.so` helper into a Wine runtime/prefix
- runs the selected launcher
- captures the presented back buffer directly from dxmt9 when `capture_frame` is set
- falls back to window capture only when an internal frame dump is unavailable
- writes `actual.png`, `diff.png`, `ssim.txt`, `dxmt9.log`, and `result.json`

Current note:

- the committed sample references are stale for several shader apps
- use the builtin-oracle compare suite to judge current renderer parity
- current builtin-vs-dxmt9 sample parity:
  - `dx-sdk-basichlsl`: `0.9539`
  - `dx-sdk-tutorial07`: `0.9094`
  - `dxut-simple-sample`: `0.9426`
  - `irrlicht-managed-lights`: `0.9979`

Current verified bootstrap entry:

- `dxmt9-wsi-present-local`

This is not a catalogue target from the experiments spec. It exists to validate
the launcher, frame-dump capture, and SSIM workflow locally before external
sample binaries are staged.

Current verified real application entry:

- `dx-sdk-basichlsl`
  - Heroic Wine 11.6 builtin path
  - 240 frames
  - direct backbuffer capture
  - `ssim = 1.0000`
- `dx-sdk-tutorial07`
  - Heroic Wine 11.6 builtin path
  - 180 frames
  - direct backbuffer capture
  - `ssim = 1.0000`
- `dxut-simple-sample`
  - Heroic Wine 11.6 builtin path
  - 180 frames
  - direct backbuffer capture
  - `ssim = 1.0000`
- `dx-sdk-hdrformats`
  - Heroic Wine 11.6 builtin path
  - 180 frames
  - direct backbuffer capture
  - `ssim = 1.0000`
- `irrlicht-managed-lights`
  - Heroic Wine 11.6 builtin path
  - 180 frames
  - direct backbuffer capture
  - `ssim = 1.0000`

Current verified host:

- Heroic Wine 11.6 builtin path

Current exploratory commercial target:

- `anno-1404-gold`
  - local Heroic install
  - supported runtime: `Wine-11.6-DXMT`
  - plain `Wine-11.6` is research-only because the game currently trips Wine's
    `d3dx10_43` / `D3DX10SaveTextureToMemory` path before it becomes a usable
    baseline
  - reference-free exploratory capture for real-game bring-up

Current exploratory feature targets:

- `dxmt9-water-rt`
  - repo-local DX9 sample
  - render-to-texture + projected UV + alpha blend
  - intended as the first focused proxy for `Anno 1404` water rendering bugs
  - builtin-oracle compare now matches and can be used as a regression gate
- `dxmt9-multitexture-terrain`
  - repo-local DX9 sample
  - multi-sampler terrain material blend
  - intended as the second focused proxy for outdoor terrain/material bugs
  - builtin-oracle compare currently matches and can be used as a regression gate

Current exploratory commercial-oracle candidate:

- `street-fighter-iv-benchmark`
  - Heroic `11.6-DXMT` research lane for `dxmt9`
  - keep the Heroic prefix fresh; if the benchmark starts failing after local DLL
    or `winetricks` experiments, recreate the prefix instead of trying to
    salvage it
  - wrapper now installs prefix-native `d3dx9_41` and mirrors the 32-bit DLL next to the extracted benchmark binary
- `street-fighter-iv-benchmark-crossover-oracle`
  - CrossOver `Heroic` bottle oracle lane
  - uses builtin `d3d9` / `d3dx9_41` for commercial visual comparison
  - current automatic capture is not trustworthy yet; use this lane as a manual visual oracle host until window capture is stabilized
