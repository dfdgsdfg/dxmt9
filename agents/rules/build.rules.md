# Build — Operational Rules

The full build guide (toolchain setup, both deployment lanes, install
layouts, TLA+ verification) lives in [`docs/build.md`](../../docs/build.md).
This file carries only the operational rules an agent needs when driving
builds.

## Rule: Python is the uv-owned project environment

`.python-version` owns the exact Python request, `pyproject.toml` requires an
uv-managed interpreter, and the committed `uv.lock` owns dependencies.
`.mise.toml` pins uv only; mise is an optional task/environment manager, not a
Python owner. Drive repository Python commands through `scripts/run_python.sh`;
it prefers an installed mise uv and falls back to a compatible PATH uv. Do not
assume a caller's `python3` resolves to the project environment. Meson binds
only that launcher, and uv lazily synchronizes the locked environment. It is an
internal cache: do not activate it, add it to PATH, or repair it with
`/usr/bin/python3`, Homebrew Python, Conda, or an agent-specific interpreter.
After changing tool or dependency metadata, update the lock deliberately and
run `mise run python:check` when mise is available, or run the audit through
`scripts/run_python.sh` otherwise.

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

## Rule: never build `winemetal_dxmt9.so` with a bare `ninja` target

`ninja src/winemetal/unix/winemetal_dxmt9.so` skips the
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
option matches, and prefer a file the change cannot touch (`winemetal_dxmt9.so` for a
D3D9-only change) as a byte-identity check. Run a same-build A/A pair first — it
validates the harness, though note it is structurally blind to a worktree
*configuration* asymmetry, which only the parity check catches.

**The Wine root is shared mutable state, so staging is a side effect on every
later run.** `stage_builtin_pe_dlls` copies `d3d9.dll` / `winemetal_dxmt9.dll` from
`--exe`'s directory into `$WINE_ROOT/lib/wine/x86_64-windows/`, and they stay
there. A conformance bisect that builds old commits in throwaway worktrees
therefore leaves the *last point measured* staged in the Wine root — verified
2026-08-22, where the root held the midpoint build after the run finished. The
next conformance or wild run then silently measures that binary. **Restage HEAD
(any default-args runner invocation does it) before trusting a later run, and
delete bisect worktrees when done** so a stale `--exe` cannot be pointed at
them. The `.staged-build.json` sidecar is what makes this checkable: compare
its hash against the tree you meant to test.

**Builtin-lane PE DLLs are loaded from the Wine root, not from where you put
them.** `wine_builtin_dll=true` postprocesses `d3d9.dll` / `winemetal_dxmt9.dll` with
Wine's `"Wine builtin DLL"` signature, so Wine resolves them from
`$WINE_ROOT/lib/wine/<arch>-windows/` regardless of the path `LoadLibrary` was
given. A copy beside the executable or in the prefix's `system32` is inert.
Anything that wants to test a freshly built PE DLL must stage it into the Wine
root (`install_heroic_wine.sh`, or `stage_builtin_pe_dlls()` in
`run_d3d9_conformance.py`). Verifying the wrong copy's hash reads exactly like
success.

## Rule: a worktree is not an isolation boundary for repo-global state

Parallel work happens in `git worktree` checkouts, which isolate the *working
tree* and nothing else. Several things an agent reaches for are shared across
every worktree and every concurrent session, and each has already caused a real
incident here:

| Shared thing | Incident |
|---|---|
| **The stash list** | Twice on 2026-08-22. `git stash` on an already-clean tree stashes nothing and returns 0; the following `git stash pop` then applies the repo's *oldest* stash — in both cases a July WIP entry from unrelated work — onto current master, with conflicts. The pop conflicting is what saved the entry; a clean pop would have silently dropped it. |
| **The Wine root** | A conformance bisect left the last-measured commit's `d3d9.dll` staged in `$WINE_ROOT`, so the next run silently measured it (see the staging rule above). |
| **`tmp/` outputs** | `run_d3d9_conformance.py --output` is one fixed path every run overwrites, which is why runs now archive to `experiments/output/conformance-<UTC>/`. |

**Rules:**

- Do not use `git stash` in this repository. To compare against another commit,
  use a detached checkout or a second worktree. If a pop conflicts
  unexpectedly, `git reset --hard HEAD` clears the failed application without
  losing the entry — then check `git stash list` before doing anything else.
- Never assume a bare `stash@{0}` is yours. Multiple sessions and seven
  long-lived entries share that stack.
- Before trusting any measurement, confirm which artifacts it actually loaded
  (`.staged-build.json`, `result.json:staged_build`) rather than which ones you
  built.

## CI

`.github/workflows/ci.yml` runs the host unit build + `meson test` on
push/PR. `.github/workflows/release.yml` builds and publishes the
app-local package on `v*` tags. Keep workflow build commands in sync with
`docs/build.md` when the build interface changes.
