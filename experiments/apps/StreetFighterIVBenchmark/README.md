# Street Fighter IV Benchmark

This entry is a commercial-oracle candidate for `dxmt9`.

Why this target:

- short, reproducible benchmark-style run
- D3D9Ex-era renderer with real game content
- better oracle candidate than launcher-heavy full games

Expected executable:

- default catalogue path: `/Users/dididi/games/_Heroic/Street Fighter IV Benchmark/Benchmark.exe`
- public benchmark archives may instead unpack `StreetFighterIV_Benchmark.exe`
- runner autodiscovery also checks:
  - `~/games/Street Fighter IV Benchmark/Benchmark.exe`
  - `~/games/Street Fighter IV Benchmark/StreetFighterIV_Benchmark.exe`
  - `~/Games/Street Fighter IV Benchmark/Benchmark.exe`
  - `~/Games/Street Fighter IV Benchmark/StreetFighterIV_Benchmark.exe`
  - `~/Applications/Street Fighter IV Benchmark/Benchmark.exe`
  - `~/Applications/Street Fighter IV Benchmark/StreetFighterIV_Benchmark.exe`
  - `~/Downloads/Street Fighter IV Benchmark/Benchmark.exe`
  - `~/Downloads/Street Fighter IV Benchmark/StreetFighterIV_Benchmark.exe`
- override at run time with:
  - extracted benchmark binary:

```sh
bash scripts/run_apps/run_sfiv_benchmark_experiment.sh --binary "/path/to/Benchmark.exe"
```

  - or the original installer package:

```sh
bash scripts/run_apps/run_sfiv_benchmark_experiment.sh --binary "~/Downloads/StreetFighterIV_Benchmark.exe"
```

Installer handling:

- if `--binary` points at the public installer package `StreetFighterIV_Benchmark.exe`,
  the wrapper now extracts the embedded MSI to `~/.cache/dxmt9/sfiv-benchmark/`
  and runs the real benchmark binary from there
- override the cache location with:

```sh
bash scripts/run_apps/run_sfiv_benchmark_experiment.sh \
  --binary "~/Downloads/StreetFighterIV_Benchmark.exe" \
  --cache-root "/tmp/sfiv-cache"
```

- this path requires `msiextract` from `msitools`
  - install with `brew install msitools`

This app is intentionally reference-optional until a local install is staged and
captured under a chosen oracle host.

Host lanes:

- Heroic `dxmt9` lane:

```sh
bash scripts/run_apps/run_sfiv_benchmark_experiment.sh \
  --host heroic \
  --binary "~/Downloads/StreetFighterIV_Benchmark.exe"
```

  Notes:
  - use a fresh prefix for reliable results
  - if an older prefix has been touched by prior `winetricks`, debug envs, or
    manual DLL experiments, delete it and recreate it with:

```sh
rm -rf "$HOME/Games/_Prefixes/Street Fighter IV Benchmark"
export WINEPREFIX="$HOME/Games/_Prefixes/Street Fighter IV Benchmark"
"$HOME/Library/Application Support/heroic/tools/wine/Wine-11.6-DXMT/Contents/Resources/wine/bin/wineboot" -u
```

  - the direct manual launch baseline is:

```sh
cd "/Users/dididi/games/_Windows/STREETFIGHTERIV_BENCHMARK" && \
export WINEPREFIX="$HOME/Games/_Prefixes/Street Fighter IV Benchmark" && \
export WINEDLLOVERRIDES="d3d9=n,b;ddraw=n,b" && \
"$HOME/Library/Application Support/heroic/tools/wine/Wine-11.6-DXMT/Contents/Resources/wine/bin/wine" \
start /unix ./StreetFighterIV_Benchmark.exe
```

  - the wrapper extracts the public installer when needed
  - it ensures native `d3dx9_41` is installed into the Heroic prefix with `winetricks`
  - it then copies the 32-bit `d3dx9_41.dll` next to the extracted benchmark EXE
    so Heroic `11.6-DXMT` can get past the D3DX9 loader blocker

- CrossOver oracle lane:

```sh
bash scripts/run_apps/run_sfiv_benchmark_experiment.sh \
  --host crossover \
  --binary "~/Downloads/StreetFighterIV_Benchmark.exe"
```

  Notes:
  - this uses the CrossOver `Heroic` bottle as a visual oracle host
  - the CrossOver lane skips `dxmt9` staging and forces builtin `d3d9` / `d3dx9_41`
  - current automatic capture still falls back to a generic full-screen shot on this machine,
    so treat CrossOver as a manual oracle host until SFIV window capture is stabilized
