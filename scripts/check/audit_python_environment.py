#!/usr/bin/env python3
"""Fail when the repository's mise/uv/Meson Python contract drifts."""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tomllib
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def read_toml(path: Path) -> dict:
    with path.open("rb") as handle:
        return tomllib.load(handle)


def main() -> int:
    errors: list[str] = []
    mise = read_toml(REPO_ROOT / ".mise.toml")
    project = read_toml(REPO_ROOT / "pyproject.toml")
    lock = read_toml(REPO_ROOT / "uv.lock")

    tools = mise.get("tools", {})
    mise_python = tools.get("python")
    mise_uv = tools.get("uv")
    pinned_python = (REPO_ROOT / ".python-version").read_text().strip()

    if not isinstance(mise_python, str) or not re.fullmatch(r"\d+\.\d+\.\d+", mise_python):
        errors.append(".mise.toml tools.python must be an exact X.Y.Z version")
    elif pinned_python != mise_python:
        errors.append(
            f".python-version ({pinned_python}) != .mise.toml Python ({mise_python})"
        )

    if not isinstance(mise_uv, str) or not re.fullmatch(r"\d+\.\d+\.\d+", mise_uv):
        errors.append(".mise.toml tools.uv must be an exact X.Y.Z version")

    if isinstance(mise_python, str):
        major_minor = ".".join(mise_python.split(".")[:2])
        expected_project_range = f">={major_minor},<{int(major_minor.split('.')[0])}.{int(major_minor.split('.')[1]) + 1}"
        actual_project_range = project.get("project", {}).get("requires-python")
        if actual_project_range != expected_project_range:
            errors.append(
                f"pyproject requires-python must be {expected_project_range!r}, got {actual_project_range!r}"
            )
        expected_lock_range = f"=={major_minor}.*"
        if lock.get("requires-python") != expected_lock_range:
            errors.append(
                f"uv.lock requires-python must be {expected_lock_range!r}, got {lock.get('requires-python')!r}"
            )

    if mise.get("env", {}).get("_", {}).get("path") is not None:
        errors.append(".mise.toml must not expose uv's project environment through PATH")

    ignored = subprocess.run(
        ["git", "check-ignore", "-q", "uv.lock"],
        cwd=REPO_ROOT,
        check=False,
    )
    if ignored.returncode == 0:
        errors.append("uv.lock is ignored; the resolved dependency graph must be committed")
    elif ignored.returncode not in (1,):
        errors.append("git check-ignore failed while auditing uv.lock")

    expected_prefix = (REPO_ROOT / ".venv").resolve()
    actual_prefix = Path(sys.prefix).resolve()
    if actual_prefix != expected_prefix:
        errors.append(
            f"audit is not running in the uv project environment: {actual_prefix} != {expected_prefix}"
        )

    if isinstance(mise_python, str):
        actual_python = ".".join(str(part) for part in sys.version_info[:3])
        if actual_python != mise_python:
            errors.append(
                f"uv environment Python ({actual_python}) != mise pin ({mise_python})"
            )

    mise_interpreter = subprocess.run(
        ["mise", "which", "python"],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    if mise_interpreter.returncode != 0:
        errors.append("mise could not resolve the pinned Python interpreter")
    else:
        expected_base_python = Path(mise_interpreter.stdout.strip()).resolve()
        actual_base_python = Path(sys._base_executable).resolve()
        if actual_base_python != expected_base_python:
            errors.append(
                "uv environment does not derive from mise Python: "
                f"{actual_base_python} != {expected_base_python}"
            )

    uv_version = subprocess.run(
        ["uv", "--version"],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    uv_version_fields = uv_version.stdout.split()
    actual_uv = uv_version_fields[1] if len(uv_version_fields) >= 2 else ""
    if uv_version.returncode != 0 or actual_uv != mise_uv:
        errors.append(f"active uv ({actual_uv or 'unresolved'}) != mise pin ({mise_uv})")

    meson_text = (REPO_ROOT / "meson.build").read_text()
    if "'scripts', 'run_python.sh'" not in meson_text:
        errors.append("Meson is not bound to the uv project launcher")
    launcher_path = REPO_ROOT / "scripts" / "run_python.sh"
    launcher_text = launcher_path.read_text()
    if not os.access(launcher_path, os.X_OK):
        errors.append("scripts/run_python.sh is not executable")
    for required_fragment in (
        '"${VIRTUAL_ENV:-}" == "$repo_root/.venv"',
        'exec "$VIRTUAL_ENV/bin/python" "$@"',
        'exec mise exec -C "$repo_root" -- uv run',
        '--project "$repo_root"',
        '--directory "$caller_pwd"',
        "--locked",
        "--python python",
    ):
        if required_fragment not in launcher_text:
            errors.append(
                f"scripts/run_python.sh is missing {required_fragment!r}"
            )
    tests_meson_text = (REPO_ROOT / "tests/meson.build").read_text()
    if "find_program('python3'" in tests_meson_text:
        errors.append("tests/meson.build shadows the root project Python binding")

    tracked_shell = subprocess.run(
        ["git", "ls-files", "*.sh"],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
        check=True,
    )
    for relative in tracked_shell.stdout.splitlines():
        shell_text = (REPO_ROOT / relative).read_text()
        if re.search(r"\bpython3\b", shell_text):
            errors.append(f"{relative} contains a bare python3 command")

    for test_path in (REPO_ROOT / "tests").rglob("*.py"):
        test_text = test_path.read_text()
        if re.search(r"[\"']python3[\"']", test_text):
            errors.append(
                f"{test_path.relative_to(REPO_ROOT)} spawns a bare python3; use sys.executable"
            )

    for workflow_name in ("ci.yml", "release.yml"):
        workflow = (REPO_ROOT / ".github/workflows" / workflow_name).read_text()
        if "jdx/mise-action@" not in workflow:
            errors.append(f"{workflow_name} does not install the mise toolchain")
        if "mise run python:sync" not in workflow:
            errors.append(f"{workflow_name} does not run the locked sync task")

    if errors:
        for error in errors:
            print(f"python environment audit: {error}", file=sys.stderr)
        return 1

    print(
        "python environment audit ok: "
        f"python={mise_python} uv={mise_uv} prefix={actual_prefix}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
