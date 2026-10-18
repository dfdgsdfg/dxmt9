#!/usr/bin/env python3
"""Bounded, verified zstd archive and restore primitives for trace retention."""
from __future__ import annotations

import hashlib
import json
import os
import platform
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import tarfile
import tempfile
from typing import Any

MAX_ENTRIES = 100_000
MAX_EXPANDED_BYTES = 1 << 50
CHUNK_BYTES = 1024 * 1024


def _canonical(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      allow_nan=False).encode("utf-8")


def _safe_path(path: Path) -> Path:
    path = Path(os.path.abspath(path))
    for part in (path, *path.parents):
        if part.is_symlink() and str(part) not in ("/var", "/tmp"):
            raise ValueError(f"symlinked retention path: {part}")
    return path


def _identity(info: os.stat_result) -> dict[str, int]:
    return {"device": info.st_dev, "inode": info.st_ino,
            "mtime_ns": info.st_mtime_ns, "size": info.st_size}


def _file_digest(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        while True:
            block = stream.read(CHUNK_BYTES)
            if not block:
                break
            digest.update(block)
            size += len(block)
    return digest.hexdigest(), size


def _tree_digest(entries: list[dict[str, Any]]) -> str:
    rows = []
    for entry in entries:
        if entry["kind"] == "directory":
            rows.append({"path": entry["path"], "kind": "directory"})
        else:
            rows.append({"path": entry["path"], "sha256": entry["sha256"],
                         "bytes": entry["size"]})
    return hashlib.sha256(_canonical(rows)).hexdigest()


def snapshot_tree(source: str | Path) -> dict[str, Any]:
    """Snapshot a regular file or symlink-free directory tree."""
    source = _safe_path(Path(source))
    root_stat = source.lstat()
    if stat.S_ISREG(root_stat.st_mode):
        digest, size = _file_digest(source)
        if _identity(source.lstat()) != _identity(root_stat):
            raise ValueError(f"source changed while snapshotting: {source}")
        entries = [{"path": source.name, "kind": "file", "size": size,
                    "sha256": digest, "mode": stat.S_IMODE(root_stat.st_mode),
                    "mtime_ns": root_stat.st_mtime_ns,
                    "identity": _identity(root_stat)}]
        return {"kind": "file", "root_name": source.name,
                "identity": _identity(root_stat), "entries": entries,
                "tree_digest": digest, "total_bytes": size}
    if not stat.S_ISDIR(root_stat.st_mode):
        raise ValueError(f"unsupported retention source: {source}")

    entries: list[dict[str, Any]] = []
    total = 0
    for path in sorted(source.rglob("*"), key=lambda item: item.relative_to(source).as_posix()):
        info = path.lstat()
        relative = path.relative_to(source).as_posix()
        if stat.S_ISDIR(info.st_mode):
            entry = {"path": relative, "kind": "directory", "size": 0,
                     "mode": stat.S_IMODE(info.st_mode), "mtime_ns": info.st_mtime_ns,
                     "identity": _identity(info)}
        elif stat.S_ISREG(info.st_mode):
            digest, size = _file_digest(path)
            after = path.lstat()
            if _identity(after) != _identity(info):
                raise ValueError(f"source changed while snapshotting: {path}")
            entry = {"path": relative, "kind": "file", "size": size,
                     "sha256": digest, "mode": stat.S_IMODE(info.st_mode),
                     "mtime_ns": info.st_mtime_ns, "identity": _identity(info)}
            total += size
        else:
            raise ValueError(f"unsupported retention entry: {path}")
        entries.append(entry)
        if len(entries) > MAX_ENTRIES or total > MAX_EXPANDED_BYTES:
            raise ValueError("retention source exceeds archive bounds")
    if _identity(source.lstat()) != _identity(root_stat):
        raise ValueError(f"source changed while snapshotting: {source}")
    return {"kind": "directory", "root_name": source.name,
            "identity": _identity(root_stat), "root_mode": stat.S_IMODE(root_stat.st_mode),
            "root_mtime_ns": root_stat.st_mtime_ns, "entries": entries,
            "tree_digest": _tree_digest(entries), "total_bytes": total}


def _validate_snapshot(snapshot: dict[str, Any]) -> dict[str, dict[str, Any]]:
    if not isinstance(snapshot, dict) or snapshot.get("kind") not in ("file", "directory"):
        raise ValueError("invalid expected snapshot")
    entries = snapshot.get("entries")
    if not isinstance(entries, list) or len(entries) > MAX_ENTRIES:
        raise ValueError("invalid expected snapshot entries")
    by_name: dict[str, dict[str, Any]] = {}
    total = 0
    for entry in entries:
        if not isinstance(entry, dict) or entry.get("kind") not in ("file", "directory"):
            raise ValueError("invalid expected snapshot entry")
        name = entry.get("path")
        posix = PurePosixPath(name) if isinstance(name, str) else None
        if (posix is None or not name or posix.is_absolute() or
                any(part in ("", ".", "..") for part in name.split("/")) or
                posix.as_posix() != name or name in by_name):
            raise ValueError("invalid or duplicate expected member path")
        size = entry.get("size")
        if not isinstance(size, int) or size < 0:
            raise ValueError("invalid expected member size")
        if entry["kind"] == "file":
            digest = entry.get("sha256")
            if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
                raise ValueError("invalid expected member digest")
            total += size
        elif size != 0:
            raise ValueError("directory member has nonzero size")
        if not isinstance(entry.get("mode"), int) or not isinstance(entry.get("mtime_ns"), int):
            raise ValueError("invalid expected member metadata")
        by_name[name] = entry
    if snapshot["kind"] == "directory":
        for name in by_name:
            parent = PurePosixPath(name).parent
            if parent != PurePosixPath("."):
                ancestor = by_name.get(parent.as_posix())
                if ancestor is None or ancestor["kind"] != "directory":
                    raise ValueError("expected snapshot has a missing or non-directory parent")
    if total != snapshot.get("total_bytes") or total > MAX_EXPANDED_BYTES:
        raise ValueError("expected snapshot byte count mismatch")
    if snapshot["kind"] == "file" and (len(by_name) != 1 or next(iter(by_name.values()))["kind"] != "file"):
        raise ValueError("invalid file snapshot")
    calculated = (next(iter(by_name.values()))["sha256"] if snapshot["kind"] == "file"
                  else _tree_digest(entries))
    if calculated != snapshot.get("tree_digest"):
        raise ValueError("expected snapshot digest mismatch")
    return by_name


def _source_matches(source: Path, expected: dict[str, Any]) -> None:
    current = snapshot_tree(source)
    if current != expected:
        raise ValueError(f"source differs from expected snapshot: {source}")


def _preflight(path: Path, min_free_bytes: int, required_bytes: int) -> None:
    if min_free_bytes < 0 or required_bytes < 0:
        raise ValueError("space requirements must be nonnegative")
    path.mkdir(parents=True, exist_ok=True)
    path = _safe_path(path)
    if shutil.disk_usage(path).free < min_free_bytes + required_bytes:
        raise OSError("insufficient free space for retention operation")


def _archive_bound(snapshot: dict[str, Any]) -> int:
    # Tar headers, PAX metadata and block padding dominate for tiny files; zstd
    # can expand incompressible input slightly, so reserve raw bytes plus margin.
    headers = sum(1024 + len(entry["path"].encode("utf-8")) * 2
                  for entry in snapshot["entries"])
    tar_bound = snapshot["total_bytes"] + headers + 10240
    # zstd's frame and block headers add a small amount even when input is
    # incompressible. Keep a conservative fixed margin plus per-block allowance.
    return tar_bound + 1024 * 1024 + ((tar_bound + 131071) // 131072) * 64


def _restore_allocation_bound(snapshot: dict[str, Any], block_size: int) -> int:
    """Estimate allocated data plus inode/directory metadata for the staged tree."""
    if block_size <= 0:
        raise ValueError("filesystem block size must be positive")
    allocation = 8192  # destination root or temporary parent for a file restore
    for entry in snapshot["entries"]:
        allocation += 8192  # per-entry inode, directory record and metadata allowance
        if entry["kind"] == "file" and entry["size"]:
            allocation += ((entry["size"] + block_size - 1) // block_size) * block_size
    return allocation


class _BoundedReader:
    """Count decompressed bytes returned from the zstd pipe, with one-byte overrun detection."""
    def __init__(self, stream, limit: int):
        self.stream = stream
        self.limit = limit
        self.count = 0

    def read(self, size: int = -1) -> bytes:
        remaining = self.limit - self.count
        request = min(CHUNK_BYTES if size < 0 else size, remaining + 1)
        data = self.stream.read(request)
        self.count += len(data)
        if self.count > self.limit:
            raise ValueError("decompressed archive exceeds its byte bound")
        return data


def _rename_noreplace(source: Path, destination: Path) -> None:
    """Atomically publish without replacing an entry created by a racing actor."""
    if platform.system() == "Darwin":
        import ctypes
        libc = ctypes.CDLL(None, use_errno=True)
        rename_excl = getattr(libc, "renamex_np", None)
        if rename_excl is None:
            raise OSError("atomic no-replace rename is unavailable")
        rename_excl.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint)
        rename_excl.restype = ctypes.c_int
        if rename_excl(os.fsencode(source), os.fsencode(destination), 0x4) != 0:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error), str(destination))
        return
    if platform.system() == "Linux":
        import ctypes
        libc = ctypes.CDLL(None, use_errno=True)
        renameat2 = getattr(libc, "renameat2", None)
        if renameat2 is None:
            raise OSError("atomic no-replace rename is unavailable")
        renameat2.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int,
                              ctypes.c_char_p, ctypes.c_uint)
        renameat2.restype = ctypes.c_int
        if renameat2(-100, os.fsencode(source), -100, os.fsencode(destination), 1) != 0:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error), str(destination))
        return
    raise OSError("atomic no-replace rename is unsupported on this platform")


def _fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _fsync_path(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _remove_staging_tree(path: Path) -> None:
    """Remove a private staging tree, making owned readonly directories removable."""
    path = Path(path)
    if not path.exists() and not path.is_symlink():
        return
    root_info = path.lstat()
    if not stat.S_ISDIR(root_info.st_mode) or root_info.st_uid != os.geteuid():
        raise ValueError(f"refusing to remove non-owned staging directory: {path}")
    os.chmod(path, stat.S_IMODE(root_info.st_mode) | stat.S_IWUSR | stat.S_IXUSR)
    with os.scandir(path) as children:
        names = [Path(item.path) for item in children]
    for child in names:
        info = child.lstat()
        if stat.S_ISDIR(info.st_mode):
            _remove_staging_tree(child)
        elif stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode):
            child.unlink()
        else:
            raise ValueError(f"refusing to remove special staging entry: {child}")
    path.rmdir()


def _add_member(tar: tarfile.TarFile, path: Path, entry: dict[str, Any]) -> None:
    info = tarfile.TarInfo(entry["path"])
    info.mode = entry["mode"]
    info.pax_headers = {"dxmt9.mtime_ns": str(entry["mtime_ns"])}
    if entry["kind"] == "directory":
        info.type = tarfile.DIRTYPE
        info.size = 0
        tar.addfile(info)
    else:
        info.type = tarfile.REGTYPE
        info.size = entry["size"]
        with path.open("rb") as stream:
            tar.addfile(info, stream)


def _run_tar_zstd(source: Path, snapshot: dict[str, Any], output: Path, zstd: str) -> None:
    with output.open("wb") as archive_stream:
        process = subprocess.Popen([zstd, "-3", "-T1", "-c"], stdin=subprocess.PIPE,
                                   stdout=archive_stream, stderr=subprocess.PIPE)
        try:
            assert process.stdin is not None
            with tarfile.open(fileobj=process.stdin, mode="w|", format=tarfile.PAX_FORMAT) as tar:
                if snapshot["kind"] == "file":
                    _add_member(tar, source, snapshot["entries"][0])
                else:
                    for entry in snapshot["entries"]:
                        _add_member(tar, source / PurePosixPath(entry["path"]), entry)
            process.stdin.close()
            stderr = process.stderr.read() if process.stderr is not None else b""
            if process.stderr is not None:
                process.stderr.close()
            process.wait()
            if process.returncode != 0:
                raise RuntimeError(f"zstd archive failed: {stderr.decode(errors='replace').strip()}")
        except BaseException:
            if process.poll() is None:
                process.kill()
            process.wait()
            if process.stderr is not None:
                process.stderr.close()
            raise


def _extract_verified(archive: Path, stage: Path, snapshot: dict[str, Any], zstd: str) -> None:
    expected = _validate_snapshot(snapshot)
    info = archive.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_size > _archive_bound(snapshot):
        raise ValueError("archive is not a bounded regular file")
    if not stage.exists():
        stage.mkdir(parents=True, mode=0o700)
    process = subprocess.Popen([zstd, "-d", "-c", str(archive)], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    assert process.stdout is not None
    bounded = _BoundedReader(process.stdout, _archive_bound(snapshot))
    seen: set[str] = set()
    expanded = 0
    try:
        with tarfile.open(fileobj=bounded, mode="r|") as tar:
            for member in tar:
                name = member.name
                path = PurePosixPath(name)
                if (not name or path.is_absolute() or path.as_posix() != name or
                        any(part in ("", ".", "..") for part in name.split("/"))):
                    raise ValueError(f"unsafe archive member path: {name!r}")
                if name in seen:
                    raise ValueError(f"duplicate archive member: {name}")
                seen.add(name)
                entry = expected.get(name)
                if entry is None:
                    raise ValueError(f"unexpected archive member: {name}")
                if member.issym() or member.islnk() or not (member.isdir() or member.isfile()):
                    raise ValueError(f"unsupported archive member type: {name}")
                if member.isdir() != (entry["kind"] == "directory"):
                    raise ValueError(f"archive member type mismatch: {name}")
                if member.mode & 0o7777 != entry["mode"]:
                    raise ValueError(f"archive member mode mismatch: {name}")
                if member.pax_headers.get("dxmt9.mtime_ns") != str(entry["mtime_ns"]):
                    raise ValueError(f"archive member timestamp mismatch: {name}")
                target = stage / path
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=False)
                else:
                    if member.size != entry["size"]:
                        raise ValueError(f"archive member size mismatch: {name}")
                    expanded += member.size
                    if expanded > snapshot["total_bytes"] or expanded > MAX_EXPANDED_BYTES:
                        raise ValueError("archive expansion exceeds expected byte bound")
                    target.parent.mkdir(parents=True, exist_ok=True)
                    source = tar.extractfile(member)
                    if source is None:
                        raise ValueError(f"missing archive data: {name}")
                    digest = hashlib.sha256()
                    size = 0
                    with target.open("xb") as output:
                        while True:
                            block = source.read(CHUNK_BYTES)
                            if not block:
                                break
                            size += len(block)
                            digest.update(block)
                            output.write(block)
                    if size != entry["size"] or digest.hexdigest() != entry["sha256"]:
                        raise ValueError(f"archive member digest mismatch: {name}")
                if entry["kind"] == "file":
                    os.chmod(target, entry["mode"])
                    os.utime(target, ns=(entry["mtime_ns"], entry["mtime_ns"]), follow_symlinks=False)
                    _fsync_path(target)
            # Tar's streaming reader may have read past its first end marker.
            # Permit only zero padding there and drain the rest through the same
            # byte limiter so concatenated archives cannot escape validation.
            tar_buffer = tar.fileobj.buf
            if any(tar_buffer):
                raise ValueError("nonzero data follows tar end marker")
        while True:
            tail = bounded.read(CHUNK_BYTES)
            if not tail:
                break
            if any(tail):
                raise ValueError("nonzero data follows tar end marker")
        process.wait()
        stderr = process.stderr.read() if process.stderr is not None else b""
        if process.stderr is not None:
            process.stderr.close()
        if process.returncode != 0:
            raise ValueError(f"zstd archive is corrupt: {stderr.decode(errors='replace').strip()}")
    except tarfile.ReadError as error:
        if process.poll() is None:
            process.kill()
        process.wait()
        process.stdout.close()
        if process.stderr is not None:
            process.stderr.close()
        raise ValueError(f"archive tar stream is corrupt: {error}") from error
    except BaseException:
        if process.poll() is None:
            process.kill()
        process.wait()
        process.stdout.close()
        if process.stderr is not None:
            process.stderr.close()
        raise
    process.stdout.close()
    if process.stderr is not None:
        process.stderr.close()
    if seen != set(expected):
        missing = sorted(set(expected) - seen)
        raise ValueError(f"archive is missing expected members: {missing[:4]}")
    if expanded != snapshot["total_bytes"]:
        raise ValueError("archive expanded byte count mismatch")
    for name, entry in sorted(expected.items(), key=lambda item: item[0].count("/"), reverse=True):
        if entry["kind"] == "directory":
            target = stage / PurePosixPath(name)
            os.chmod(target, entry["mode"])
            os.utime(target, ns=(entry["mtime_ns"], entry["mtime_ns"]), follow_symlinks=False)
            _fsync_path(target)
    _fsync_path(stage)


def archive_verified(source: str | Path, archive_dir: str | Path,
                     scratch_dir: str | Path, expected_snapshot: dict[str, Any],
                     min_free_bytes: int, zstd: str = "zstd") -> dict[str, Any]:
    source, archive_dir, scratch_dir = map(_safe_path, (Path(source), Path(archive_dir), Path(scratch_dir)))
    _validate_snapshot(expected_snapshot)
    _source_matches(source, expected_snapshot)
    raw_size = expected_snapshot["total_bytes"]
    archive_size_bound = _archive_bound(expected_snapshot)
    scratch_dir.mkdir(parents=True, exist_ok=True)
    archive_dir.mkdir(parents=True, exist_ok=True)
    scratch_dir, archive_dir = _safe_path(scratch_dir), _safe_path(archive_dir)
    scratch_block_size = os.statvfs(scratch_dir).f_frsize or os.statvfs(scratch_dir).f_bsize
    extraction_bound = _restore_allocation_bound(expected_snapshot, scratch_block_size)
    _preflight(scratch_dir, min_free_bytes, extraction_bound)
    _preflight(archive_dir, min_free_bytes, archive_size_bound)
    if os.stat(scratch_dir).st_dev == os.stat(archive_dir).st_dev:
        required = min_free_bytes + extraction_bound + archive_size_bound
        if shutil.disk_usage(scratch_dir).free < required:
            raise OSError("insufficient free space for archive and extraction verification")
    fd, temp_name = tempfile.mkstemp(prefix=".retention-archive-", suffix=".tar.zst", dir=archive_dir)
    os.close(fd)
    temp_archive = Path(temp_name)
    try:
        _run_tar_zstd(source, expected_snapshot, temp_archive, zstd)
        _source_matches(source, expected_snapshot)
        if temp_archive.stat().st_size > _archive_bound(expected_snapshot):
            raise RuntimeError("compressed archive exceeds its byte bound")
        with temp_archive.open("rb") as stream:
            os.fsync(stream.fileno())
        with temp_archive.open("rb") as stream:
            archive_hash = hashlib.sha256()
            while block := stream.read(CHUNK_BYTES):
                archive_hash.update(block)
        archive_digest = archive_hash.hexdigest()
        _extract_verified(temp_archive, scratch_dir / (temp_archive.name + ".verify"),
                          expected_snapshot, zstd)
        # Verification staging is discarded here; its existence and contents were bounded.
        _remove_staging_tree(scratch_dir / (temp_archive.name + ".verify"))
        final = archive_dir / f"{archive_digest}.tar.zst"
        if final.exists():
            digest, _ = _file_digest(final)
            if digest != archive_digest:
                raise FileExistsError(f"archive digest path collision: {final}")
            if final.is_symlink():
                raise ValueError(f"symlink at archive digest path: {final}")
            temp_archive.unlink()
        else:
            _rename_noreplace(temp_archive, final)
            _fsync_directory(archive_dir)
        return {"archive_path": str(final), "sha256": archive_digest,
                "size": final.stat().st_size, "contents": expected_snapshot}
    finally:
        if temp_archive.exists():
            temp_archive.unlink()
        verify_stage = scratch_dir / (temp_archive.name + ".verify")
        if verify_stage.exists():
            _remove_staging_tree(verify_stage)


def restore_verified(archive: str | Path, expected_snapshot: dict[str, Any],
                     destination: str | Path, min_free_bytes: int,
                     zstd: str = "zstd") -> dict[str, Any]:
    archive, destination = _safe_path(Path(archive)), _safe_path(Path(destination))
    _validate_snapshot(expected_snapshot)
    if destination.exists() or destination.is_symlink():
        raise FileExistsError(f"restore destination already exists: {destination}")
    parent = destination.parent
    parent.mkdir(parents=True, exist_ok=True)
    parent = _safe_path(parent)
    fs = os.statvfs(parent)
    allocation = _restore_allocation_bound(expected_snapshot, fs.f_frsize or fs.f_bsize)
    _preflight(parent, min_free_bytes, allocation)
    fd, stage_name = tempfile.mkstemp(prefix=".retention-restore-", dir=parent)
    os.close(fd)
    stage = Path(stage_name)
    stage.unlink()
    stage_payload = stage if expected_snapshot["kind"] == "directory" else stage.with_suffix(".file")
    stage_payload.mkdir(mode=0o700) if expected_snapshot["kind"] == "directory" else None
    try:
        if expected_snapshot["kind"] == "file":
            stage_payload.mkdir(mode=0o700)
            _extract_verified(archive, stage_payload, expected_snapshot, zstd)
            restored_file = stage_payload / expected_snapshot["entries"][0]["path"]
            os.replace(restored_file, stage)
            _fsync_directory(stage_payload)
            stage_payload.rmdir()
        else:
            _extract_verified(archive, stage_payload, expected_snapshot, zstd)
            os.chmod(stage_payload, expected_snapshot["root_mode"])
            os.utime(stage_payload, ns=(expected_snapshot["root_mtime_ns"],
                                       expected_snapshot["root_mtime_ns"]))
            _fsync_path(stage_payload)
            os.replace(stage_payload, stage)
        # Root directory identity changes across restore; member content identity is exact.
        if expected_snapshot["kind"] == "file":
            actual = snapshot_tree(stage)
            if actual["tree_digest"] != expected_snapshot["tree_digest"]:
                raise ValueError("restored file digest mismatch")
        else:
            actual = snapshot_tree(stage)
            if actual["tree_digest"] != expected_snapshot["tree_digest"]:
                raise ValueError("restored tree digest mismatch")
        if destination.exists() or destination.is_symlink():
            raise FileExistsError(f"restore destination appeared during restore: {destination}")
        _rename_noreplace(stage, destination)
        _fsync_directory(parent)
        return {"destination": str(destination), "tree_digest": actual["tree_digest"],
                "total_bytes": actual["total_bytes"], "verified": True}
    finally:
        if stage.is_dir():
            _remove_staging_tree(stage)
        elif stage.exists():
            stage.unlink()
        if stage_payload != stage and stage_payload.exists():
            _remove_staging_tree(stage_payload)
