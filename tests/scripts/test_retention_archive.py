"""Safety checks for verified retention archive and restore primitives."""
from pathlib import Path
import io
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts.tools.retention_archive import archive_verified, restore_verified, snapshot_tree
import scripts.tools.retention_archive as retention_archive


ZSTD = "zstd"


def tar_bytes(members: list[tuple[str, bytes, bytes]], mtime_ns: str = "0") -> bytes:
    tar_buffer = io.BytesIO()
    with tarfile.open(fileobj=tar_buffer, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for name, data, kind in members:
            info = tarfile.TarInfo(name)
            info.type = kind
            info.size = len(data) if kind == tarfile.REGTYPE else 0
            info.mode = 0o644
            info.pax_headers = {"dxmt9.mtime_ns": mtime_ns}
            tar.addfile(info, io.BytesIO(data) if info.size else None)
    return tar_buffer.getvalue()


def malicious_archive(path: Path, members: list[tuple[str, bytes, bytes]], mtime_ns: str = "0",
                      trailing: bytes = b"") -> None:
    """Build arbitrary tar members and optional post-end-marker bytes."""
    result = subprocess.run([ZSTD, "-3", "-T1", "-c"],
                            input=tar_bytes(members, mtime_ns) + trailing,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(result.stderr.decode())
    path.write_bytes(result.stdout)


class RetentionArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.scratch = self.root / "scratch"
        self.archives = self.root / "archives"

    def archive(self, source: Path):
        return archive_verified(source, self.archives, self.scratch,
                                snapshot_tree(source), 0, ZSTD)

    def test_file_archive_restore_preserves_bytes_and_metadata(self):
        source = self.root / "cold.log"
        source.write_bytes(b"raw evidence\x00\xff")
        os.chmod(source, 0o640)
        os.utime(source, ns=(1_700_000_000_123_456_789,) * 2)
        expected = snapshot_tree(source)
        result = archive_verified(source, self.archives, self.scratch, expected, 0, ZSTD)
        restored = self.root / "restored.log"
        proof = restore_verified(result["archive_path"], expected, restored, 0, ZSTD)
        self.assertTrue(proof["verified"])
        self.assertEqual(restored.read_bytes(), source.read_bytes())
        self.assertEqual(restored.stat().st_mode & 0o777, 0o640)
        self.assertEqual(restored.stat().st_mtime_ns, expected["entries"][0]["mtime_ns"])
        self.assertTrue(source.exists())

    def test_directory_archive_restore_includes_empty_directories(self):
        source = self.root / "bundle"
        (source / "nested" / "empty").mkdir(parents=True)
        payload = source / "nested" / "item.bin"
        payload.write_bytes(b"member")
        expected = snapshot_tree(source)
        result = self.archive(source)
        destination = self.root / "restored-bundle"
        restore_verified(result["archive_path"], expected, destination, 0, ZSTD)
        self.assertEqual(snapshot_tree(destination)["tree_digest"], expected["tree_digest"])
        self.assertTrue((destination / "nested" / "empty").is_dir())

    def test_changed_source_is_rejected_and_never_removed(self):
        source = self.root / "changed.txt"
        source.write_text("original")
        expected = snapshot_tree(source)
        source.write_text("changed")
        with self.assertRaisesRegex(ValueError, "differs from expected"):
            archive_verified(source, self.archives, self.scratch, expected, 0, ZSTD)
        self.assertEqual(source.read_text(), "changed")
        self.assertFalse(self.archives.exists())

    def test_corrupt_archive_and_existing_destination_refused(self):
        source = self.root / "item"
        source.write_bytes(b"bytes")
        expected = snapshot_tree(source)
        result = self.archive(source)
        archive = Path(result["archive_path"])
        data = bytearray(archive.read_bytes())
        data[-1] ^= 0x80
        archive.write_bytes(data)
        destination = self.root / "out"
        with self.assertRaises((ValueError, RuntimeError)):
            restore_verified(archive, expected, destination, 0, ZSTD)
        self.assertFalse(destination.exists())
        destination.write_text("keep")
        with self.assertRaises(FileExistsError):
            restore_verified(archive, expected, destination, 0, ZSTD)
        self.assertEqual(destination.read_text(), "keep")

    def test_duplicate_and_traversal_members_rejected(self):
        source = self.root / "expected"
        source.write_bytes(b"ok")
        expected = snapshot_tree(source)
        for index, names in enumerate((("expected", "expected"), ("../escape",))):
            archive = self.root / f"bad-{index}.tar.zst"
            malicious_archive(archive, [(name, b"ok", tarfile.REGTYPE) for name in names])
            with self.subTest(names=names), self.assertRaises(ValueError):
                restore_verified(archive, expected, self.root / f"out-{index}", 0, ZSTD)
            self.assertFalse((self.root / "escape").exists())

    def test_expansion_beyond_manifest_rejected(self):
        source = self.root / "small"
        source.write_bytes(b"ok")
        expected = snapshot_tree(source)
        archive = self.root / "expanded.tar.zst"
        malicious_archive(archive, [("small", b"x" * (len(b"ok") + 1), tarfile.REGTYPE)],
                          str(expected["entries"][0]["mtime_ns"]))
        with self.assertRaisesRegex(ValueError, "size mismatch"):
            restore_verified(archive, expected, self.root / "expanded", 0, ZSTD)
        self.assertFalse((self.root / "expanded").exists())

    def test_concatenated_tar_member_after_end_marker_rejected(self):
        source = self.root / "small-concat"
        source.write_bytes(b"ok")
        expected = snapshot_tree(source)
        first = tar_bytes([("small-concat", b"ok", tarfile.REGTYPE)],
                          str(expected["entries"][0]["mtime_ns"]))
        second = tar_bytes([("extra", b"escape", tarfile.REGTYPE)])
        archive = self.root / "concatenated.tar.zst"
        packed_frames = []
        for tar_stream in (first, second):
            packed = subprocess.run([ZSTD, "-3", "-T1", "-c"], input=tar_stream,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            packed_frames.append(packed.stdout)
        archive.write_bytes(b"".join(packed_frames))
        destination = self.root / "concat-out"
        with self.assertRaisesRegex(ValueError, "nonzero data follows"):
            restore_verified(archive, expected, destination, 0, ZSTD)
        self.assertFalse(destination.exists())

    def test_huge_compressed_zero_tail_is_bounded(self):
        source = self.root / "small-tail"
        source.write_bytes(b"ok")
        expected = snapshot_tree(source)
        archive = self.root / "huge-tail.tar.zst"
        malicious_archive(archive, [("small-tail", b"ok", tarfile.REGTYPE)],
                          str(expected["entries"][0]["mtime_ns"]), trailing=b"\0" * (3 * 1024 * 1024))
        destination = self.root / "tail-out"
        with self.assertRaisesRegex(ValueError, "byte bound"):
            restore_verified(archive, expected, destination, 0, ZSTD)
        self.assertFalse(destination.exists())

    def test_preflight_accounts_for_tiny_file_allocation_overhead(self):
        source = self.root / "many-tiny"
        source.mkdir()
        for index in range(12):
            (source / f"f{index}").write_bytes(b"x")
        expected = snapshot_tree(source)
        self.assertGreater(retention_archive._restore_allocation_bound(expected, 4096),
                           expected["total_bytes"])
        with patch.object(retention_archive.shutil, "disk_usage",
                          return_value=SimpleNamespace(free=expected["total_bytes"])):
            with self.assertRaisesRegex(OSError, "insufficient free space"):
                restore_verified(self.root / "unused.tar.zst", expected,
                                 self.root / "budget-out", 0, ZSTD)

    def test_restore_fsyncs_extracted_tree_before_atomic_publish(self):
        source = self.root / "sync-source"
        (source / "nested").mkdir(parents=True)
        (source / "nested" / "bytes").write_bytes(b"durable")
        expected = snapshot_tree(source)
        archive = self.archive(source)
        destination = self.root / "sync-restored"
        fsynced = []
        published = []
        original_fsync_path = retention_archive._fsync_path
        original_rename = retention_archive._rename_noreplace

        def note_fsync(path):
            fsynced.append(Path(path))
            original_fsync_path(Path(path))

        def note_publish(src, dst):
            published.append(Path(dst))
            self.assertTrue(any(path.name == "bytes" for path in fsynced))
            original_rename(src, dst)

        with patch.object(retention_archive, "_fsync_path", side_effect=note_fsync), \
             patch.object(retention_archive, "_rename_noreplace", side_effect=note_publish):
            restore_verified(archive["archive_path"], expected, destination, 0, ZSTD)
        self.assertEqual(published, [destination])
        self.assertTrue(any(path.name == "nested" for path in fsynced))
        self.assertTrue(any(path.name.startswith(".retention-restore-") for path in fsynced))

    def test_restore_populates_read_only_nested_directory_before_metadata(self):
        source = self.root / "readonly-source"
        nested = source / "readonly"
        nested.mkdir(parents=True)
        (nested / "bytes").write_bytes(b"content")
        os.chmod(nested, 0o500)
        self.addCleanup(lambda: os.chmod(nested, 0o700) if nested.exists() else None)
        expected = snapshot_tree(source)
        archive = self.archive(source)
        self.assertEqual(list(self.scratch.iterdir()), [])
        destination = self.root / "readonly-restored"
        restore_verified(archive["archive_path"], expected, destination, 0, ZSTD)
        self.assertEqual((destination / "readonly" / "bytes").read_bytes(), b"content")
        self.assertEqual((destination / "readonly").stat().st_mode & 0o777, 0o500)
        self.assertEqual([p.name for p in self.root.iterdir() if p.name.startswith(".retention-restore-")], [])

    def test_failed_compressor_never_publishes_archive(self):
        source = self.root / "payload"
        source.write_bytes(b"keep me")
        fake_zstd = self.root / "broken-zstd"
        fake_zstd.write_text("#!/bin/sh\ncat >/dev/null\nprintf partial\nexit 7\n")
        fake_zstd.chmod(0o755)
        with self.assertRaisesRegex(RuntimeError, "zstd archive failed"):
            archive_verified(source, self.archives, self.scratch,
                             snapshot_tree(source), 0, str(fake_zstd))
        self.assertEqual(source.read_bytes(), b"keep me")
        self.assertEqual(list(self.archives.glob("*.tar.zst")), [])
        self.assertEqual(list(self.scratch.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
