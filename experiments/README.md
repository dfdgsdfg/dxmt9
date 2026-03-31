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
```

One-shot wrappers for the verified real apps:

```sh
bash scripts/run_basic_experiment.sh --wine-root "$WINE_ROOT"
bash scripts/run_tutorial07_experiment.sh --wine-root "$WINE_ROOT"
bash scripts/run_hdrformats_experiment.sh --wine-root "$WINE_ROOT"
bash scripts/run_simple_sample_experiment.sh --wine-root "$WINE_ROOT"
bash scripts/run_irrlicht_managed_lights_experiment.sh --wine-root "$WINE_ROOT"
```

Permanent-prefix installer for Heroic:

```sh
bash scripts/install_heroic_experiment_prefix.sh --prefix "$HOME/.wine-dxmt9-heroic" --wine-root "$WINE_ROOT"
```

The runner:

- reads [`CATALOGUE.toml`](./CATALOGUE.toml)
- stages `d3d9.dll`, `dxmt9.dll`, and `dxmt9.so` into a Wine runtime/prefix
- runs the selected launcher
- captures the presented back buffer directly from dxmt9 when `capture_frame` is set
- falls back to window capture only when an internal frame dump is unavailable
- writes `actual.png`, `diff.png`, `ssim.txt`, `dxmt9.log`, and `result.json`

Current verified bootstrap entry:

- `dxmt9-wsi-present-local`

This is not a catalogue target from the experiments spec. It exists to validate
the launcher, frame-dump capture, and SSIM workflow locally before external
sample binaries are staged.

Current verified real application entry:

- `dx-sdk-basichlsl`
  - Heroic Wine 11.5 builtin path
  - 240 frames
  - direct backbuffer capture
  - `ssim = 1.0000`
- `dx-sdk-tutorial07`
  - Heroic Wine 11.5 builtin path
  - 180 frames
  - direct backbuffer capture
  - `ssim = 1.0000`
- `dxut-simple-sample`
  - Heroic Wine 11.5 builtin path
  - 180 frames
  - direct backbuffer capture
  - `ssim = 1.0000`
- `dx-sdk-hdrformats`
  - Heroic Wine 11.5 builtin path
  - 180 frames
  - direct backbuffer capture
  - `ssim = 1.0000`
- `irrlicht-managed-lights`
  - Heroic Wine 11.5 builtin path
  - 180 frames
  - direct backbuffer capture
  - `ssim = 1.0000`

Current verified host:

- Heroic Wine 11.5 builtin path
