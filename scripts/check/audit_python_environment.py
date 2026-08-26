#!/usr/bin/env python3
"""Fail when the repository's mise/uv/Meson Python contract drifts."""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
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
    mise_uv = tools.get("uv")
    pinned_python = (REPO_ROOT / ".python-version").read_text().strip()

    if "python" in tools:
        errors.append("mise must not own Python; pin it only in .python-version")
    if not re.fullmatch(r"\d+\.\d+\.\d+", pinned_python):
        errors.append(".python-version must contain an exact X.Y.Z version")

    if not isinstance(mise_uv, str) or not re.fullmatch(r"\d+\.\d+\.\d+", mise_uv):
        errors.append(".mise.toml tools.uv must be an exact X.Y.Z version")

    if re.fullmatch(r"\d+\.\d+\.\d+", pinned_python):
        major_minor = ".".join(pinned_python.split(".")[:2])
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

    uv_project = project.get("tool", {}).get("uv", {})
    if uv_project.get("python-preference") != "only-managed":
        errors.append("pyproject.toml must set tool.uv.python-preference to only-managed")

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

    actual_python = ".".join(str(part) for part in sys.version_info[:3])
    if actual_python != pinned_python:
        errors.append(
            f"uv environment Python ({actual_python}) != project pin ({pinned_python})"
        )

    mise_uv_lookup = (
        subprocess.run(
            ["mise", "which", "uv"],
            cwd=REPO_ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        if shutil.which("mise")
        else None
    )
    if mise_uv_lookup is not None and mise_uv_lookup.returncode == 0:
        uv_executable = Path(mise_uv_lookup.stdout.strip()).resolve()
    else:
        path_uv = shutil.which("uv")
        uv_executable = Path(path_uv).resolve() if path_uv else None

    expected_base_python: Path | None = None
    if uv_executable is None:
        errors.append("neither mise-managed nor PATH uv is available")
    else:
        managed_interpreter = subprocess.run(
            [str(uv_executable), "python", "find", "--managed-python", pinned_python],
            cwd=REPO_ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        if managed_interpreter.returncode != 0:
            errors.append(
                f"uv cannot resolve managed Python {pinned_python}: "
                f"{managed_interpreter.stderr.strip()}"
            )
        else:
            expected_base_python = Path(managed_interpreter.stdout.strip()).resolve()
            actual_base_python = Path(sys._base_executable).resolve()
            if actual_base_python != expected_base_python:
                errors.append(
                    "project environment does not derive from uv-managed Python: "
                    f"{actual_base_python} != {expected_base_python}"
                )

    if uv_executable is not None:
        uv_version = subprocess.run(
            [str(uv_executable), "--version"],
            cwd=REPO_ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        uv_version_fields = uv_version.stdout.split()
        actual_uv = uv_version_fields[1] if len(uv_version_fields) >= 2 else ""
    else:
        uv_version = None
        actual_uv = ""

    if uv_version is None or uv_version.returncode != 0 or not re.fullmatch(
        r"\d+\.\d+\.\d+", actual_uv
    ):
        errors.append(f"active uv version is invalid: {actual_uv or 'unresolved'}")
    elif isinstance(mise_uv, str):
        actual_uv_tuple = tuple(int(part) for part in actual_uv.split("."))
        minimum_uv_tuple = tuple(int(part) for part in mise_uv.split("."))
        if actual_uv_tuple < minimum_uv_tuple:
            errors.append(f"active uv ({actual_uv}) is older than supported ({mise_uv})")

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
        'python_request=$(<"$repo_root/.python-version")',
        "uv_args=(",
        '--project "$repo_root"',
        '--directory "$caller_pwd"',
        "--locked",
        "--managed-python",
        '--python "$python_request"',
        "mise which uv",
        'exec mise exec -C "$repo_root" -- uv',
        "command -v uv",
        'exec "$path_uv"',
    ):
        if required_fragment not in launcher_text:
            errors.append(
                f"scripts/run_python.sh is missing {required_fragment!r}"
            )

    if uv_executable is not None:
        with tempfile.TemporaryDirectory(prefix="dxmt9-uv-only-") as temp_dir:
            temp_bin = Path(temp_dir)
            (temp_bin / "uv").symlink_to(uv_executable)
            uv_only_env = os.environ.copy()
            uv_only_env["PATH"] = f"{temp_bin}:/usr/bin:/bin"
            uv_only_env.pop("VIRTUAL_ENV", None)
            uv_only = subprocess.run(
                [
                    str(launcher_path),
                    "-c",
                    (
                        "import json, sys; "
                        "print(json.dumps({'version': '.'.join(map(str, sys.version_info[:3])), "
                        "'base_executable': sys._base_executable}))"
                    ),
                ],
                cwd=REPO_ROOT,
                env=uv_only_env,
                text=True,
                capture_output=True,
                check=False,
            )
            if uv_only.returncode != 0:
                errors.append(
                    "uv-only launcher path failed without mise: "
                    f"{uv_only.stderr.strip()}"
                )
            else:
                try:
                    uv_only_identity = json.loads(uv_only.stdout)
                except json.JSONDecodeError:
                    errors.append("uv-only launcher returned invalid identity JSON")
                else:
                    if uv_only_identity.get("version") != pinned_python:
                        errors.append(
                            "uv-only launcher selected Python "
                            f"{uv_only_identity.get('version')} instead of {pinned_python}"
                        )
                    uv_only_base = Path(
                        uv_only_identity.get("base_executable", "")
                    ).resolve()
                    if expected_base_python is None:
                        errors.append(
                            "uv-only launcher identity cannot be compared because the "
                            "managed Python lookup failed"
                        )
                    elif uv_only_base != expected_base_python:
                        errors.append(
                            "uv-only launcher did not select a uv-managed Python: "
                            f"{uv_only_base} != {expected_base_python}"
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
        f"python={pinned_python} uv={actual_uv} prefix={actual_prefix}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
