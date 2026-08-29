# dxmt9

Direct3D 9 → Metal translation layer for Wine on macOS.

dxmt9 translates D3D9 API calls from Windows applications running under
Wine directly into Metal — no Vulkan or MoltenVK middle layer. It
implements `IDirect3D9`, `IDirect3D9Ex`, `IDirect3DDevice9`, and
`IDirect3DDevice9Ex`, and ships as a drop-in `d3d9.dll` you place next to
the game, in the style of DXVK.

## How it works

```
game.exe
  └─ d3d9.dll         (PE)          D3D9 COM surface; records state and draws
       └─ winemetal_dxmt9.dll (PE)        bridge: dispatches into the unix side
            └─ winemetal_dxmt9.so (unix)  Wine unixlib provider: encodes Metal
                 └─ Metal / CAMetalLayer
```

The PE side records D3D9 commands into flat, pointer-free chunks; the
unix side replays them into Metal command buffers and presents through
`CAMetalLayer` via `winemac.drv`. Design points, stated plainly:

- **Direct Metal encoding.** One translation step instead of
D3D9 → Vulkan → Metal.
- **Formally verified concurrency.** The concurrency-sensitive
subsystems — command queue, resource lifetime, encoder lifecycle,
present pacing, bridge wire protocol — are modeled in 20 TLA+ specs
checked with TLC on every test run. Debug builds assert the same
invariants at runtime.
- **Data-oriented hot paths.** Draw/state/bridge paths use flat records
and arenas, avoiding per-draw heap allocation.
- **Two deployment lanes.** App-local (copy files next to the game) and
Wine-builtin (installed into the Wine runtime); both use the same
three binaries.

## Performance

Measured on a 16 GB MacBook Air with an Apple M1 8-core GPU. The dxmt9,
Sikarugir D9VK/MoltenVK, and both WineD3D SFIV columns were refreshed
2026-08-29; the remaining WineD3D rows retain the 2026-08-25/26 Sikarugir and
Heroic Wine Staging 11.16 baselines:


| Workload                    | dxmt9 / Metal (Sikarugir) | D9VK / MoltenVK (Sikarugir) | WineD3D / OpenGL (Sikarugir) | WineD3D / OpenGL (Heroic 11.16) |
| --------------------------- | -------------------------: | ---------------------------: | ----------------------------: | -------------------------------: |
| 3DMark05 GT1                | `31.2`                     | `28.8`                       | `31.4`                        | `27.7`                           |
| 3DMark05 GT2                | `30.1`                     | `27.0`                       | `30.7`                        | `21.7`                           |
| 3DMark05 GT3                | `66.5`                     | `46.6`                       | `61.0`                        | `57.4`                           |
| 3DMark06 GT1                | `15.7`                     | `16.1`                       | `16.0`                        | `14.9`                           |
| 3DMark06 GT2                | `18.1`                     | `19.2`                       | `18.9`                        | `16.5`                           |
| 3DMark06 HDR1               | `37.0`                     | `46.5`                       | `37.6`                        | `29.7`                           |
| 3DMark06 HDR2               | `16.0`                     | `17.1`                       | `15.6`                        | `13.6`                           |
| Street Fighter IV Benchmark | `44.3`                     | `46.8`                       | `43.8`                        | `63.3`                           |

dxmt9 uses one `perf` run per workload: 1024x768 for 3DMark05 and 1280x720 for
3DMark06 and SFIV. The WineD3D runs use 1024x768 for 3DMark05 and 1280x800 for
3DMark06. D9VK/MoltenVK uses 1024x768 for 3DMark05 and 1280x800 for 3DMark06
and SFIV. Sikarugir WineD3D SFIV uses 1280x720; Heroic 11.16 WineD3D SFIV uses
1280x800. All three non-dxmt9 SFIV reference values are 240-second
benchmark-overlay averages.

See the [performance overview](docs/perfomance/overview.md) for methodology and
[wild FPS refresh](docs/perfomance/baselines/baselines-wild-fps-refresh.04.md)
and [3DMark06 WineD3D baseline](docs/perfomance/baselines/baselines-3dmark06-wined3d.05.md)
and [Sikarugir D9VK/MoltenVK baseline](docs/perfomance/baselines/baselines-d9vk-moltenvk.06.md)
and [SFIV WineD3D baseline](docs/perfomance/baselines/baselines-sfiv-wined3d.07.md)
for the measurements and limitations. [One frame end to end](docs/perfomance/frame-lifecycle.md)
shows where the frame time goes.

## Requirements

- macOS on Apple Silicon (current Wine hosts run the x86_64 lane under
Rosetta 2).
- A Wine build whose `winemac.so` exports `_macdrv_functions` — dxmt9
attaches its `CAMetalLayer` through that interface. This is the main
compatibility constraint today:


| Wine runtime                                                               | Works? | Notes                                                                                                                                                                                                  |
| -------------------------------------------------------------------------- | ------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| [Sikarugir](https://github.com/Sikarugir-App) engine `sikarugir-cx-24.0.7` | Yes    | The runtime dxmt9 is tested against                                                                                                                                                                    |
| Self-built Wine with the macdrv symbol-export patch                        | Yes    | Reproducible-from-source path; see `specs/winemetal/requirements.md` §4                                                                                                                                |
| [3Shain/wine](https://github.com/3Shain/wine) fork release `v9.9-mingw`    | Yes    | The upstream-DXMT author's Wine; also serves DXMT's D3D10/11 DLLs, so one runtime covers D3D9–11. No bundled Wine Mono/Gecko: bootstrap fresh prefixes once with `WINEDLLOVERRIDES="mscoree=;mshtml="` |
| Heroic / Gcenx Wine builds (`Wine-11.x`, `-DXMT` variants)                 | No     | `winemac.so` is stripped; dxmt9 cannot attach a layer                                                                                                                                                  |
| CrossOver (the commercial product)                                         | No     | Wrapper/runtime layout blocks unixlib staging                                                                                                                                                          |
| GPTK 1.1 (Wine 7.7)                                                        | No     | Too old for this unixlib bridge model                                                                                                                                                                  |


## Installation

Download `dxmt9-app-local-<version>.tar.gz` from
[Releases](https://github.com/dfdgsdfg/dxmt9/releases) and copy one PE
architecture plus the unix provider into the game's directory.

For a 64-bit game:

```sh
GAME_DIR="/path/to/game"
cp pe/x64/d3d9.dll pe/x64/winemetal_dxmt9.dll pe/x64/libc++.dll pe/x64/libunwind.dll "$GAME_DIR/"
cp unix/x86_64-unix/winemetal_dxmt9.so "$GAME_DIR/"
```

For a 32-bit game, use `pe/x86/` instead (the unix provider is the same).

Then run the game through Wine as usual. Wine's default DLL search loads
the `d3d9.dll` next to the executable; if your launcher forces an
override, use native-first:

```sh
WINEDLLOVERRIDES="d3d9=n,b"
```

Do not use `d3d9=b` — it bypasses the app-local DLLs entirely.

To install dxmt9 into the Wine runtime itself (the builtin lane), or to
build from source, see [docs/build.md](docs/build.md).

## Formal verification

The concurrency-sensitive parts of the backend are specified in 20 TLA+
modules under [`specs/verification/tla/`](specs/verification/tla/):


| Area                           | Specs                                                                                                                                                                                                                                                                                 |
| ------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Command queue &amp; encoding   | `CommandQueue`, `EncoderLifecycle`, `EncodeSessionCompletion`, `QueueLifecycleRefinement`, `ConcurrentProgressSignals`, `CpuReadySessionProgress`, `SessionCapacityLease`, `PostEncodePayloadRetirement`, `EncodeSchedulingProgress`, `ParallelDrawBinding`, `RenderTapeParallelJoin` |
| Resource &amp; buffer lifetime | `ResourceLifetime`, `BufferBackingVersioning`                                                                                                                                                                                                                                         |
| Present pacing                 | `PresentFrameLatency`, `DrawableToken`, `PresentIdAba`                                                                                                                                                                                                                                |
| Query resolution               | `QuerySeqId`                                                                                                                                                                                                                                                                          |
| Bridge wire protocol           | `WireObjectRegistry`, `ReplayScopedDrain`                                                                                                                                                                                                                                             |
| Frame-graph DCE                | `DceChunkLookahead`                                                                                                                                                                                                                                                                   |


`meson test` runs TLC over every model; to run them alone:

```sh
bash scripts/check/verify_tla.sh
```

## Status


| Layer                                                                                  | Status   |
| -------------------------------------------------------------------------------------- | -------- |
| Core (D3D9 COM surface, device state, draw calls)                                      | Complete |
| Metal backend (command queue, PSO cache, FFP shaders, D3DBC translation)               | Complete |
| Bridge ABI + PE forwarding (`d3d9.dll` / `winemetal_dxmt9.dll` / `winemetal_dxmt9.so`) | Complete |
| WSI (`winemac` legacy + fallback resolution, `CAMetalLayer`)                           | Complete |


Correctness and performance work is ongoing and documented as it lands;
see [`docs/perfomance/`](docs/perfomance/) for the measurement trail.

## Building from source

Toolchain setup, both deployment lanes, manual install layouts, and the
repository map are in [docs/build.md](docs/build.md). CI builds and
tests every push; releases are packaged by
[`.github/workflows/release.yml`](.github/workflows/release.yml).

## License

[MIT](LICENSE)
