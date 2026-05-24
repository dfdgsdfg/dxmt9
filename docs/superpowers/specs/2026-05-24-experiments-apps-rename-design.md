# experiments/apps Test Naming Normalization — Design

- Date: 2026-05-24
- Status: approved for planning
- Base commit: `fa3157e` (branched from `agent/gap-d3d9-impl`)
- Isolated worktree: `/Users/dididi/workspaces/dxmt9-apps-rename` on branch `rename/experiments-apps-taxonomy`

## Problem

Catalogue test ids and their backing directories grew organically into six
inconsistent prefix conventions (`dxmt9-`, `d9vk-`, `dx-sdk-`, `dxut-`,
`irrlicht-`, and none), with ad-hoc id↔directory transforms
(`D9VKD3D9FixedFunctionQuirks`→`d9vk-d3d9-ffp-quirks`,
`D3D9IntentProbe`→`dxmt9-wsi-present-local`), PascalCase directories with one
lowercase exception (`irrlicht`), and entries with no separators (`3dmark05`).
The goal is a single predictable taxonomy across ids, directories, exe outputs,
and source file names.

## Naming Rule

```
<category>-<api>-<topic>
  category ∈ { conf, perf, sample, app }
  api      = d3d9   (every current entry; segment kept for future d3d11/d3d12)
  topic    = kebab-case; split compound words (basichlsl → basic-hlsl);
             drop redundant classifier suffixes (-policy, -local)
```

- **category meaning**: `conf` = behavior/conformance probe, `perf` =
  performance probe, `sample` = SDK sample or project demo app, `app` =
  commercial binary.
- **provenance** (d9vk / dxsdk / project): removed from the name; it already
  lives in the catalogue `scope=` field.
- **directories** (`experiments/apps/`): kebab stem matching the id when the
  mapping is 1:1. The two multi-probe binaries keep a binary-unit name because
  one directory backs many ids:
  - `D3D9IntentProbe` → `conf-d3d9-intent-probe` (backs 7 conf ids)
  - `PerformanceProbe` → `perf-d3d9-probe` (backs 7 perf ids)
- **exe / source**: follow the directory stem. Two classes differ:
  - **Built-from-source apps** (those with a `build_script` in the catalogue —
    the d9vk conf probes, the intent probe, the perf probes, and any project
    app compiled here): rename both source and exe, and update the build
    scripts / meson targets. exe keeps its arch suffix (`conf-d3d9-clear-x64.exe`,
    `-x86.exe`); C++ source uses snake_case of the stem (`conf_d3d9_clear.cpp`).
  - **Vendored prebuilt binaries** (SDK samples like `SimpleSample.exe`,
    `20.ManagedLights.exe`, and the commercial apps): no source to rename and no
    build step. Rename the `.exe` file on disk to the new stem and update the
    `binary` path in the catalogue only.
  The implementation plan enumerates which app falls in which class.

## Full id Mapping (35 entries)

### conf (13)

| old id | new id |
|---|---|
| d9vk-d3d9-clear | conf-d3d9-clear |
| d9vk-d3d9-buffer | conf-d3d9-buffer |
| d9vk-d3d9-triangle | conf-d3d9-triangle |
| d9vk-d3d9-lock-matrix | conf-d3d9-lock-matrix |
| d9vk-d3d9-ffp-quirks | conf-d3d9-ffp-quirks |
| dxmt9-d3d9-srgbtexture | conf-d3d9-srgb-texture |
| dxmt9-d3d9-float-texture | conf-d3d9-float-texture |
| dxmt9-d3d9-stream-source | conf-d3d9-stream-source |
| dxmt9-d3d9-shademode-provoking | conf-d3d9-shademode-provoking |
| dxmt9-d3d9-pointsize-policy | conf-d3d9-pointsize |
| dxmt9-d3d9-yuv-format-policy | conf-d3d9-yuv-format |
| dxmt9-d3d9-vendor-format-policy | conf-d3d9-vendor-format |
| dxmt9-wsi-present-local | conf-d3d9-wsi-present |

### perf (11)

| old id | new id |
|---|---|
| dxmt9-perf-present-only | perf-d3d9-present-only |
| dxmt9-perf-offscreen-heavy | perf-d3d9-offscreen-heavy |
| dxmt9-perf-many-draw | perf-d3d9-many-draw |
| dxmt9-perf-ffp-only | perf-d3d9-ffp-only |
| dxmt9-perf-multi-rt | perf-d3d9-multi-rt |
| dxmt9-perf-depth-heavy | perf-d3d9-depth-heavy |
| dxmt9-perf-skeletal | perf-d3d9-skeletal |
| dxmt9-perf-bridge-empty | perf-d3d9-bridge-empty |
| dxmt9-perf-encode-replay | perf-d3d9-encode-replay |
| dxmt9-perf-present-loop | perf-d3d9-present-loop |
| dxmt9-perf-chain-parametric | perf-d3d9-chain-parametric |

### sample (7)

| old id | new id |
|---|---|
| dx-sdk-basichlsl | sample-d3d9-basic-hlsl |
| dx-sdk-tutorial07 | sample-d3d9-tutorial07 |
| dx-sdk-hdrformats | sample-d3d9-hdr-formats |
| dxut-simple-sample | sample-d3d9-dxut-simple |
| irrlicht-managed-lights | sample-d3d9-irrlicht-lights |
| dxmt9-water-rt | sample-d3d9-water-rt |
| dxmt9-multitexture-terrain | sample-d3d9-multitexture-terrain |

### app (4)

| old id | new id |
|---|---|
| anno-1404-gold | app-d3d9-anno-1404 |
| 3dmark05 | app-d3d9-3dmark05 |
| street-fighter-iv-benchmark | app-d3d9-sfiv-benchmark |
| street-fighter-iv-benchmark-crossover-oracle | app-d3d9-sfiv-benchmark-crossover-oracle |

## Directory Mapping (`experiments/apps/`)

| old dir | new dir | ids backed |
|---|---|---|
| D9VKD3D9Clear | conf-d3d9-clear | 1 |
| D9VKD3D9Buffer | conf-d3d9-buffer | 1 |
| D9VKD3D9Triangle | conf-d3d9-triangle | 1 |
| D9VKD3D9LockMatrix | conf-d3d9-lock-matrix | 1 |
| D9VKD3D9FixedFunctionQuirks | conf-d3d9-ffp-quirks | 1 |
| D3D9IntentProbe | conf-d3d9-intent-probe | 7 |
| BasicHLSL | sample-d3d9-basic-hlsl | 1 |
| Tutorial07 | sample-d3d9-tutorial07 | 1 |
| HDRFormats | sample-d3d9-hdr-formats | 1 |
| DXUTSimpleSample | sample-d3d9-dxut-simple | 1 |
| irrlicht | sample-d3d9-irrlicht-lights | 1 |
| WaterRT | sample-d3d9-water-rt | 1 |
| MultiTextureTerrain | sample-d3d9-multitexture-terrain | 1 |
| PerformanceProbe | perf-d3d9-probe | 7 |
| BridgeEmptyProbe | perf-d3d9-bridge-empty | 1 |
| EncodeReplayProbe | perf-d3d9-encode-replay | 1 |
| PresentLoopProbe | perf-d3d9-present-loop | 1 |
| ChainParametricProbe | perf-d3d9-chain-parametric | 1 |

`conf-d3d9-wsi-present` has no `experiments/apps/` directory (it builds from
`build/wsi_present/`); only its id changes.

The exe and source files inside each renamed directory are renamed to the new
stem (e.g. `D9VKD3D9Clear/d3d9_clear.cpp` → `conf-d3d9-clear/conf_d3d9_clear.cpp`,
exe `d3d9-clear-x64.exe` → `conf-d3d9-clear-x64.exe`). Exact per-file exe/source
names are enumerated in the implementation plan.

## Reference-Update Surface (~60 files)

1. `experiments/CATALOGUE.toml` — source of truth: `name`, `binary`,
   `build_script`, `app_dir` fields.
2. Directory + exe + source renames via `git mv` (18 directories).
3. Runner / suite scripts (~10): `scripts/run_apps/*`, `scripts/run_suites/*`
   — hardcoded ids, app lists, and binary path builders
   (e.g. `binary_for_app()` in `run_dx9_fast_sanity_suite.sh`).
4. `scripts/tools/*.py` (3): `gen_wine_d3d9_test_inventory.py`,
   `run_d3d9_conformance.py`, `run_dx9_present_policy_ab.py`.
5. `tests/`: `conformance/d3d9/MANIFEST.toml`, `HARNESS_MANIFEST.toml`,
   `meson.build`, `tests/conformance/d3d9/d3d9_conformance_fixtures.h`,
   `tests/conformance/d3d9/meson.build`, `tests/native/backend/*` where they
   reference catalogue ids.
6. `specs/` + `docs/` (~15): prose references.
7. `experiments/launchers/{3dmark05,street-fighter-iv-benchmark}.sh` — rename
   the script files to the new ids as well.

### Critical caveat — src/ matches are mostly false positives

`rg` flags 5 `src/` files (`dxmt9_perf_counters.{cpp,hpp}`,
`dxmt9_heap_manager.cpp`, `dxmt9_resource_pool.cpp`, `dxmt9_shader_sources.hpp`)
on the `dxmt9-perf-` substring. These are GPU **perf-counter subsystem** names,
**not** catalogue test ids. A blanket find-replace would corrupt the counter
system. Each `src/` hit must be inspected manually and changed only if it is a
genuine catalogue-id reference (expected: none).

## Execution & Verification

- All edits happen in the isolated worktree (`rename/experiments-apps-taxonomy`,
  based at `fa3157e`) to avoid colliding with the autonomous multi-agent system
  actively committing to `agent/gap-d3d9-impl`.
- Order: CATALOGUE.toml → `git mv` directories/exe/source → scripts → tools →
  tests/meson → specs/docs → launcher script renames → manual src/ inspection.
- Verification gate:
  1. `meson` reconfigure + `ninja` build of affected build dirs
     (`build-win32-x64-builtin`, `build-win32-x86-builtin`,
     `build-x86_64-builtin`) — catches broken exe/source references in meson.
  2. Re-run `scripts/run_suites/run_dx9_fast_sanity_suite.sh` — confirms the
     renamed ids and directories work end-to-end (expected: same 9 pass /
     3 lock-matrix fail baseline as pre-rename).
  3. `rg` sweep confirming zero stale old-id / old-dir references remain
     (excluding `experiments/output/**` artifacts).
- Merge back to a mainline branch is deferred to the user, given the live churn
  on `agent/gap-d3d9-impl`. The worktree path and branch name are returned for
  the user to merge when ready.

## Out of Scope (YAGNI)

- `experiments/output/<id>` artifacts are regenerable; not renamed. Old
  directories are orphaned and re-created under new ids on the next run; a
  separate cleanup can drop the orphans.
- `experiments/prefixs/` and `experiments/apps_3rd/` caches are not renamed.
- No restructuring of the catalogue schema, runner architecture, or build
  layout beyond what the rename requires.
- No new abstraction or backward-compatibility alias for old ids — the rename
  is a clean break.
