#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import tomllib


REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from scripts.check import check_debug_result_schema as debug_result_schema  # noqa: E402

DEFAULT_MANIFEST = REPO_ROOT / "tests" / "module_boundary" / "MANIFEST.toml"
DEFAULT_UNIX_BUILD_DIR = REPO_ROOT / "build-x86_64-builtin"
RESULT_SCHEMA = "dxmt9.module_boundary.result.v1"
PHASE_EVENT_RE = re.compile(r"^\[dxmt9-module-boundary\] phase=(?P<phase>\S+) event=(?P<event>\S+)$")
VALID_FAILURE_CATEGORIES = {
    "none",
    "artifact-staging",
    "pe-loader-export",
    "bridge-abi-mismatch",
    "unix-module-load",
    "provider-entry-dispatch",
    "public-d3d9-smoke",
    "command-submission",
    "unsupported-runtime",
}
ROLE_STAGE_PATH = {
    "probe": Path("module_boundary_probe.exe"),
    "provider-probe": Path("tests") / "native" / "backend" / "dxmt9-unix-chunk-injection-probe",
    "libdxmt9_native.dylib": Path("src") / "libdxmt9_native.dylib",
    "provider-winemetal.so": Path("src") / "winemetal" / "unix" / "winemetal.so",
    "d3d9.dll": Path("d3d9.dll"),
    "winemetal.dll": Path("winemetal.dll"),
    "winemetal.so": Path("unix") / "winemetal.so",
    "ntdll.so": Path("unix") / "ntdll.so",
    "winemac.so": Path("unix") / "winemac.so",
    "libc++.dll": Path("libc++.dll"),
    "libunwind.dll": Path("libunwind.dll"),
    "builtin-d3d9.dll": Path("builtin") / "d3d9.dll",
    "builtin-winemetal.dll": Path("builtin") / "winemetal.dll",
    "builtin-winemetal.so": Path("builtin") / "winemetal.so",
}


@dataclass(frozen=True)
class Artifact:
    role: str
    path: Path


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def artifact_record(role: str, path: Path, *, relative_to: Path | None = None) -> dict[str, Any]:
    display_path = path
    if relative_to is not None:
        try:
            display_path = path.relative_to(relative_to)
        except ValueError:
            display_path = path
    return {
        "role": role,
        "path": display_path.as_posix(),
        "sha256": sha256_file(path),
        "bytes": path.stat().st_size,
    }


def parse_artifact(value: str) -> Artifact:
    if "=" not in value:
        raise argparse.ArgumentTypeError("artifact must use role=/path/to/file")
    role, path_text = value.split("=", 1)
    if not role or not path_text:
        raise argparse.ArgumentTypeError("artifact role and path must be non-empty")
    return Artifact(role=role, path=Path(path_text).expanduser().resolve())


def default_pe_build_dir(arch: str, lane: str) -> Path:
    if lane == "builtin":
        if arch == "x86":
            return REPO_ROOT / "build-win32-x86-builtin"
        return REPO_ROOT / "build-win32-x64-builtin"
    if arch == "x86":
        return REPO_ROOT / "build-win32-x86"
    return REPO_ROOT / "build-win32-x64"


def wine_candidates() -> list[Path]:
    values: list[Path] = []
    for key in ("WINE", "DXMT_WINE", "DXMT9_WINE"):
        value = os.environ.get(key)
        if value:
            values.append(Path(value).expanduser())
    values.extend(
        [
            Path.home()
            / "Library"
            / "Application Support"
            / "heroic"
            / "tools"
            / "wine"
            / "Wine-11.7-DXMT"
            / "Contents"
            / "MacOS"
            / "wine",
            Path.home()
            / "Library"
            / "Application Support"
            / "heroic"
            / "tools"
            / "wine"
            / "Wine-11.7"
            / "Contents"
            / "MacOS"
            / "wine",
        ]
    )
    for name in ("wine64", "wine"):
        found = shutil.which(name)
        if found:
            values.append(Path(found))
    seen: set[Path] = set()
    unique: list[Path] = []
    for value in values:
        path = value.resolve()
        if path in seen:
            continue
        seen.add(path)
        unique.append(path)
    return unique


def resolve_wine(args: argparse.Namespace) -> Path | None:
    if args.wine:
        return args.wine.expanduser().resolve()
    for candidate in wine_candidates():
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def wine_lib_dir_from_binary(wine: Path) -> Path | None:
    candidates = [
        wine.parent.parent / "Resources" / "wine" / "lib" / "wine",
        wine.parent.parent / "lib" / "wine",
        wine.parent.parent.parent / "lib" / "wine",
    ]
    for candidate in candidates:
        if candidate.is_dir():
            return candidate
    return None


def builtin_artifacts(args: argparse.Namespace) -> list[Artifact]:
    lib_dir = (args.builtin_lib_dir.expanduser().resolve() if args.builtin_lib_dir else None)
    if lib_dir is None and args.wine:
        lib_dir = wine_lib_dir_from_binary(args.wine)
    if lib_dir is None:
        return []

    windows_dir = "i386-windows" if args.arch == "x86" else "x86_64-windows"
    unix_dir = "x86_64-unix"
    return [
        Artifact("builtin-d3d9.dll", lib_dir / windows_dir / "d3d9.dll"),
        Artifact("builtin-winemetal.dll", lib_dir / windows_dir / "winemetal.dll"),
        Artifact("builtin-winemetal.so", lib_dir / unix_dir / "winemetal.so"),
    ]


def pe_runtime_artifacts(pe_build_dir: Path) -> list[Artifact]:
    candidates: list[Artifact] = []
    search_dirs = [
        pe_build_dir / "tests" / "module_boundary",
        pe_build_dir / "tests" / "conformance" / "d3d9",
    ]
    for name in ("libc++.dll", "libunwind.dll"):
        for directory in search_dirs:
            path = directory / name
            if path.is_file():
                candidates.append(Artifact(name, path))
                break
    return candidates


def unix_runtime_artifacts(unix_build_dir: Path, wine: Path | None) -> list[Artifact]:
    candidates: list[Artifact] = []
    search_dirs = [unix_build_dir / "src" / "winemetal" / "unix"]
    if wine is not None:
        wine_lib_dir = wine_lib_dir_from_binary(wine)
        if wine_lib_dir is not None:
            search_dirs.append(wine_lib_dir / "x86_64-unix")
    for name in ("ntdll.so", "winemac.so"):
        for directory in search_dirs:
            path = directory / name
            if path.is_file():
                candidates.append(Artifact(name, path.resolve()))
                break
    return candidates


def discover_default_artifacts(args: argparse.Namespace) -> list[Artifact]:
    if args.artifact:
        return args.artifact
    if args.lane == "provider-side":
        native_build_dir = (args.native_build_dir or REPO_ROOT / "build").resolve()
        return [
            Artifact(
                "provider-probe",
                native_build_dir / "tests" / "native" / "backend" / "dxmt9-unix-chunk-injection-probe",
            ),
            Artifact("libdxmt9_native.dylib", native_build_dir / "src" / "libdxmt9_native.dylib"),
            Artifact("provider-winemetal.so", native_build_dir / "src" / "winemetal" / "unix" / "winemetal.so"),
        ]
    if args.lane not in {"app-local", "builtin"}:
        return []

    pe_build_dir = (args.pe_build_dir or default_pe_build_dir(args.arch, args.lane)).resolve()
    unix_build_dir = (args.unix_build_dir or DEFAULT_UNIX_BUILD_DIR).resolve()
    probe_name = f"dxmt9-module-boundary-probe_{args.arch}.exe"
    artifacts = [
        Artifact("probe", pe_build_dir / "tests" / "module_boundary" / probe_name),
    ]
    if args.lane == "builtin":
        return artifacts + builtin_artifacts(args)
    return artifacts + [
        Artifact("d3d9.dll", pe_build_dir / "tests" / "module_boundary" / "d3d9.dll"),
        Artifact("winemetal.dll", pe_build_dir / "tests" / "module_boundary" / "winemetal.dll"),
        Artifact("winemetal.so", unix_build_dir / "src" / "winemetal" / "unix" / "winemetal.so"),
    ] + pe_runtime_artifacts(pe_build_dir) + unix_runtime_artifacts(unix_build_dir, args.wine)


def load_manifest(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise FileNotFoundError(f"manifest missing: {path}")
    with path.open("rb") as handle:
        return tomllib.load(handle)


def manifest_cases(path: Path) -> list[dict[str, Any]]:
    data = load_manifest(path)
    cases = data.get("case")
    if not isinstance(cases, list):
        raise ValueError(f"manifest has no [[case]] entries: {path}")
    return [case for case in cases if isinstance(case, dict)]


def case_by_id(path: Path, case_id: str) -> dict[str, Any]:
    for case in manifest_cases(path):
        if case.get("id") == case_id:
            return case
    raise KeyError(f"case not found: {case_id}")


def validate_manifest(path: Path) -> None:
    checker = REPO_ROOT / "scripts" / "check" / "check_module_boundary_manifest.py"
    completed = subprocess.run(
        [sys.executable, str(checker), "--manifest", str(path)],
        cwd=REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        sys.stderr.write(completed.stdout)
        sys.stderr.write(completed.stderr)
        raise SystemExit(completed.returncode)
    print(completed.stdout, end="")


def list_cases(path: Path) -> None:
    for case in manifest_cases(path):
        lanes = ",".join(case.get("lanes", []))
        arches = ",".join(case.get("arches", []))
        print(f"{case.get('id')}\t{case.get('status')}\t{lanes}\t{arches}\t{case.get('title')}")


def base_result(args: argparse.Namespace, case: dict[str, Any]) -> dict[str, Any]:
    return {
        "schema": RESULT_SCHEMA,
        "case": case.get("id"),
        "lane": args.lane,
        "arch": args.arch,
        "cost_class": case.get("cost_class"),
        "command": sys.argv,
        "environment": selected_environment(),
        "artifacts": [],
        "exit_code": None,
        "failure_category": "none",
        "checks": [],
        "log_excerpt": [],
    }


def selected_environment() -> dict[str, str]:
    keys = [
        "WINEDLLOVERRIDES",
        "DXMT9_WINEMETAL_SO",
        "DXMT9_MODULE_BOUNDARY_LOAD_MODE",
        "DXMT9_ALLOW_RUNTIME_PROVIDER_FALLBACK",
        "DXMT_LOG_LEVEL",
        "DXMT9_BRIDGE_VERBOSE",
        "DYLD_LIBRARY_PATH",
    ]
    return {key: os.environ[key] for key in keys if key in os.environ}


def record_check(result: dict[str, Any], name: str, status: str, summary: str) -> None:
    result["checks"].append({"name": name, "status": status, "summary": summary})


def fail(result: dict[str, Any], category: str, summary: str) -> dict[str, Any]:
    if category not in VALID_FAILURE_CATEGORIES:
        category = "unsupported-runtime"
    result["failure_category"] = category
    result["exit_code"] = 1
    record_check(result, category, "fail", summary)
    return result


def debug_failure_category(module_category: str) -> str:
    if module_category == "none":
        return "none"
    if module_category == "bridge-abi-mismatch":
        return "wine-abi-handshake"
    if module_category in {
        "artifact-staging",
        "pe-loader-export",
        "unix-module-load",
        "provider-entry-dispatch",
        "public-d3d9-smoke",
    }:
        return "wine-provider-locator"
    return "headless-unsupported"


def build_debug_result(result: dict[str, Any]) -> dict[str, Any]:
    command = result.get("executed_command") or result.get("command") or sys.argv
    if not isinstance(command, list) or not command:
        command = sys.argv
    command = [str(item) for item in command if str(item)]

    case = str(result.get("case") or "unknown")
    lane = str(result.get("lane") or "unknown")
    arch = str(result.get("arch") or "unknown")
    diagnostics = {
        "module_boundary": {
            "case": case,
            "lane": lane,
            "arch": arch,
            "cost_class": result.get("cost_class"),
            "exit_code": result.get("exit_code"),
            "failure_category": result.get("failure_category"),
            "checks": result.get("checks", []),
        }
    }
    if "last_phase" in result:
        diagnostics["module_boundary"]["last_phase"] = result["last_phase"]
    if "timeout" in result:
        diagnostics["module_boundary"]["timeout"] = result["timeout"]
    if "phase_events" in result:
        diagnostics["module_boundary"]["phase_events"] = result["phase_events"]
    if "probe" in result:
        diagnostics["module_boundary"]["probe"] = result["probe"]
    if "debug_sidecars" in result:
        diagnostics["module_boundary"]["sidecars"] = result["debug_sidecars"]

    wine = {}
    if result.get("wine"):
        wine["binary"] = result["wine"]
    if lane in {"app-local", "builtin", "provider-side"}:
        wine["provider_locator_mode"] = lane
    if result.get("failure_category") == "bridge-abi-mismatch":
        wine["abi_status"] = "mismatch"
    elif result.get("failure_category") == "none":
        wine["abi_status"] = "not_applicable" if lane == "provider-side" else "unknown"
    else:
        wine["abi_status"] = "unknown"
    diagnostics["wine"] = wine

    return {
        "schema": debug_result_schema.SCHEMA,
        "module": "module-boundary",
        "boundary": f"module-boundary:{case}",
        "command": command,
        "correlation": {
            "run_id": f"module-boundary:{case}:{lane}:{arch}",
        },
        "environment": dict(result.get("environment") or {}),
        "artifacts": [],
        "diagnostics": diagnostics,
        "failure_category": debug_failure_category(str(result.get("failure_category") or "unsupported-runtime")),
    }


def write_json_sidecar(output: Path, rel: Path, payload: dict[str, Any]) -> dict[str, Any]:
    path = output.parent / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return {
        "role": "log",
        "path": rel.as_posix(),
        "format": "json",
    }


def module_boundary_sidecars(result: dict[str, Any], output: Path) -> list[dict[str, Any]]:
    artifacts: list[dict[str, Any]] = []
    sidecars: list[dict[str, Any]] = []
    staged = result.get("artifacts")
    if isinstance(staged, list) and staged:
        rel = Path("artifacts") / "module_boundary_stage_manifest.json"
        artifacts.append(
            {
                "role": "boundary-dump-manifest",
                "path": rel.as_posix(),
                "format": "json",
            }
        )
        stage_path = output.parent / rel
        stage_path.parent.mkdir(parents=True, exist_ok=True)
        stage_path.write_text(
            json.dumps(
                {
                    "schema": "dxmt9.module_boundary.stage_manifest.v1",
                    "case": result.get("case"),
                    "lane": result.get("lane"),
                    "arch": result.get("arch"),
                    "artifacts": staged,
                },
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
        sidecars.append({"kind": "stage-manifest", "path": rel.as_posix()})

    env = result.get("environment") if isinstance(result.get("environment"), dict) else {}
    locator_payload = {
        "schema": "dxmt9.module_boundary.provider_locator.v1",
        "lane": result.get("lane"),
        "wine": result.get("wine"),
        "provider_locator_mode": env.get("DXMT9_MODULE_BOUNDARY_LOAD_MODE", result.get("lane")),
        "selected_provider": env.get("DXMT9_WINEMETAL_SO"),
        "allow_runtime_fallback": env.get("DXMT9_ALLOW_RUNTIME_PROVIDER_FALLBACK"),
        "failure_category": result.get("failure_category"),
    }
    artifacts.append(write_json_sidecar(output, Path("logs") / "provider_locator.json", locator_payload))
    sidecars.append({"kind": "provider-locator", "path": "logs/provider_locator.json"})

    combined_log = "\n".join(
        str(result.get(key) or "") for key in ("stdout", "stderr")
    )
    abi_payload = {
        "schema": "dxmt9.module_boundary.abi_handshake.v1",
        "status": "mismatch" if result.get("failure_category") == "bridge-abi-mismatch" else "unknown",
        "failure_category": result.get("failure_category"),
        "diagnostic_excerpt": text_tail(combined_log).splitlines()[-20:],
    }
    if result.get("failure_category") == "none":
        abi_payload["status"] = "not_applicable" if result.get("lane") == "provider-side" else "unknown"
    artifacts.append(write_json_sidecar(output, Path("logs") / "abi_handshake.json", abi_payload))
    sidecars.append({"kind": "abi-handshake", "path": "logs/abi_handshake.json"})

    result["debug_sidecars"] = sidecars
    return artifacts


def write_debug_result(result: dict[str, Any], output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    sidecar_artifacts = module_boundary_sidecars(result, output)
    debug_result = build_debug_result(result)
    debug_result["artifacts"].extend(sidecar_artifacts)

    log_lines = []
    for key in ("stdout", "stderr"):
        value = result.get(key)
        if isinstance(value, str) and value:
            log_lines.append(f"===== {key} =====")
            log_lines.append(value)
    excerpt = result.get("log_excerpt")
    if isinstance(excerpt, list) and excerpt and not log_lines:
        log_lines = [str(line) for line in excerpt]
    if log_lines:
        log_path = output.parent / "logs" / "module_boundary.log"
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text("\n".join(log_lines) + "\n", encoding="utf-8")
        debug_result["artifacts"].append(
            {
                "role": "log",
                "path": log_path.relative_to(output.parent).as_posix(),
                "format": "text",
            }
        )

    errors = debug_result_schema.validate_result(debug_result)
    if errors:
        raise ValueError("invalid module-boundary debug result: " + "; ".join(errors))
    output.write_text(json.dumps(debug_result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {output}")


def emit_result(result: dict[str, Any], output: Path | None, debug_output: Path | None = None) -> None:
    text = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if output is None:
        print(text, end="")
    else:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(text, encoding="utf-8")
        print(f"wrote {output}")
    if debug_output is not None:
        write_debug_result(result, debug_output)


def stage_target(artifact: Artifact) -> Path:
    if artifact.role in ROLE_STAGE_PATH:
        return ROLE_STAGE_PATH[artifact.role]
    return Path("artifacts") / artifact.role / artifact.path.name


def stage_artifacts(artifacts: list[Artifact], stage_dir: Path) -> tuple[list[dict[str, Any]], dict[str, Path]]:
    stage_dir.mkdir(parents=True, exist_ok=True)
    records: list[dict[str, Any]] = []
    staged: dict[str, Path] = {}
    for artifact in artifacts:
        if not artifact.path.is_file():
            raise FileNotFoundError(f"{artifact.role}: artifact missing: {artifact.path}")
        target = stage_dir / stage_target(artifact)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(artifact.path, target)
        records.append(artifact_record(artifact.role, target, relative_to=stage_dir))
        staged[artifact.role] = target
    return records, staged


def validate_result_shape(path: Path) -> list[str]:
    errors: list[str] = []
    try:
        result = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        return [f"invalid JSON: {exc}"]

    if result.get("schema") != RESULT_SCHEMA:
        errors.append(f"schema must be {RESULT_SCHEMA!r}")
    if result.get("failure_category") not in VALID_FAILURE_CATEGORIES:
        errors.append(f"invalid failure_category {result.get('failure_category')!r}")
    if not isinstance(result.get("checks"), list):
        errors.append("checks must be a list")
    if not isinstance(result.get("artifacts"), list):
        errors.append("artifacts must be a list")
    if not isinstance(result.get("environment"), dict):
        errors.append("environment must be an object")
    return errors


def parse_probe_json(stdout: str) -> dict[str, Any] | None:
    start = stdout.find("{")
    end = stdout.rfind("}")
    if start < 0 or end < start:
        return None
    try:
        value = json.loads(stdout[start:end + 1])
    except json.JSONDecodeError:
        return None
    return value if isinstance(value, dict) else None


def parse_phase_events(stdout: str) -> list[dict[str, str]]:
    events: list[dict[str, str]] = []
    for line in stdout.splitlines():
        match = PHASE_EVENT_RE.match(line.strip())
        if match:
            events.append(match.groupdict())
    return events


def merge_phase_events(result: dict[str, Any], stdout: str) -> str | None:
    events = parse_phase_events(stdout)
    if not events:
        probe = result.get("probe")
        if isinstance(probe, dict) and isinstance(probe.get("last_phase"), str):
            result["last_phase"] = probe["last_phase"]
            return probe["last_phase"]
        return None
    result["phase_events"] = events
    last_phase = events[-1]["phase"]
    result["last_phase"] = last_phase
    return last_phase


def merge_probe_result(result: dict[str, Any], probe: dict[str, Any]) -> None:
    result["probe"] = probe
    if isinstance(probe.get("last_phase"), str):
        result["last_phase"] = probe["last_phase"]
    category = probe.get("failure_category")
    if isinstance(category, str) and category in VALID_FAILURE_CATEGORIES and category != "none":
        result["failure_category"] = category
    for item in probe.get("checks", []):
        if not isinstance(item, dict):
            continue
        name = str(item.get("name", "probe_check"))
        status = str(item.get("status", "unknown"))
        summary = json.dumps(item, sort_keys=True)
        record_check(result, name, status, summary)


def infer_failure_category(stdout: str, stderr: str) -> str | None:
    combined = stdout + stderr
    if "winemetal.dll/winemetal.so ABI hash mismatch" in combined:
        return "bridge-abi-mismatch"
    if (
        "abi-hash unix-call failed status=0xc0000135" in combined
        or "initialize(winemetal.so): final status=0xc0000135" in combined
    ):
        return "unix-module-load"
    return None


def apply_diagnostic_category(result: dict[str, Any], stdout: str, stderr: str) -> None:
    category = infer_failure_category(stdout, stderr)
    if category is None:
        return
    if result["failure_category"] in {"none", "public-d3d9-smoke", "command-submission"}:
        result["failure_category"] = category
    record_check(result, category, "fail", "classified from PE/unix bridge diagnostics")


def text_tail(value: str | bytes | None) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")[-8192:]
    return value[-8192:]


def run_provider_probe(args: argparse.Namespace, result: dict[str, Any], staged: dict[str, Path]) -> int:
    probe = staged.get("provider-probe")
    if probe is None:
        emit_result(fail(result, "artifact-staging", "missing role=provider-probe artifact"), args.output, args.debug_output)
        return 1

    env = os.environ.copy()
    env.setdefault("DXMT_PERF_COUNTERS", "1")
    env.setdefault("UNIX_CHUNK_INJECT_ITERATIONS", str(args.provider_iterations))
    env.setdefault("UNIX_CHUNK_INJECT_DRAWS", str(args.provider_draws))
    result["environment"].update({
        "DXMT_PERF_COUNTERS": env["DXMT_PERF_COUNTERS"],
        "UNIX_CHUNK_INJECT_ITERATIONS": env["UNIX_CHUNK_INJECT_ITERATIONS"],
        "UNIX_CHUNK_INJECT_DRAWS": env["UNIX_CHUNK_INJECT_DRAWS"],
    })

    command = [str(probe)]
    result["executed_command"] = command
    try:
        completed = subprocess.run(
            command,
            cwd=probe.parent,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=args.timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        result["exit_code"] = None
        result["stdout"] = text_tail(exc.stdout)
        result["stderr"] = text_tail(exc.stderr)
        result["log_excerpt"] = (result["stdout"] + result["stderr"]).splitlines()[-20:]
        emit_result(
            fail(result, "command-submission", f"provider probe timed out after {args.timeout}s"),
            args.output,
            args.debug_output,
        )
        return 1
    result["exit_code"] = completed.returncode
    result["stdout"] = completed.stdout[-8192:]
    result["stderr"] = completed.stderr[-8192:]
    result["log_excerpt"] = (completed.stdout + completed.stderr).splitlines()[-20:]

    if "[unix_chunk_inject]" in completed.stdout:
        record_check(result, "provider_entry_dispatch", "pass", "unix chunk injection probe emitted result line")
    else:
        result["failure_category"] = "provider-entry-dispatch"
        record_check(result, "provider_entry_dispatch", "fail", "missing unix chunk injection result line")

    if "[dxmt9-perf]" in completed.stdout or "[dxmt9-perf]" in completed.stderr:
        record_check(result, "provider_counters", "pass", "perf counter line emitted")
    else:
        record_check(result, "provider_counters", "skip", "perf counter line unavailable")

    if completed.returncode != 0:
        if result["failure_category"] == "none":
            result["failure_category"] = "command-submission"
        emit_result(result, args.output, args.debug_output)
        return completed.returncode

    if result["failure_category"] == "none":
        record_check(result, "runtime_execution", "pass", "provider-side probe completed successfully")
    emit_result(result, args.output, args.debug_output)
    return 0


def run_pe_probe(args: argparse.Namespace, result: dict[str, Any], staged: dict[str, Path]) -> int:
    probe = staged.get("probe")
    provider = staged.get("winemetal.so")
    if probe is None:
        emit_result(fail(result, "artifact-staging", "missing role=probe artifact"), args.output, args.debug_output)
        return 1
    if args.lane == "app-local" and provider is None:
        emit_result(
            fail(result, "artifact-staging", "app-local lane requires role=winemetal.so artifact"),
            args.output,
            args.debug_output,
        )
        return 1
    if not args.wine:
        emit_result(fail(result, "unsupported-runtime", "--wine is required for PE lanes"), args.output, args.debug_output)
        return 1

    env = os.environ.copy()
    env["WINEDLLOVERRIDES"] = args.dll_overrides or "d3d9,winemetal=n,b"
    env["DXMT9_MODULE_BOUNDARY_LOAD_MODE"] = args.lane
    if args.lane == "app-local" and provider is not None:
        env["DXMT9_WINEMETAL_SO"] = str(provider)
    result["environment"].update({
        "WINEDLLOVERRIDES": env["WINEDLLOVERRIDES"],
        "DXMT9_MODULE_BOUNDARY_LOAD_MODE": env["DXMT9_MODULE_BOUNDARY_LOAD_MODE"],
        **({"DXMT9_WINEMETAL_SO": env["DXMT9_WINEMETAL_SO"]} if "DXMT9_WINEMETAL_SO" in env else {}),
    })
    result["wine"] = str(args.wine)

    command = [str(args.wine), f".{os.sep}{probe.name}"]
    result["executed_command"] = command
    try:
        completed = subprocess.run(
            command,
            cwd=probe.parent,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=args.timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        result["exit_code"] = 1
        result["stdout"] = text_tail(exc.stdout)
        result["stderr"] = text_tail(exc.stderr)
        result["log_excerpt"] = (result["stdout"] + result["stderr"]).splitlines()[-20:]
        probe_json = parse_probe_json(result["stdout"])
        if probe_json is not None:
            merge_probe_result(result, probe_json)
        last_phase = merge_phase_events(result, result["stdout"])
        apply_diagnostic_category(result, result["stdout"], result["stderr"])
        result["timeout"] = {"seconds": args.timeout, "last_phase": last_phase}
        summary = f"PE probe timed out after {args.timeout}s"
        if last_phase:
            summary += f" during/after phase {last_phase}"
        if result["failure_category"] == "none":
            result["failure_category"] = "command-submission"
        record_check(result, "process_timeout", "fail", summary)
        emit_result(result, args.output, args.debug_output)
        return 1
    result["exit_code"] = completed.returncode
    result["stdout"] = completed.stdout[-8192:]
    result["stderr"] = completed.stderr[-8192:]
    result["log_excerpt"] = (completed.stdout + completed.stderr).splitlines()[-20:]
    merge_phase_events(result, completed.stdout)

    probe_json = parse_probe_json(completed.stdout)
    if probe_json is None:
        emit_result(
            fail(result, "public-d3d9-smoke", "probe did not emit parseable JSON"),
            args.output,
            args.debug_output,
        )
        return 1
    merge_probe_result(result, probe_json)
    apply_diagnostic_category(result, completed.stdout, completed.stderr)
    if completed.returncode != 0:
        if result["failure_category"] == "none":
            result["failure_category"] = "public-d3d9-smoke"
        emit_result(result, args.output, args.debug_output)
        return completed.returncode
    if result["failure_category"] != "none":
        emit_result(result, args.output, args.debug_output)
        return 1

    if result["failure_category"] == "none":
        record_check(result, "runtime_execution", "pass", "PE probe completed successfully")
    emit_result(result, args.output, args.debug_output)
    return 0


def scaffold_result(args: argparse.Namespace) -> int:
    case = case_by_id(args.manifest, args.case)
    result = base_result(args, case)
    for check in case.get("checks", []):
        record_check(result, str(check), "skip", "scaffold result only; runtime lane not executed")
    result["failure_category"] = "unsupported-runtime"
    result["exit_code"] = 0
    emit_result(result, args.output, args.debug_output)
    return 0


def run_case(args: argparse.Namespace) -> int:
    case = case_by_id(args.manifest, args.case)
    result = base_result(args, case)

    declared_lanes = set(case.get("lanes", []))
    declared_arches = set(case.get("arches", []))
    if args.lane not in declared_lanes:
        emit_result(
            fail(result, "unsupported-runtime", f"case does not support lane {args.lane}"),
            args.output,
            args.debug_output,
        )
        return 1
    if args.arch not in declared_arches:
        emit_result(
            fail(result, "unsupported-runtime", f"case does not support arch {args.arch}"),
            args.output,
            args.debug_output,
        )
        return 1

    with tempfile.TemporaryDirectory(prefix="dxmt9-module-boundary-") as tmp:
        stage_dir = (args.stage_dir or Path(tmp)).resolve()
        if args.execute and args.lane in {"app-local", "builtin"}:
            args.wine = resolve_wine(args)
        try:
            result["artifacts"], staged = stage_artifacts(discover_default_artifacts(args), stage_dir)
        except FileNotFoundError as exc:
            emit_result(fail(result, "artifact-staging", str(exc)), args.output, args.debug_output)
            return 1

        record_check(result, "artifact_staging", "pass", f"staged {len(result['artifacts'])} artifacts")

        if not args.execute:
            result["failure_category"] = "unsupported-runtime"
            result["exit_code"] = 0
            record_check(result, "runtime_execution", "skip", "use --execute to run Wine/provider lane")
            emit_result(result, args.output, args.debug_output)
            return 0

        if args.lane in {"app-local", "builtin"}:
            return run_pe_probe(args, result, staged)
        if args.lane == "provider-side":
            return run_provider_probe(args, result, staged)

        emit_result(
            fail(
                result,
                "unsupported-runtime",
                "provider-side execution is scaffolded until native provider probe is connected",
            ),
            args.output,
            args.debug_output,
        )
        return 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="dxmt9 module-boundary harness runner")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("validate-manifest")
    subparsers.add_parser("list")

    scaffold = subparsers.add_parser("emit-scaffold-result")
    scaffold.add_argument("--case", default="result-schema")
    scaffold.add_argument("--lane", default="provider-side")
    scaffold.add_argument("--arch", default="native")
    scaffold.add_argument("--output", type=Path)
    scaffold.add_argument("--debug-output", type=Path)

    validate_result = subparsers.add_parser("validate-result")
    validate_result.add_argument("path", type=Path)

    run = subparsers.add_parser("run")
    run.add_argument("--case", required=True)
    run.add_argument("--lane", choices=("provider-side", "app-local", "builtin"), required=True)
    run.add_argument("--arch", choices=("native", "x64", "x86"), required=True)
    run.add_argument("--artifact", action="append", type=parse_artifact, default=[])
    run.add_argument("--native-build-dir", type=Path)
    run.add_argument("--pe-build-dir", type=Path)
    run.add_argument("--unix-build-dir", type=Path)
    run.add_argument("--stage-dir", type=Path)
    run.add_argument("--output", type=Path)
    run.add_argument("--debug-output", type=Path)
    run.add_argument("--wine", type=Path)
    run.add_argument("--builtin-lib-dir", type=Path)
    run.add_argument("--dll-overrides")
    run.add_argument("--execute", action="store_true")
    run.add_argument("--timeout", type=int, default=60)
    run.add_argument("--provider-iterations", type=int, default=1)
    run.add_argument("--provider-draws", type=int, default=1)

    return parser.parse_args()


def main() -> int:
    args = parse_args()
    args.manifest = args.manifest.resolve()
    if args.command == "validate-manifest":
        validate_manifest(args.manifest)
        return 0
    if args.command == "list":
        list_cases(args.manifest)
        return 0
    if args.command == "emit-scaffold-result":
        return scaffold_result(args)
    if args.command == "validate-result":
        errors = validate_result_shape(args.path)
        if errors:
            for error in errors:
                print(error, file=sys.stderr)
            return 1
        print(f"module-boundary result ok: {args.path}")
        return 0
    if args.command == "run":
        return run_case(args)
    raise AssertionError(args.command)


if __name__ == "__main__":
    raise SystemExit(main())
