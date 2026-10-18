#!/usr/bin/env python3
"""Resolve archived retention sources through bounded, verified staging."""
from __future__ import annotations

from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile
from typing import Any, Iterator

ROOT = Path(__file__).resolve().parents[2]
STORE_RELATIVE = Path("experiments/output/_retention")
MAX_CONTROL_BYTES = 16 * 1024 * 1024
MAX_OPERATIONS = 200_000
HEX_SHA256 = re.compile(r"^[0-9a-f]{64}$")
READER_QUALIFICATION_SCHEMA = "dxmt9.retention.reader-qualification.v1"
SUPPORTED_READER_CONTRACTS = {"cpu-supply-v2", "wait-trace-v1"}

from scripts.tools import retention_archive


def _json(path: Path) -> dict[str, Any]:
    _safe_path(path)
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_CONTROL_BYTES:
        raise ValueError(f"invalid or oversized retention control file: {path}")
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"invalid retention control file: {path}")
    return value


def _safe_path(path: Path) -> Path:
    path = Path(os.path.abspath(path))
    for part in (path, *path.parents):
        if part.is_symlink() and str(part) not in ("/var", "/tmp"):
            raise ValueError(f"symlinked retention path: {part}")
    return path


def _hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _repo_path(path: Path, repo: Path) -> Path:
    raw = Path(path)
    if not raw.is_absolute() and ".." in raw.parts:
        raise ValueError("parent traversal is not allowed")
    result = _safe_path(raw if raw.is_absolute() else repo / raw)
    try:
        result.relative_to(repo)
    except ValueError:
        # Existing external inputs are valid read-only inputs, but can never be
        # mapped to a repository archive record.
        return result
    return result


def _logical_path(path: Path, repo: Path) -> str | None:
    try:
        return path.relative_to(repo).as_posix()
    except ValueError:
        return None


def _regular_file(path: Path) -> bool:
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False
    if stat.S_ISDIR(info.st_mode):
        return False
    if not stat.S_ISREG(info.st_mode):
        raise ValueError(f"logical source is not a regular file: {path}")
    return True


@contextmanager
def _reader_lease(store: Path) -> Iterator[None]:
    if not store.exists():
        yield
        return
    _safe_path(store)
    lock = _safe_path(store / "mutation.lock")
    with lock.open("a+") as stream:
        try:
            fcntl.flock(stream, fcntl.LOCK_SH | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError("another retention mutation holds the lease") from error
        try:
            yield
        finally:
            fcntl.flock(stream, fcntl.LOCK_UN)


def _receipt_paths(operations: Path) -> list[Path]:
    paths: list[Path] = []
    for path in operations.glob("*/receipt.json"):
        paths.append(path)
        if len(paths) > MAX_OPERATIONS:
            raise ValueError("retention operation count exceeds resolver bound")
    return paths


def _zstd() -> str:
    # Prefer an installed Homebrew keg even when its bin directory is absent
    # from PATH, then use the active PATH tool.
    brew = shutil.which("brew")
    if brew:
        try:
            prefix = subprocess.run([brew, "--prefix", "zstd"], check=True,
                                    capture_output=True, text=True, timeout=10).stdout.strip()
            candidate = Path(prefix) / "bin" / "zstd"
            if candidate.is_file() and os.access(candidate, os.X_OK):
                return str(candidate)
        except (OSError, subprocess.SubprocessError):
            pass
    found = shutil.which("zstd")
    if not found:
        raise FileNotFoundError("zstd is required to resolve archived retention sources")
    return found


def _receipt_records(repo: Path, store: Path, logical: str) -> tuple[list[dict[str, Any]], bool]:
    operations = store / "operations"
    matches: list[dict[str, Any]] = []
    interrupted = False
    if not operations.exists():
        return matches, interrupted
    _safe_path(operations)
    receipt_paths = _receipt_paths(operations)
    for receipt_path in receipt_paths:
        receipt = _json(receipt_path)
        actions = receipt.get("actions")
        if not isinstance(actions, list):
            raise ValueError(f"invalid receipt actions: {receipt_path}")
        touched = [item for item in actions if isinstance(item, dict) and item.get("path") == logical]
        if not touched:
            continue
        if receipt.get("status") in ("interrupted", "running") or any(
                item.get("status") in ("running", "interrupted") or
                item.get("phase") not in (None, "complete") and item.get("status") != "complete"
                for item in touched):
            interrupted = True
        if receipt.get("status") in ("complete", "complete-with-blocked") and any(
                item.get("status") == "complete" and
                item.get("availability") in ("deleted", "pruned-derived") for item in touched):
            raise ValueError(f"tombstoned source requires explicit recovery: {logical}")
        for action in touched:
            record = _validate_record(repo, store, receipt_path, receipt, action)
            if record is not None:
                matches.append(record)
    return matches, interrupted


def _validate_record(repo: Path, store: Path, receipt_path: Path,
                     receipt: dict[str, Any], action: dict[str, Any]) -> dict[str, Any] | None:
    """Return one completed archived file record; reject malformed bindings."""
    if (action.get("action") != "archive" or action.get("status") != "complete" or
            action.get("phase") != "complete" or action.get("availability") != "archived"):
        return None
    operation = receipt_path.parent
    plan_path, contents_path = operation / "plan.json", operation / "contents.json"
    plan = _json(plan_path)
    contents = _json(contents_path)
    if (receipt.get("schema") != "dxmt9.retention.v1" or
            plan.get("schema") != "dxmt9.retention.v1" or plan.get("repo") != str(repo) or
            contents.get("schema") != "dxmt9.retention.v1"):
        raise ValueError(f"retention schema or repository binding mismatch: {operation}")
    if receipt.get("plan_sha256") != _hash_file(plan_path):
        raise ValueError(f"receipt does not bind frozen plan: {receipt_path}")
    entries = plan.get("entries")
    sources = contents.get("sources")
    if not isinstance(entries, list) or not isinstance(sources, list):
        raise ValueError(f"invalid plan or contents records: {operation}")
    path = action.get("path")
    planned = [entry for entry in entries if isinstance(entry, dict) and entry.get("path") == path]
    recorded = [source for source in sources if isinstance(source, dict) and source.get("path") == path]
    if len(planned) != 1 or len(recorded) != 1:
        raise ValueError(f"source/action binding is missing or ambiguous: {path}")
    entry, source = planned[0], recorded[0]
    snapshot = entry.get("snapshot")
    if (entry.get("action") != action.get("action") or
            action.get("action") != "archive" or source.get("snapshot") != snapshot or
            action.get("status") != "complete" or action.get("phase") != "complete" or
            action.get("availability") != "archived"):
        raise ValueError(f"archived source binding mismatch: {path}")
    if receipt.get("status") not in ("complete", "complete-with-blocked"):
        return None
    if not isinstance(snapshot, dict) or snapshot.get("kind") != "file":
        return None
    retention_archive._validate_snapshot(snapshot)
    if len(snapshot["entries"]) != 1 or snapshot["entries"][0]["path"] != Path(path).name:
        raise ValueError(f"file snapshot does not bind logical basename: {path}")
    archive = action.get("archive")
    if not isinstance(archive, dict) or archive.get("contents") != snapshot:
        raise ValueError(f"archive does not bind source snapshot: {path}")
    archive_digest = archive.get("sha256")
    if not isinstance(archive_digest, str) or not HEX_SHA256.fullmatch(archive_digest):
        raise ValueError(f"invalid archive digest: {path}")
    expected_archive = store / "archives" / f"{archive_digest}.tar.zst"
    recorded_archive = archive.get("archive_path")
    if not isinstance(recorded_archive, str) or Path(recorded_archive) != expected_archive:
        raise ValueError(f"archive is not at its content-addressed path: {path}")
    archive_path = _safe_path(expected_archive)
    if not archive_path.is_file() or archive_path.is_symlink() or _hash_file(archive_path) != archive_digest:
        raise ValueError(f"archive hash or file type mismatch: {archive_path}")
    policy = plan.get("policy")
    if not isinstance(policy, dict):
        raise ValueError(f"plan has no frozen capacity policy: {plan_path}")
    floor = policy.get("free_space_floor_bytes")
    if not isinstance(floor, int) or floor < 0:
        raise ValueError(f"invalid frozen free-space floor: {plan_path}")
    if receipt.get("policy") != policy:
        raise ValueError(f"receipt capacity policy differs from frozen plan: {receipt_path}")
    qualification = _validate_reader_qualification(
        repo, store, entry, action, snapshot, archive_digest)
    return {"snapshot": snapshot, "archive": archive_path,
            "archive_sha256": archive_digest, "floor": floor,
            "operation": operation, "reader_qualification": qualification}


def _validate_reader_qualification(repo: Path, store: Path,
                                   entry: dict[str, Any], action: dict[str, Any],
                                   snapshot: dict[str, Any], archive_sha256: str) -> dict[str, Any] | None:
    review = entry.get("review")
    if not isinstance(review, dict):
        review = {}
    planned = entry.get("reader_qualification")
    contract = review.get("reader_contract")
    if planned is None and contract is None:
        return None
    if not isinstance(planned, dict):
        raise ValueError("qualified archive has no frozen reader qualification")
    bound = action.get("reader_qualification")
    if not isinstance(bound, dict) or bound != planned:
        raise ValueError("archive action reader qualification differs from frozen plan")
    if contract is None:
        contract = planned.get("contract")
    if (not isinstance(contract, str) or contract not in SUPPORTED_READER_CONTRACTS or
            planned.get("contract") != contract):
        raise ValueError("reader qualification contract binding mismatch")
    raw_path, proof_sha256 = planned.get("path"), planned.get("sha256")
    if (not isinstance(raw_path, str) or not isinstance(proof_sha256, str) or
            not HEX_SHA256.fullmatch(proof_sha256)):
        raise ValueError("invalid reader qualification path or digest")
    relative = Path(raw_path)
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError("reader qualification path traversal")
    proof_path = _safe_path(repo / relative)
    operations = store / "operations"
    if (proof_path.parent.parent != operations or
            not proof_path.name.startswith("reader-qualification-")):
        raise ValueError("reader qualification must be an operations proof")
    if not proof_path.is_file():
        raise ValueError("reader qualification proof is missing")
    proof = _json(proof_path)
    if _hash_file(proof_path) != proof_sha256:
        raise ValueError("reader qualification proof digest mismatch")
    logical = action["path"]
    parity = proof.get("original_reader_sha256")
    if (proof.get("schema") != READER_QUALIFICATION_SCHEMA or
            proof.get("source") != logical or
            proof.get("source_bytes") != snapshot.get("total_bytes") or
            proof.get("source_sha256") != snapshot.get("tree_digest") or
            proof.get("archive_sha256") != archive_sha256 or
            proof.get("contract") != contract or proof.get("qualified") is not True or
            not isinstance(parity, str) or not HEX_SHA256.fullmatch(parity) or
            proof.get("restored_reader_sha256") != parity):
        raise ValueError("reader qualification proof/source/archive/parity binding mismatch")
    return planned


def _matching_record(path: Path, repo: Path, store: Path,
                     expected_sha256: str | None = None) -> dict[str, Any]:
    logical = _logical_path(path, repo)
    if logical is None:
        raise FileNotFoundError(f"missing external source cannot be resolved: {path}")
    if (".." in Path(logical).parts or
            not (logical.startswith("traces/") or logical.startswith("experiments/output/")) or
            logical == "experiments/output/_retention" or
            logical.startswith("experiments/output/_retention/")):
        raise ValueError(f"path is outside the archiveable source namespace: {logical}")
    records, interrupted = _receipt_records(repo, store, logical)
    if interrupted:
        raise ValueError(f"interrupted retention operation touches source: {logical}")
    if expected_sha256 is not None:
        if not isinstance(expected_sha256, str) or not HEX_SHA256.fullmatch(expected_sha256):
            raise ValueError("expected_sha256 must be a lowercase SHA-256 digest")
        records = [item for item in records
                   if item["snapshot"]["tree_digest"] == expected_sha256]
    if not records:
        raise FileNotFoundError(f"no completed archive for missing source: {logical}")
    identities = {item["snapshot"]["tree_digest"] for item in records}
    if len(identities) != 1:
        raise ValueError(f"ambiguous archived identities for source: {logical}")
    # Multiple valid operations for identical original bytes are equivalent;
    # choose the first stable operation path so resolution is deterministic.
    return sorted(records, key=lambda item: str(item["operation"]))[0]


@contextmanager
def resolved_artifact(path: Path, *, repo: Path | None = None,
                      expected_sha256: str | None = None) -> Iterator[Path]:
    """Yield a verified materialized or privately restored source path."""
    repository = _safe_path(Path(repo) if repo is not None else ROOT)
    argument = Path(path)
    if repo is None and not argument.is_absolute():
        argument = Path.cwd() / argument
    source = _repo_path(argument, repository)
    store = repository / STORE_RELATIVE
    with _reader_lease(store):
        if _regular_file(source):
            if expected_sha256 is not None and _hash_file(source) != expected_sha256:
                raise ValueError(f"source digest mismatch: {source}")
            yield source
            return
        if source.exists() or source.is_symlink():
            raise ValueError(f"logical source exists but is not a regular file: {source}")
        record = _matching_record(source, repository, store, expected_sha256)
        staging_parent = _safe_path(store / "reader-staging")
        staging_parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        if staging_parent.is_symlink() or not stat.S_ISDIR(staging_parent.lstat().st_mode):
            raise ValueError("reader staging path is not a real directory")
        os.chmod(staging_parent, 0o700)
        staging = Path(tempfile.mkdtemp(prefix="reader-", dir=staging_parent))
        try:
            os.chmod(staging, 0o700)
            destination = staging / source.name
            retention_archive.restore_verified(record["archive"], record["snapshot"],
                                                destination, record["floor"], _zstd())
            if _hash_file(destination) != record["snapshot"]["tree_digest"]:
                raise ValueError(f"restored source digest mismatch: {source}")
            yield destination
        finally:
            retention_archive._remove_staging_tree(staging)


def verify_available(path: Path, *, repo: Path | None = None) -> int:
    """Verify a source and any archived missing descendants of a cited directory."""
    repository = _safe_path(Path(repo) if repo is not None else ROOT)
    argument = Path(path)
    if repo is None and not argument.is_absolute():
        argument = Path.cwd() / argument
    source = _repo_path(argument, repository)
    store = repository / STORE_RELATIVE
    with _reader_lease(store):
        if _regular_file(source):
            return 0
        if source.is_dir() and not source.is_symlink():
            logical = _logical_path(source, repository)
            if logical is None:
                return 0
            if (not (logical.startswith("traces/") or logical.startswith("experiments/output/")) or
                    logical == "experiments/output/_retention" or
                    logical.startswith("experiments/output/_retention/")):
                raise ValueError(f"directory is outside the archiveable source namespace: {logical}")
            return _verify_directory(source, logical, repository, store)
        with resolved_artifact(source, repo=repository):
            return 1


def _verify_directory(directory: Path, logical: str, repo: Path, store: Path) -> int:
    operations = store / "operations"
    if not operations.exists():
        return 0
    paths: set[str] = set()
    receipt_paths = _receipt_paths(operations)
    for receipt_path in receipt_paths:
        receipt = _json(receipt_path)
        actions = receipt.get("actions")
        if not isinstance(actions, list):
            raise ValueError(f"invalid receipt actions: {receipt_path}")
        for action in actions:
            candidate = action.get("path") if isinstance(action, dict) else None
            if isinstance(candidate, str) and candidate.startswith(logical.rstrip("/") + "/"):
                candidate_path = repo / candidate
                try:
                    candidate_path.relative_to(directory)
                except ValueError:
                    continue
                if candidate_path.is_symlink():
                    raise ValueError(f"archived directory child is a symlink: {candidate_path}")
                try:
                    child_info = candidate_path.lstat()
                except FileNotFoundError:
                    paths.add(candidate)
                    continue
                archive_touched = (action.get("action") == "archive" and
                                   action.get("availability") == "archived")
                if archive_touched and not stat.S_ISREG(child_info.st_mode):
                    raise ValueError(f"archived directory child is not a regular file: {candidate_path}")
    count = 0
    for candidate in sorted(paths):
        with resolved_artifact(repo / candidate, repo=repo):
            count += 1
    return count


def read_bytes(path: Path, *, repo: Path | None = None) -> bytes:
    with resolved_artifact(path, repo=repo) as physical:
        return physical.read_bytes()


def digest(path: Path, *, repo: Path | None = None) -> str:
    with resolved_artifact(path, repo=repo) as physical:
        return _hash_file(physical)
