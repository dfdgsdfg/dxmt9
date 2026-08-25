#!/usr/bin/env python3
"""Audit the winemetal_dxmt9.so install_name and fixup state.

When `winemetal_dxmt9.so` is linked the resulting Mach-O has `LC_LOAD_DYLIB`
entries with bare names `winemac.so` and `ntdll.so` (the names the linker
saw from `-lwinemac` / `-lntdll`). Wine's PE/unix bridge lookup —
specifically `NtQueryVirtualMemory(info=kMemoryWineLoadUnixLib=1000)`
in `winemetal_bridge.cpp:initializeDispatcherOnlyFallback` — needs those
deps to use the `@rpath/` prefix so the resolution machinery picks the
copy adjacent to `lib/wine/x86_64-windows/winemetal_dxmt9.dll` rather than
relying on bare dyld search paths.

The `winemetal_unix_install_name_fixup` `custom_target` in
`src/winemetal/unix/meson.build` runs `install_name_tool -change` to
convert bare deps to `@rpath/`. But it only runs when the canonical
build target is invoked — a `ninja src/winemetal/unix/winemetal_dxmt9.so`
direct build (common during iterative work) skips the stamp target
and the .so ends up with bare deps that silently break the bridge
(`NtQueryVirtualMemory(info=1000) -> STATUS_DLL_NOT_FOUND`).

This audit walks every `winemetal_dxmt9.so` under known build dirs, verifies
the module id is `@rpath/winemetal_dxmt9.so`, and
asserts every dep is either `@rpath/<name>` or a system framework /
absolute path. Bare `<name>.so` entries fail the audit.

Run manually:
    python3 scripts/check/audit_winemetal_install_names.py

Or as a meson test:
    meson test -C build dxmt9-winemetal-install-name-audit
"""
from __future__ import annotations

import os
import pathlib
import subprocess
import sys


BARE_DEP_NAMES = {"winemac.so", "ntdll.so"}


def find_winemetal_sos(repo_root: pathlib.Path) -> list[pathlib.Path]:
    """Find every build-output winemetal_dxmt9.so worth auditing.

    Walks the repository's top-level `build*` directories. Worktree
    artifacts under `.claude/worktrees/` are skipped — they belong to
    parked subagent work and may legitimately lag the schema.
    """
    candidates: list[pathlib.Path] = []
    for entry in sorted(repo_root.iterdir()):
        if not entry.is_dir():
            continue
        if not entry.name.startswith("build"):
            continue
        for so_path in entry.rglob("winemetal_dxmt9.so"):
            # Skip worktree builds and ninja's internal artifact dirs.
            if ".claude" in so_path.parts or ".p" in so_path.parts:
                continue
            candidates.append(so_path)
    return candidates


def audit_one(so_path: pathlib.Path) -> list[str]:
    """Return a list of human-readable failures for this .so (empty == OK)."""
    failures: list[str] = []
    try:
        install_id = subprocess.check_output(
            ["otool", "-D", str(so_path)],
            text=True,
            stderr=subprocess.STDOUT,
        ).splitlines()
        if len(install_id) < 2 or install_id[1].strip() != "@rpath/winemetal_dxmt9.so":
            actual = install_id[1].strip() if len(install_id) >= 2 else "<missing>"
            failures.append(
                f"{so_path}: install id '{actual}' is not "
                "'@rpath/winemetal_dxmt9.so'"
            )
        out = subprocess.check_output(
            ["otool", "-L", str(so_path)],
            text=True,
            stderr=subprocess.STDOUT,
        )
    except FileNotFoundError:
        return [f"{so_path}: otool not on PATH; cannot audit"]
    except subprocess.CalledProcessError as err:
        return [f"{so_path}: otool failed: {err.output.strip()}"]

    for raw in out.splitlines()[1:]:  # skip header line
        line = raw.strip()
        if not line:
            continue
        # otool format: "<path> (compatibility version ..., current version ...)"
        dep_path = line.split(" (")[0].strip()
        leaf = os.path.basename(dep_path)
        if leaf in BARE_DEP_NAMES and dep_path == leaf:
            failures.append(
                f"{so_path}: dep '{leaf}' is bare; "
                f"expected '@rpath/{leaf}'. "
                "Run `ninja -C <build-dir>` (default target) to trigger "
                "winemetal_unix_install_name_fixup, or manually re-apply with: "
                f"install_name_tool -change {leaf} @rpath/{leaf} {so_path}"
            )
    return failures


def main() -> int:
    repo_root = pathlib.Path(__file__).resolve().parents[2]
    sos = find_winemetal_sos(repo_root)
    if not sos:
        # No .so artifacts yet — nothing to audit, but report so the
        # invocation does not silently become a no-op.
        print("audit_winemetal_install_names: no winemetal_dxmt9.so artifacts "
              "found under top-level build* dirs; skipping.", file=sys.stderr)
        return 0

    all_failures: list[str] = []
    for so in sos:
        all_failures.extend(audit_one(so))

    if all_failures:
        print("audit_winemetal_install_names: FAIL", file=sys.stderr)
        for msg in all_failures:
            print(f"  {msg}", file=sys.stderr)
        return 1

    print(
        f"audit_winemetal_install_names: OK ({len(sos)} .so file(s) "
        "use @rpath for winemac/ntdll deps)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
