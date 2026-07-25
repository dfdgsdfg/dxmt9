# Public README Restructure + CI Release — Design

Date: 2026-07-25
Status: approved

## Goal

Prepare the repository for public release:

1. Rewrite `README.md` as a user-facing document — what dxmt9 is, how it
   works, measured performance, supported runtimes, how to install a
   release package, and the formal-verification story. Restrained tone:
   facts, numbers, and tables; no marketing copy.
2. Move the developer build/install detail out of the README into
   `docs/build.md`, with a thin `agents/rules/build.rules.md` pointer
   carrying only agent-relevant operational rules.
3. Add GitHub Actions: a CI build/test workflow and a tag-triggered
   release workflow that publishes the native app-local package.

## Decisions (from brainstorming)

- **Release artifact**: app-local package only
  (`dxmt9-app-local-<tag>.tar.gz` from
  `scripts/tools/package_app_local.py`). The builtin lane stays a
  build-from-source path documented in `docs/build.md`.
- **Developer docs location**: `docs/build.md` for humans;
  `agents/rules/build.rules.md` is a thin pointer plus the few
  agent-operational build rules (build-dir naming, the direct-ninja
  install_name fixup gotcha).
- **Scope**: docs restructure and both workflows in this change.
- **CI TLC**: included — `scripts/check/verify_tla.sh` self-downloads
  `tla2tools.jar` and only needs a JDK on the runner.
- **Release trigger**: tag push `v*` (plus manual `workflow_dispatch`).

## New README outline

```
# dxmt9              — one-line definition
How it works          — 3-binary architecture, direct Metal (no Vulkan
                        middle layer), factual advantages incl. TLA+
Performance           — existing measured snapshot table (kept honest:
                        frame-sampled averages, not 3DMark scores)
Requirements          — macOS/Apple Silicon; supported Wine runtimes
                        stated honestly (winemac.so must export
                        _macdrv_functions; Sikarugir-CX 24.0.7 known
                        good; Heroic/Gcenx stripped builds and CrossOver
                        product unsupported)
Installation          — download release, copy 5 files next to the game,
                        WINEDLLOVERRIDES guidance
Formal verification   — what the four TLA+ specs cover + one-line TLC run
Status                — condensed layer status table
Building from source  — link to docs/build.md
License / Credits     — related projects (DXMT etc.)
```

## CI facts verified during design

- `winemetal.so` links against `winemac.so` / `ntdll.so` from a Wine
  tree (`-Dwine_install_path`), so the release workflow must install a
  Wine runtime before building `build-x86_64-builtin`. Known-good:
  `python3 scripts/wine/install_wine.py --engine sikarugir-cx-24.0.7
  --target-id sikarugir-cx-24.0.7` → root at
  `experiments/wine/sikarugir-cx-24.0.7`.
- App-local PE DLLs (`-Dwine_builtin_dll=false`) need only llvm-mingw
  (pre-built release tarball; no Homebrew formula).
- `package_app_local.py` defaults expect `build-win32-x64`,
  `build-win32-x86`, `build-x86_64-builtin`, and `~/llvm-mingw`; all
  overridable via flags.
- `scripts/check/verify_tla.sh` uses `tlc` on PATH, else
  `$TLA2TOOLS_JAR`, else downloads the jar; needs only `java`.

## Workflows

- `.github/workflows/ci.yml` — push/PR: Homebrew meson/ninja + Temurin,
  `meson setup build && meson test -C build` on a macOS arm64 runner.
- `.github/workflows/release.yml` — tag `v*` / manual: cache and unpack
  llvm-mingw and the Sikarugir engine, build the two app-local PE lanes
  and the x86_64 unix provider, run `package_app_local.py`, tar + sha256,
  create the GitHub Release with both files attached.

## Out of scope

- Builtin-lane release artifacts.
- Changing build system behavior or deployment specs.
- Updating performance numbers (last committed snapshot stands).
