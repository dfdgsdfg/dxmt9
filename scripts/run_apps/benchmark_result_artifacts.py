"""Bounded discovery and atomic publication of benchmark-owned result files."""

from __future__ import annotations

import hashlib
import os
import re
import shutil
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


_DRIVE_LETTER_RE = re.compile(r"^([A-Za-z]):[\\/](.*)$")


@dataclass(frozen=True)
class BenchmarkResultFileFingerprint:
    size: int
    mtime_ns: int
    sha256: str


@dataclass(frozen=True)
class BenchmarkResultCapturePlan:
    product: str
    environment: str
    requested_value: str
    requested_mode: str
    requested_path: Path
    search_roots: tuple[Path, ...]
    before: dict[Path, BenchmarkResultFileFingerprint]


def benchmark_result_environment(app_name: str) -> tuple[str, str] | None:
    products = {
        "app-d3d9-3dmark05": ("3dmark05", "DXMT_3DMARK05_RESULT_FILE"),
        "app-d3d9-3dmark06": ("3dmark06", "DXMT_3DMARK06_RESULT_FILE"),
    }
    return products.get(app_name)


def fingerprint_benchmark_result_file(
    path: Path,
    *,
    attempts: int = 3,
) -> BenchmarkResultFileFingerprint | None:
    for _ in range(attempts):
        try:
            if path.is_symlink() or not path.is_file():
                return None
            before = path.stat()
            digest = hashlib.sha256()
            with path.open("rb") as handle:
                while chunk := handle.read(1024 * 1024):
                    digest.update(chunk)
            after = path.stat()
        except OSError:
            return None
        if before.st_size == after.st_size and before.st_mtime_ns == after.st_mtime_ns:
            return BenchmarkResultFileFingerprint(
                size=after.st_size,
                mtime_ns=after.st_mtime_ns,
                sha256=digest.hexdigest(),
            )
        time.sleep(0.01)
    return None


def snapshot_benchmark_result_files(
    roots: tuple[Path, ...],
) -> dict[Path, BenchmarkResultFileFingerprint]:
    snapshot: dict[Path, BenchmarkResultFileFingerprint] = {}
    for root in roots:
        if root.is_symlink() or not root.is_dir():
            continue
        for directory, directory_names, file_names in os.walk(root, followlinks=False):
            directory_path = Path(directory)
            directory_names[:] = [
                name for name in directory_names
                if not (directory_path / name).is_symlink()
            ]
            for name in file_names:
                if Path(name).suffix.lower() != ".3dr":
                    continue
                path = directory_path / name
                fingerprint = fingerprint_benchmark_result_file(path)
                if fingerprint is not None:
                    snapshot[path.absolute()] = fingerprint
    return snapshot


def _requested_benchmark_result_path(
    value: str,
    *,
    workdir: Path,
    prefix: Path,
) -> Path:
    drive_match = _DRIVE_LETTER_RE.match(value)
    if drive_match:
        drive, rest = drive_match.groups()
        normalized_rest = rest.replace("\\", "/").lstrip("/")
        return prefix / f"drive_{drive.lower()}" / normalized_rest
    requested = Path(value.replace("\\", "/")).expanduser()
    if requested.is_absolute():
        return requested
    return workdir / requested


def prepare_benchmark_result_capture(
    *,
    app_name: str,
    workdir: Path,
    prefix: Path,
    output_name: str,
    env: dict[str, str],
) -> BenchmarkResultCapturePlan | None:
    identity = benchmark_result_environment(app_name)
    if identity is None:
        return None
    product, environment = identity
    requested_value = env.get(environment, "")
    requested_mode = "explicit"
    if not requested_value:
        safe_output_name = re.sub(r"[^A-Za-z0-9._-]+", "-", output_name).strip(".-")
        requested_value = (
            f"{safe_output_name or product}-{os.getpid()}-{time.time_ns()}.3dr"
        )
        env[environment] = requested_value
        requested_mode = "auto"

    requested_path = _requested_benchmark_result_path(
        requested_value,
        workdir=workdir,
        prefix=prefix,
    )
    candidates = (
        workdir,
        prefix / "drive_c" / "users",
        requested_path.parent,
    )
    roots: list[Path] = []
    seen: set[Path] = set()
    for candidate in candidates:
        absolute = candidate.absolute()
        if absolute in seen:
            continue
        seen.add(absolute)
        roots.append(absolute)
    roots_tuple = tuple(roots)
    return BenchmarkResultCapturePlan(
        product=product,
        environment=environment,
        requested_value=requested_value,
        requested_mode=requested_mode,
        requested_path=requested_path.absolute(),
        search_roots=roots_tuple,
        before=snapshot_benchmark_result_files(roots_tuple),
    )


def _atomic_copy_benchmark_result(
    source: Path,
    destination: Path,
    expected: BenchmarkResultFileFingerprint,
) -> BenchmarkResultFileFingerprint:
    destination.parent.mkdir(parents=True, exist_ok=True)
    for _ in range(3):
        descriptor, temporary_name = tempfile.mkstemp(
            dir=destination.parent,
            prefix=f".{destination.name}.",
            suffix=".tmp",
        )
        temporary = Path(temporary_name)
        try:
            with source.open("rb") as source_handle, os.fdopen(descriptor, "wb") as destination_handle:
                shutil.copyfileobj(source_handle, destination_handle, length=1024 * 1024)
                destination_handle.flush()
                os.fsync(destination_handle.fileno())
            source_fingerprint = fingerprint_benchmark_result_file(source)
            copied_fingerprint = fingerprint_benchmark_result_file(temporary)
            if source_fingerprint == expected and copied_fingerprint is not None:
                if copied_fingerprint.sha256 == expected.sha256:
                    os.replace(temporary, destination)
                    return copied_fingerprint
        finally:
            try:
                os.close(descriptor)
            except OSError:
                pass
            temporary.unlink(missing_ok=True)
        time.sleep(0.01)
    raise OSError(f"benchmark result changed while copying: {source}")


def collect_benchmark_result_files(
    plan: BenchmarkResultCapturePlan,
    *,
    output_dir: Path,
) -> dict[str, Any]:
    current = snapshot_benchmark_result_files(plan.search_roots)
    changed = [
        (path, fingerprint, "created" if path not in plan.before else "modified")
        for path, fingerprint in current.items()
        if plan.before.get(path) != fingerprint
    ]
    artifact_root = output_dir / "benchmark-results"
    files: list[dict[str, Any]] = []
    errors: list[dict[str, str]] = []
    captured_sources: set[Path] = set()
    used_names: set[str] = set()
    for source, fingerprint, change in sorted(changed, key=lambda item: str(item[0])):
        artifact_name = source.name
        if artifact_name in used_names:
            base_name = (
                f"{source.stem}-{fingerprint.sha256[:12]}{source.suffix.lower()}"
            )
            artifact_name = base_name
            collision = 2
            while artifact_name in used_names:
                artifact_name = (
                    f"{Path(base_name).stem}-{collision}{source.suffix.lower()}"
                )
                collision += 1
        used_names.add(artifact_name)
        destination = artifact_root / artifact_name
        try:
            copied = _atomic_copy_benchmark_result(source, destination, fingerprint)
        except OSError as exc:
            errors.append({"source": str(source), "message": str(exc)})
            continue
        files.append({
            "source": str(source),
            "artifact": str(destination.relative_to(output_dir)),
            "change": change,
            "bytes": copied.size,
            "sha256": copied.sha256,
            "source_mtime_ns": fingerprint.mtime_ns,
        })
        captured_sources.add(source.absolute())

    return {
        "format": "3dr",
        "product": plan.product,
        "requested": {
            "environment": plan.environment,
            "value": plan.requested_value,
            "mode": plan.requested_mode,
            "resolved_source": str(plan.requested_path),
        },
        "search_roots": [str(path) for path in plan.search_roots],
        "status": "error" if errors else ("captured" if files else "not_emitted"),
        "found": len(files),
        "missing_requested": plan.requested_path not in captured_sources,
        "files": files,
        "errors": errors,
    }
