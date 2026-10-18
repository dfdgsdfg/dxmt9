"""Tests for fail-closed, archive-aware retention reads."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.tools import retention_archive, retention_resolver


class RetentionResolverTests(unittest.TestCase):
    def setUp(self):
        if not shutil.which("zstd"):
            self.skipTest("zstd is not available")
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        self.source = self.repo / "traces" / "capture.csv"
        self.source.parent.mkdir()
        self.source.write_bytes(b"header\n1,2\n")
        self.store = self.repo / "experiments" / "output" / "_retention"
        self.store.mkdir(parents=True)

    def create_operation(self, operation_id="op1", *, action="archive", receipt_status="complete",
                         action_status="complete", phase="complete", availability="archived",
                         floor=0, qualified=False, proof_state="valid"):
        operation = self.store / "operations" / operation_id
        operation.mkdir(parents=True)
        snapshot = retention_archive.snapshot_tree(self.source)
        qualification = None
        if qualified:
            proof_path = operation / "reader-qualification-test.json"
            proof = {"schema": retention_resolver.READER_QUALIFICATION_SCHEMA,
                     "source": self.source.relative_to(self.repo).as_posix(),
                     "source_bytes": snapshot["total_bytes"],
                     "source_sha256": snapshot["tree_digest"],
                     "archive_sha256": None,
                     "contract": "cpu-supply-v2", "qualified": True,
                     "original_reader_sha256": "a" * 64,
                     "restored_reader_sha256": "a" * 64}
        plan = {
            "schema": "dxmt9.retention.v1",
            "repo": str(self.repo),
            "policy": {"free_space_floor_bytes": floor, "override_reason": None},
            "entries": [{"path": self.source.relative_to(self.repo).as_posix(),
                         "action": action, "snapshot": snapshot}],
        }
        archives = self.store / "archives"
        archived = retention_archive.archive_verified(
            self.source, archives, operation, snapshot, 0, shutil.which("zstd"))
        if qualified:
            proof["archive_sha256"] = archived["sha256"]
            if proof_state == "corrupt":
                proof["source_sha256"] = "0" * 64
            proof_path.write_text(json.dumps(proof, sort_keys=True) + "\n")
            qualification = {"path": proof_path.relative_to(self.repo).as_posix(),
                             "sha256": hashlib.sha256(proof_path.read_bytes()).hexdigest(),
                             "contract": "cpu-supply-v2", "archive_sha256": archived["sha256"]}
            if proof_state == "missing":
                proof_path.unlink()
            plan["entries"][0]["reader_qualification"] = qualification
            plan["entries"][0]["review"] = {"reader_contract": "cpu-supply-v2"}
        plan_path = operation / "plan.json"
        plan_path.write_text(json.dumps(plan, sort_keys=True, indent=2) + "\n")
        contents = {"schema": "dxmt9.retention.v1", "sources": [
            {"path": plan["entries"][0]["path"], "snapshot": snapshot}
        ]}
        (operation / "contents.json").write_text(json.dumps(contents, sort_keys=True) + "\n")
        record = {"path": plan["entries"][0]["path"], "action": action,
                  "status": action_status, "phase": phase, "availability": availability,
                  "archive": archived}
        if qualification is not None:
            record["reader_qualification"] = qualification
        receipt = {"schema": "dxmt9.retention.v1",
                   "plan_sha256": hashlib.sha256(plan_path.read_bytes()).hexdigest(),
                   "policy": plan["policy"], "status": receipt_status, "actions": [record]}
        (operation / "receipt.json").write_text(json.dumps(receipt, sort_keys=True) + "\n")
        return operation, plan_path, record, receipt

    def test_missing_logical_path_resolves_to_verified_temporary_copy(self):
        self.create_operation()
        expected = self.source.read_bytes()
        self.source.unlink()
        with retention_resolver.resolved_artifact(self.source, repo=self.repo) as physical:
            self.assertEqual(physical.read_bytes(), expected)
            self.assertNotEqual(physical, self.source)
            self.assertTrue(physical.exists())
        self.assertFalse(self.source.exists())
        self.assertEqual(list((self.store / "reader-staging").iterdir()), [])

    def test_successful_deletion_tombstone_does_not_resurrect_old_archive(self):
        self.create_operation('old-archive')
        self.source.write_bytes(b'new generation')
        operation = self.store / 'operations/new-delete'
        operation.mkdir()
        (operation / 'receipt.json').write_text(json.dumps({
            'schema': 'dxmt9.retention.v1', 'status': 'complete', 'actions': [{
                'path': self.source.relative_to(self.repo).as_posix(), 'action': 'delete',
                'status': 'complete', 'phase': 'complete', 'availability': 'deleted'}]}))
        self.source.unlink()
        with self.assertRaisesRegex(ValueError, 'tombstoned'):
            retention_resolver.read_bytes(self.source, repo=self.repo)

    def test_archive_copy_is_not_permission_to_resolve_missing_source(self):
        self.create_operation(action="archive-copy", availability="materialized")
        self.source.unlink()
        with self.assertRaises(FileNotFoundError):
            retention_resolver.read_bytes(self.source, repo=self.repo)

    def test_corrupt_archive_receipt_plan_and_snapshot_bindings_fail_closed(self):
        cases = ("archive", "receipt", "plan", "binding")
        for case in cases:
            with self.subTest(case=case):
                operation, plan_path, action, receipt = self.create_operation("op-" + case)
                archive_path = Path(action["archive"]["archive_path"])
                if case == "archive":
                    archive_path.write_bytes(b"corrupt")
                elif case == "receipt":
                    (operation / "receipt.json").write_text("{")
                elif case == "plan":
                    plan_path.write_text(plan_path.read_text() + " ")
                else:
                    action["archive"]["contents"]["tree_digest"] = "0" * 64
                    (operation / "receipt.json").write_text(json.dumps(receipt))
                with self.assertRaises((ValueError, json.JSONDecodeError, FileNotFoundError)):
                    retention_resolver._matching_record(
                        self.source, self.repo, self.store)
                archive_path.unlink(missing_ok=True)
                shutil.rmtree(operation)

    def test_ambiguous_archived_identities_require_digest_disambiguation(self):
        first, *_ = self.create_operation("first")
        original = self.source.read_bytes()
        self.source.write_bytes(b"different bytes\n")
        self.create_operation("second")
        self.source.unlink()
        with self.assertRaisesRegex(ValueError, "ambiguous archived identities"):
            retention_resolver.read_bytes(self.source, repo=self.repo)
        with retention_resolver.resolved_artifact(
                self.source, repo=self.repo,
                expected_sha256=hashlib.sha256(original).hexdigest()) as resolved:
            self.assertEqual(resolved.read_bytes(), original)
        shutil.rmtree(first)

    def test_expected_digest_is_checked_for_materialized_and_archived_sources(self):
        with self.assertRaisesRegex(ValueError, "source digest mismatch"):
            with retention_resolver.resolved_artifact(
                    self.source, repo=self.repo, expected_sha256="0" * 64):
                pass
        expected = self.source.read_bytes()
        self.create_operation()
        self.source.unlink()
        with retention_resolver.resolved_artifact(
                self.source, repo=self.repo,
                expected_sha256=hashlib.sha256(expected).hexdigest()) as resolved:
            self.assertEqual(resolved.read_bytes(), expected)

    def test_shared_reader_lease_conflicts_with_mutation_lease(self):
        lock_path = self.store / "mutation.lock"
        with lock_path.open("a+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                with self.assertRaisesRegex(ValueError, "mutation holds the lease"):
                    retention_resolver.verify_available(self.source, repo=self.repo)
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)

    def test_directory_audit_verifies_missing_archived_descendants(self):
        directory = self.repo / "traces" / "bundle"
        directory.mkdir()
        self.source = directory / "child.csv"
        self.source.write_bytes(b"child\n")
        self.create_operation()
        self.source.unlink()
        self.assertEqual(retention_resolver.verify_available(directory, repo=self.repo), 1)
        self.assertFalse(self.source.exists())
        self.assertEqual(list((self.store / "reader-staging").iterdir()), [])

    def test_frozen_capacity_floor_is_enforced_and_staging_is_cleaned(self):
        self.create_operation(floor=1 << 60)
        self.source.unlink()
        with patch.object(retention_archive.shutil, "disk_usage",
                          return_value=type("Usage", (), {"free": 0})()):
            with self.assertRaisesRegex(OSError, "insufficient free space"):
                retention_resolver.read_bytes(self.source, repo=self.repo)
        self.assertEqual(list((self.store / "reader-staging").iterdir()), [])

    def test_qualified_archive_requires_intact_bound_parity_proof(self):
        for state in ("missing", "corrupt"):
            with self.subTest(state=state):
                self.create_operation("qualified-" + state, qualified=True, proof_state=state)
                self.source.unlink()
                with self.assertRaises(ValueError):
                    retention_resolver.read_bytes(self.source, repo=self.repo)
                self.source.write_bytes(b"header\n1,2\n")
                shutil.rmtree(self.store / "operations" / ("qualified-" + state))

    def test_directory_audit_rejects_archived_child_replaced_by_directory_or_symlink(self):
        for replacement in ("directory", "symlink"):
            with self.subTest(replacement=replacement):
                directory = self.repo / "traces" / replacement
                directory.mkdir()
                self.source = directory / "child.csv"
                self.source.write_bytes(b"child\n")
                self.create_operation("replacement-" + replacement)
                self.source.unlink()
                if replacement == "directory":
                    self.source.mkdir()
                else:
                    self.source.symlink_to(directory / "target.csv")
                with self.assertRaisesRegex(ValueError, "symlink|regular file"):
                    retention_resolver.verify_available(directory, repo=self.repo)
                if self.source.is_symlink():
                    self.source.unlink()
                elif self.source.is_dir():
                    self.source.rmdir()
                shutil.rmtree(self.store / "operations" / ("replacement-" + replacement))

    def test_relative_reader_path_defaults_to_current_working_directory(self):
        other = Path(self.temp.name) / "reader-cwd"
        other.mkdir()
        target = other / "local.csv"
        target.write_bytes(b"local input\n")
        previous = Path.cwd()
        try:
            os.chdir(other)
            self.assertEqual(retention_resolver.read_bytes("local.csv"), b"local input\n")
        finally:
            os.chdir(previous)


if __name__ == "__main__":
    unittest.main()
