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
       └─ winemetal.dll (PE)        bridge: dispatches into the unix side
            └─ winemetal.so (unix)  Wine unixlib provider: encodes Metal
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

Measured 2026-08-23 on a 16 GB MacBook Air with an Apple M1 8-core GPU using
Sikarugir-CX 24.0.7 Wine:

| Workload | Current sampled FPS |
|---|---:|
| 3DMark05 GT1 | `30.9` |
| 3DMark05 GT2 | `29.0` |
| 3DMark05 GT3 | `65.9` |
| Street Fighter IV Benchmark | `44.2` |

Frame-sampled averages, not benchmark scores: one supervised HEAD run per
workload (positive-frame average over each 3DMark scene and the SFIV
catalogue's 50-second observation window). Single runs, so no run range is quoted — repeated
measurements of one build on this host drift about ±3% with ambient load and
time of day. All four runs passed with zero GPU errors.

See the [performance overview](docs/perfomance/overview.md) for methodology and
[wild FPS refresh](docs/perfomance/baselines/baselines-wild-fps-refresh.03.md)
for the measurements and limitations. [One frame end to end](docs/perfomance/frame-lifecycle.md)
shows where the frame time goes.

## Requirements

- macOS on Apple Silicon (current Wine hosts run the x86_64 lane under
  Rosetta 2).
- A Wine build whose `winemac.so` exports `_macdrv_functions` — dxmt9
  attaches its `CAMetalLayer` through that interface. This is the main
  compatibility constraint today:

| Wine runtime | Works? | Notes |
|---|---|---|
| [Sikarugir](https://github.com/Sikarugir-App) engine `sikarugir-cx-24.0.7` | Yes | The runtime dxmt9 is tested against |
| Self-built Wine with the macdrv symbol-export patch | Yes | Reproducible-from-source path; see `specs/winemetal/requirements.md` §4 |
| Heroic / Gcenx Wine builds (`Wine-11.x`, `-DXMT` variants) | No | `winemac.so` is stripped; dxmt9 cannot attach a layer |
| CrossOver (the commercial product) | No | Wrapper/runtime layout blocks unixlib staging |
| GPTK 1.1 (Wine 7.7) | No | Too old for this unixlib bridge model |

## Installation

Download `dxmt9-app-local-<version>.tar.gz` from
[Releases](https://github.com/dfdgsdfg/dxmt9/releases) and copy one PE
architecture plus the unix provider into the game's directory.

For a 64-bit game:

```sh
GAME_DIR="/path/to/game"
cp pe/x64/d3d9.dll pe/x64/winemetal.dll pe/x64/libc++.dll pe/x64/libunwind.dll "$GAME_DIR/"
cp unix/x86_64-unix/winemetal.so "$GAME_DIR/"
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

| Area | Specs |
|---|---|
| Command queue & encoding | `CommandQueue`, `EncoderLifecycle`, `EncodeSessionCompletion`, `QueueLifecycleRefinement`, `ConcurrentProgressSignals`, `CpuReadySessionProgress`, `SessionCapacityLease`, `PostEncodePayloadRetirement`, `EncodeSchedulingProgress`, `ParallelDrawBinding`, `RenderTapeParallelJoin` |
| Resource & buffer lifetime | `ResourceLifetime`, `BufferBackingVersioning` |
| Present pacing | `PresentFrameLatency`, `DrawableToken`, `PresentIdAba` |
| Query resolution | `QuerySeqId` |
| Bridge wire protocol | `WireObjectRegistry`, `ReplayScopedDrain` |
| Frame-graph DCE | `DceChunkLookahead` |

`meson test` runs TLC over every model; to run them alone:

```sh
bash scripts/check/verify_tla.sh
```

## Status

| Layer | Status |
|---|---|
| Core (D3D9 COM surface, device state, draw calls) | Complete |
| Metal backend (command queue, PSO cache, FFP shaders, D3DBC translation) | Complete |
| Formal verification (TLC, 20 specs) | Complete |
| Bridge ABI + PE forwarding (`d3d9.dll` / `winemetal.dll` / `winemetal.so`) | Complete |
| WSI (`winemac` legacy + fallback resolution, `CAMetalLayer`) | Complete |

Correctness and performance work is ongoing and documented as it lands;
see [`docs/perfomance/`](docs/perfomance/) for the measurement trail.

## Building from source

Toolchain setup, both deployment lanes, manual install layouts, and the
repository map are in [docs/build.md](docs/build.md). CI builds and
tests every push; releases are packaged by
[`.github/workflows/release.yml`](.github/workflows/release.yml).

## License

[MIT](LICENSE).

Related projects: [DXMT](https://github.com/3Shain/dxmt) (D3D11/D3D12 →
Metal, MIT), [d3d9-webgl](https://github.com/LostMyCode/d3d9-webgl)
(D3D9 reference). Wine's D3D9 tests are used as a behavioral oracle;
no Wine implementation code is included.
