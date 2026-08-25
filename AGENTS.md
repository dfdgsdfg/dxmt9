# AGENTS.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

dxmt9 is a Wine / D3D9-to-Metal translation layer for macOS. It translates
Direct3D 9 calls from apps running under Wine directly into Metal, with no
Vulkan middle layer. See `docs/build.md` for full prerequisites, build, and
install instructions (`README.md` is the user-facing overview); this file is
the orientation layer for agents.

## Read these first

This repo carries detailed, long-lived rules. Consult the relevant one before
touching that surface — they encode hard-won constraints, not suggestions:

| Rule file | When it applies |
|---|---|
| `agents/rules/codebase_conventions.rules.md` | Any implementation change — PE/unix/Metal boundary, hot-path DOD shape, span ownership, formatting. |
| `agents/rules/build.rules.md` | Driving builds — build-dir naming contract, install_name fixup gotcha, ABI lockstep; full guide in `docs/build.md`. |
| `agents/rules/environment_variables.rules.md` | Any `DXMT*` / `DXMT9*` runtime knob (master list). |
| `agents/rules/metal_debugging.rules.md` | GPU-side debugging: frame capture, `.gputrace`, signposts, perf counters, 3DMark05 GT1 probe toolkit. |
| `agents/rules/rendering_correctness.rules.md` | Stateful rendering optimizations: formal/refinement-first evidence, model-code binding, GPU oracle, and promotion order. |
| `agents/rules/test_wild.rules.md` | Running against real D3D9 binaries under Wine (runtime selection — must use Sikarugir/symbol-exposing `winemac.so`). |
| `agents/rules/documentation*.rules.md` | Writing docs/specs/rules (English-only for rules/specs; AGENTS.md vs rules vs specs decision flow). |
| `specs/gap.md`, `specs/<domain>/gap.md`, `specs/d3d9/gap_d3d9.md`, `specs/tests/gap_d3d9_wine_test.md` | Implementation status / missing evidence before assuming a feature exists. |
| `docs/perfomance/overview-3dmark05-gt1.md` | Performance investigation history (experiment knowledge graph). |

## Architecture

dxmt9 is **three runtime binaries** split across the Wine PE/unix boundary. This
split is the central design constraint and is enforced by code review:

| Binary | Kind | Built from | Role |
|---|---|---|---|
| `d3d9.dll` | PE DLL | `src/d3d9/` + `src/win32/` | User-facing D3D9 COM surface. Records D3D9 semantics into POD packets; never calls Metal. |
| `winemetal_dxmt9.dll` | PE DLL | `src/winemetal/` | PE bridge. Dispatches `dxmt9c_*` ops and shader/provider calls across `wine_unix_call` into `winemetal_dxmt9.so`. |
| `winemetal_dxmt9.so` | Wine unixlib (Mach-O) | `src/winemetal/unix/` + `src/dxmt9/` | Unix-side provider. Replays packets and owns all Metal / Objective-C++ runtime code. |

**Data flow (per draw/state):** the PE side (`src/d3d9/d3d9_pe_*.cpp`,
`d3d9_pe_recorder.hpp`) records D3D9 calls into bounded, pointer-free,
schema-stable chunk records → bridged via `winemetal_dxmt9.dll` →
`src/d3d9/device_c_chunk_replay.cpp` / `device_c_record_replay.cpp` replays them
on the unix side → `src/dxmt9/` encodes into Metal command buffers.

Key boundaries, all enforced (see `codebase_conventions.rules.md`):
- **PE-side code must not call Metal/Objective-C/macOS APIs.** It only records
  and bridges.
- **Cross-boundary records must be POD, bounds-checkable, pointer-free.**
- **Metal/ObjC++ ownership stays in `src/dxmt9/` + `src/winemetal/unix/`.**
- **Resource lifetime is sequence-ID based.** Never free/recycle Metal
  resources before the queue completion waterline passes their last use. The
  concurrent subsystems are formally verified in TLA+ (`specs/verification/`).

`src/dxmt9/` runtime internals: `dxmt9_command_queue` / `dxmt9_queue` (submission
+ completion waterline), `dxmt9_draw_encoder_draw.mm` (per-draw encode),
`dxmt9_pipeline_cache` (PSO cache + FFP/tile-FFP selection), `dxmt9_presenter.mm`
(owns `CAMetalLayer` + drawable + acquire/boundary policy),
`dxmt9_blit_encoders`, `dxmt9_capture` / `dxmt9_signposts` / `dxmt9_perf_counters`
(debug surfaces).

### Generated bridge — do not hand-edit

The PE↔unix dispatch tables are **code-generated** from
`include/dxmt9/device_c.h` (the C ABI header) and
`src/winemetal/winemetal_unix_schema.h`, via `scripts/codegen/gen_wine_bridge.py`
+ `extract_device_c_schema.py`, wired as Meson `custom_target`s in `meson.build`.
To change the bridge ABI, edit the **schema/header/generator**, not the
`*.generated.{h,cpp}` outputs under `build*/`. PE and unix sides build in
lockstep — an ABI-hash handshake (`DXMT9_WINEMETAL_CALL_ABI_HASH`) refuses to
attach if they drift.

## Build & test

Full prerequisites (`llvm-mingw`, `uv`, `WINE_ROOT`, etc.) are in
`docs/build.md`.
Toolchain is **Meson + Ninja, C++20 / C17**. The repo keeps four staging build
dirs (the runner scripts and `test_wild.rules.md` expect these exact names):

| Dir | Cross/native file | Produces |
|---|---|---|
| `build/` | native host | Native unit/spec tests — no Wine, fastest inner loop. |
| `build-x86_64-builtin/` | `cross/x86_64-macos.ini` | `winemetal_dxmt9.so` unix provider for Rosetta Wine64. |
| `build-win32-x64-builtin/` | `cross/x86_64-windows.ini` | 64-bit PE `d3d9.dll` + `winemetal_dxmt9.dll`. |
| `build-win32-x86-builtin/` | `cross/i686-windows.ini` | 32-bit WoW64 PE `d3d9.dll` + `winemetal_dxmt9.dll`. |

```sh
# Native unit/spec tests (the common inner loop):
meson compile -C build
meson test -C build

# Run a single test target by name:
meson test -C build dxmt9-core-spec

# Setup a fresh native build dir if missing:
meson setup build
```

Test target names are stable and prefixed `dxmt9-` (e.g. `dxmt9-core-spec`,
`dxmt9-shader-transform-spec`, `dxmt9-verify-tla`, `dxmt9-smoke`). Pick the
smallest relevant target for the change. For queue / resource-lifetime / present
/ encoder / query changes, run `dxmt9-verify-tla` or
`bash scripts/check/verify_tla.sh` (needs `TLA2TOOLS_JAR` set — see README).

There is **no `.clang-format` / `.clang-tidy` / `werror`** yet: do not mass-format;
match local file style, keep edits scoped to changed lines, and treat
`warning_level=2` warnings as real. `git diff --check` must pass.

## Tests layout (`tests/`)

- `tests/native/` — fast native unit/spec tests grouped by owner: `core`,
  `shader`, `backend`, `bridge`, `smoke`. Pure value transforms preferred —
  testable without Wine/Metal/GPU.
- `tests/shader_runner/` — `shader_runner_dxmt9` + manifest-driven
  `.shader_test` corpus.
- `tests/conformance/` — Wine-oracle PE D3D9 conformance (Wine tests are
  behavioral oracles only — never copy Wine/LGPL implementation code into the
  project).
- `tests/integration/` — Wine/WSI end-to-end smoke.

## Scripts (`scripts/`)

Grouped by purpose (`scripts/README.md` has the inventory): `codegen/`
(Meson-wired bridge gen), `check/` (audits run as Meson tests), `build_apps/` /
`run_apps/` / `run_suites/` (D3D9 sample + experiment + benchmark runners),
`install/` (Wine/Heroic prefix setup), `tools/` (corpus, packaging, cleanup, the
3DMark05 GT1 perf-probe toolkit). Wild-run experiment harnesses live under
`experiments/` and read `experiments/CATALOGUE.toml`.

## Local reference checkouts

Sibling read-only reference repos used during this work (paths in the prior
`AGENTS.md` note): `~/workspaces/{dxmt, d9vk, dxvk, wine, wine-build,
wine-build-wow64}`. DXMT (MIT) may be referenced with notices preserved;
DXVK/D9VK are architecture/algorithm references only; Wine D3D9 tests are
behavioral oracles, not a source of copied code (see `codebase_conventions.rules.md`
license policy).
