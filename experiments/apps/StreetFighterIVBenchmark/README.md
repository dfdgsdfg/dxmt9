# Street Fighter IV Benchmark

This entry is a commercial-oracle candidate for `dxmt9`.

Why this target:

- short, reproducible benchmark-style run
- D3D9Ex-era renderer with real game content
- better oracle candidate than launcher-heavy full games

Expected executable:

- default catalogue path: `/Users/dididi/games/_Heroic/Street Fighter IV Benchmark/Benchmark.exe`
- override at run time with:

```sh
bash scripts/run_sfiv_benchmark_experiment.sh --binary "/path/to/Benchmark.exe"
```

This app is intentionally reference-optional until a local install is staged and
captured under a chosen oracle host.
