# Build — Operational Rules

The full build guide (toolchain setup, both deployment lanes, install
layouts, TLA+ verification) lives in [`docs/build.md`](../../docs/build.md).
This file carries only the operational rules an agent needs when driving
builds.

## Rule: build directory names are a contract

Scripts, launchers, and `package_app_local.py` resolve build outputs by
directory name. Use these names, not ad-hoc ones:

| Directory | Configuration |
|---|---|
| `build` | Host unit build (native arch, tests) |
| `build-x86_64-builtin` | x86_64 unix provider linked against `$WINE_ROOT` (`--native-file cross/x86_64-macos.ini -Dwine_install_path=...`) |
| `build-win32-x64-builtin` / `build-win32-x86-builtin` | Builtin-lane PE DLLs (`-Dwine_builtin_dll=true`) |
| `build-win32-x64` / `build-win32-x86` | App-local PE DLLs (`-Dwine_builtin_dll=false`) |

When a staged Wine prefix misbehaves after a code change, rebuild every
staged directory — a stale `build-win32-x86-builtin` mismatched against
`build/` has produced false bridge regressions before (see
`test_wild.rules.md` checklist item 3).

## Rule: never build `winemetal.so` with a bare `ninja` target

`ninja src/winemetal/unix/winemetal.so` skips the
`winemetal_unix_install_name_fixup` stamp, leaving bare `winemac.so` /
`ntdll.so` deps that silently break Wine's unixlib lookup
(`abi-hash unix-call failed status=0xc0000003`). Always use
`meson compile -C <builddir>`; the audit
`scripts/check/audit_winemetal_install_names.py`
(`dxmt9-winemetal-install-name-audit` meson test) catches the regression.

## Rule: PE and unix builds are an ABI lockstep pair

Any bridge/schema change requires rebuilding both PE DLL build dirs and
the unix provider together; the `DXMT9_WINEMETAL_CALL_ABI_HASH` handshake
refuses mismatched pairs at load.

## Rule: a measurement must record which binaries it ran

`result.json` carries a `staged_build` block (hash + size of the five artifacts
Wine actually loads, plus the build dirs they came from), and
`run_d3d9_conformance.py` writes a `.staged-build.json` sidecar. Do not remove
them, and when a result looks surprising, read them first.

Two failures on 2026-08-01, in one session, both of which this makes visible in
the artifact instead of requiring suspicion:

| Failure | What it produced | The tell |
|---|---|---|
| A baseline worktree took meson's **default** buildtype (`debugoptimized`, asserts live) against head's `release` | a phantom `+29.7%` A/B | staged x86 `d3d9.dll` **5.3 MB vs 938 KB** — now recorded as `bytes` |
| The D3D9 conformance suite loaded a `d3d9.dll` staged by an unrelated **3DMark** run | a suite that could not fail when the code changed | the loaded path is now named explicitly in the sidecar |

**Two things follow.**

**Build-config parity is a precondition for any A/B, not an assumption.**
`run_3dmark05_perf_probe.sh --build-root` checks only that the five directories
*exist*. Diff `meson-info/intro-buildoptions.json` between the trees until every
option matches, and prefer a file the change cannot touch (`winemetal.so` for a
D3D9-only change) as a byte-identity check. Run a same-build A/A pair first — it
validates the harness, though note it is structurally blind to a worktree
*configuration* asymmetry, which only the parity check catches.

**Builtin-lane PE DLLs are loaded from the Wine root, not from where you put
them.** `wine_builtin_dll=true` postprocesses `d3d9.dll` / `winemetal.dll` with
Wine's `"Wine builtin DLL"` signature, so Wine resolves them from
`$WINE_ROOT/lib/wine/<arch>-windows/` regardless of the path `LoadLibrary` was
given. A copy beside the executable or in the prefix's `system32` is inert.
Anything that wants to test a freshly built PE DLL must stage it into the Wine
root (`install_heroic_wine.sh`, or `stage_builtin_pe_dlls()` in
`run_d3d9_conformance.py`). Verifying the wrong copy's hash reads exactly like
success.

## CI

`.github/workflows/ci.yml` runs the host unit build + `meson test` on
push/PR. `.github/workflows/release.yml` builds and publishes the
app-local package on `v*` tags. Keep workflow build commands in sync with
`docs/build.md` when the build interface changes.
