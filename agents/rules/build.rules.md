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

## CI

`.github/workflows/ci.yml` runs the host unit build + `meson test` on
push/PR. `.github/workflows/release.yml` builds and publishes the
app-local package on `v*` tags. Keep workflow build commands in sync with
`docs/build.md` when the build interface changes.
