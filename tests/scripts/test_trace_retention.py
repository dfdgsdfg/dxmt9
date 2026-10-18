"""R-HARN-TRACE-18: fail-closed selection, frozen plans and recoverable eviction."""
from datetime import datetime, timedelta, timezone
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts.tools import retain_trace_artifacts as retention

ZSTD = shutil.which('zstd')


class RetentionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name).resolve()
        self.path = self.repo / 'traces/run-unique/bulk.csv'
        self.path.parent.mkdir(parents=True)
        self.path.write_text('frame,cpu\n1,123\n' * 2000)
        self.evidence = self.path.parent / 'completion.json'
        self.evidence.write_text('{"closed":true,"returncode":0}')
        self.relative = self.path.relative_to(self.repo).as_posix()
        old = (datetime.now(timezone.utc) - timedelta(days=100)).isoformat()
        self.review = {'owner_closed': True, 'recovery_resolved': True,
                       'reviewer': 'fixture', 'reason': 'immutable stopped fixture',
                       'completed_at': old, 'last_used_at': old, 'outcome': 'passed',
                       'reference_scope_complete': True,
                       'evidence': [{'path': self.evidence.relative_to(self.repo).as_posix(),
                                     'sha256': retention.sha256(self.evidence)}]}
        self.handles = patch.object(retention, 'open_handles', return_value=[])
        self.handles.start()
        self.addCleanup(self.handles.stop)

    def assess(self, action='archive', review=None):
        return retention.assess(self.repo, self.relative, action,
                                self.review if review is None else review)

    def plan(self, action='archive', review=None):
        plan = retention.make_plan(self.repo, [self.relative], action,
            {self.relative: self.review if review is None else review}, 0, 'small test fixture')
        return retention.save_plan(self.repo, plan)

    def test_missing_lifecycle_blocks_without_hashing(self):
        with patch.object(retention, 'snapshot_tree', side_effect=AssertionError('must not hash')):
            self.assertFalse(self.assess(review={})['eligible'])

    def test_review_uses_later_explicit_time_not_mtime(self):
        self.review['last_used_at'] = datetime.now(timezone.utc).isoformat()
        self.assertFalse(self.assess()['eligible'])
        self.assertTrue(self.assess('archive-copy')['eligible'])
        self.review['last_used_at'] = '2026-01-01'
        self.assertFalse(self.assess()['eligible'])

    def test_failed_retention_and_recovery(self):
        self.review['outcome'] = 'failed'
        self.assertFalse(self.assess()['eligible'])
        self.review['issue_resolved'] = True
        self.assertTrue(self.assess()['eligible'])
        self.review['recovery_resolved'] = False
        self.assertFalse(self.assess()['eligible'])

    def test_untracked_ancestor_path_and_digest_references_protect(self):
        document = self.repo / 'docs/current.md'
        document.parent.mkdir()
        for text in ['traces/run-unique/', retention.sha256(self.path)]:
            document.write_text(text)
            self.assertFalse(self.assess()['eligible'])
            self.assertTrue(self.assess('archive-copy')['eligible'])

    def test_indirect_metadata_and_unknown_graph_block(self):
        manifest = self.repo / 'experiments/output/analysis/manifest.json'
        manifest.parent.mkdir(parents=True)
        manifest.write_text(json.dumps({'input': retention.sha256(self.path)}))
        result = self.assess()
        self.assertIn('experiments/output/analysis/manifest.json', result['reference_scan']['references'])
        manifest.write_text('{broken')
        self.assertTrue(self.assess()['reference_scan']['unknown'])
        manifest.unlink()
        opaque = manifest.with_suffix('.duckdb')
        opaque.write_bytes(b'opaque')
        self.assertFalse(self.assess()['eligible'])

    def test_root_document_and_retained_dependency_protect(self):
        doc = self.repo / 'README.md'
        doc.write_text(self.relative)
        self.assertFalse(self.assess()['eligible'])
        doc.unlink()
        operation = retention.store_path(self.repo) / 'operations/prior'
        operation.mkdir(parents=True)
        (operation / 'contents.json').write_text(json.dumps({'sources': [{
            'review_evidence': [{'path': self.relative, 'sha256': retention.sha256(self.path)}]}]}))
        self.assertFalse(self.assess()['eligible'])

    def test_pins_wal_open_owners_and_invalid_pin_registry(self):
        store = retention.store_path(self.repo)
        store.mkdir(parents=True)
        pins = store / 'pins.json'
        pins.write_text(json.dumps({'pins': [{'path': 'traces/run-unique', 'reason': 'baseline'}]}))
        self.assertFalse(self.assess()['eligible'])
        self.assertTrue(self.assess('archive-copy')['eligible'])
        pins.unlink()
        wal = Path(str(self.path) + '-wal')
        wal.touch()
        self.assertFalse(self.assess()['eligible'])
        wal.unlink()
        with patch.object(retention, 'open_handles', return_value=['123']):
            self.assertFalse(self.assess()['eligible'])
        pins.write_text('{"pins":[{}]}')
        with self.assertRaises(ValueError):
            self.assess()

    def test_scope_symlink_hardlink_metadata_and_directory_guards(self):
        for value in ['traces/../outside', 'docs/file.csv', 'experiments/output/_retention/plan.json']:
            with self.assertRaises(ValueError):
                retention.source_path(self.repo, value)
        linked = self.path.parent / 'link.csv'
        linked.symlink_to(self.path)
        with self.assertRaises(ValueError):
            retention.source_path(self.repo, str(linked))
        linked.unlink()
        import os
        os.link(self.path, linked)
        self.assertFalse(self.assess()['eligible'])
        linked.unlink()
        self.assertFalse(retention.assess(self.repo, str(self.evidence), 'delete', self.review)['eligible'])
        self.assertFalse(retention.assess(self.repo, str(self.path.parent), 'delete', self.review)['eligible'])

    def test_stale_source_and_lifecycle_never_delete(self):
        plan = self.plan('delete')
        self.path.write_text('changed')
        with self.assertRaisesRegex(ValueError, 'stale plan'):
            retention.apply_plan(self.repo, plan)
        self.assertTrue(self.path.exists())
        self.assertEqual(retention.read_json(plan.parent / 'receipt.json')['status'], 'interrupted')
        with self.assertRaisesRegex(ValueError, 'already attempted'):
            retention.apply_plan(self.repo, plan)
        plan = self.plan('delete')
        self.evidence.write_text('{"closed":false}')
        with self.assertRaisesRegex(ValueError, 'stale plan'):
            retention.apply_plan(self.repo, plan)
        self.assertTrue(self.path.exists())

    def test_edited_plan_cannot_drop_retained_evidence(self):
        plan = self.plan('delete')
        data = retention.read_json(plan)
        data['entries'][0]['evidence'] = []
        retention.write_json(plan, data)
        with self.assertRaisesRegex(ValueError, 'stale plan'):
            retention.apply_plan(self.repo, plan)
        self.assertTrue(self.path.exists())

    def test_new_reference_after_plan_prevents_delete(self):
        plan = self.plan('delete')
        (self.repo / 'docs').mkdir()
        (self.repo / 'docs/new.md').write_text(self.relative)
        with self.assertRaisesRegex(ValueError, 'stale plan'):
            retention.apply_plan(self.repo, plan)
        self.assertTrue(self.path.exists())

    def test_explicit_deletion_retains_metadata_and_tombstone(self):
        original = self.path.read_bytes()
        receipt = retention.apply_plan(self.repo, self.plan('delete'))
        self.assertFalse(self.path.exists())
        self.assertTrue(self.evidence.exists())
        self.assertEqual(receipt['actions'][0]['availability'], 'deleted')
        self.assertEqual(receipt['actions'][0]['logical_bytes_removed'], len(original))
        self.assertEqual(receipt['actions'][0]['phase'], 'complete')

    @unittest.skipUnless(ZSTD, 'zstd required')
    def test_archive_eviction_restore_exact_and_existing_destination_refused(self):
        original = self.path.read_bytes()
        plan = self.plan()
        receipt = retention.apply_plan(self.repo, plan, ZSTD)
        self.assertFalse(self.path.exists())
        self.assertEqual(receipt['actions'][0]['availability'], 'archived')
        self.assertLess(receipt['actions'][0]['archive']['size'], len(original))
        retention.restore(self.repo, plan.parent / 'receipt.json', self.path, ZSTD)
        self.assertEqual(self.path.read_bytes(), original)
        with self.assertRaises(FileExistsError):
            retention.restore(self.repo, plan.parent / 'receipt.json', self.path, ZSTD)
        self.assertTrue(self.evidence.exists())

    @unittest.skipUnless(ZSTD, 'zstd required')
    def test_archive_failure_and_post_archive_reference_leave_source(self):
        plan = self.plan()
        with patch.object(retention, 'archive_verified', side_effect=ValueError('corrupt archive')):
            with self.assertRaisesRegex(ValueError, 'corrupt archive'):
                retention.apply_plan(self.repo, plan, ZSTD)
        self.assertTrue(self.path.exists())
        plan = self.plan()
        real_archive = retention.archive_verified
        def archive_and_reference(*args, **kwargs):
            value = real_archive(*args, **kwargs)
            (self.repo / 'docs').mkdir()
            (self.repo / 'docs/appeared.md').write_text(self.relative)
            return value
        with patch.object(retention, 'archive_verified', side_effect=archive_and_reference):
            with self.assertRaisesRegex(ValueError, 'changed before eviction'):
                retention.apply_plan(self.repo, plan, ZSTD)
        self.assertTrue(self.path.exists())

    def test_interrupted_unlink_restores_source_and_journals_intent(self):
        plan = self.plan('delete')
        unlink = Path.unlink
        def interrupted(path, *args, **kwargs):
            if '.retention-' in path.name:
                raise OSError('simulated unlink interruption')
            return unlink(path, *args, **kwargs)
        with patch.object(Path, 'unlink', interrupted):
            with self.assertRaisesRegex(OSError, 'simulated unlink'):
                retention.apply_plan(self.repo, plan)
        self.assertTrue(self.path.exists())
        receipt = retention.read_json(plan.parent / 'receipt.json')
        self.assertEqual(receipt['status'], 'interrupted')
        self.assertEqual(receipt['actions'][0]['phase'], 'unlink-intent')

    def test_test_source_reference_protects_artifact(self):
        caller = self.repo / 'tests/consumer.py'
        caller.parent.mkdir()
        caller.write_text('SOURCE = ' + repr(self.relative))
        self.assertFalse(self.assess()['eligible'])

    def test_post_unlink_sync_failure_records_actual_availability(self):
        plan = self.plan('delete')
        real_sync = retention.sync_dir
        count = 0
        def failed_sync(path):
            nonlocal count
            if path == self.path.parent:
                count += 1
                if count == 2:
                    raise OSError('directory sync failed after unlink')
            return real_sync(path)
        with patch.object(retention, 'sync_dir', side_effect=failed_sync):
            with self.assertRaisesRegex(OSError, 'after unlink'):
                retention.apply_plan(self.repo, plan)
        self.assertFalse(self.path.exists())
        receipt = retention.read_json(plan.parent / 'receipt.json')
        self.assertEqual(receipt['status'], 'interrupted')
        self.assertEqual(receipt['actions'][0]['availability'], 'deleted')
        self.assertEqual(receipt['actions'][0]['phase'], 'unlinked-awaiting-directory-sync')

    def test_selected_dependency_blocks_both_candidates(self):
        dependency = self.path.parent / 'input.bin'
        dependency.write_bytes(b'input')
        dependent_review = dict(self.review, retained_inputs=[{
            'path': dependency.relative_to(self.repo).as_posix(),
            'sha256': retention.sha256(dependency)}], class_='derived')
        dependent_review['class'] = 'derived'
        dependent_review['verified_outputs'] = self.review['evidence']
        dependent_review['regeneration'] = 'verified fixture recipe'
        dependency_review = dict(self.review, **{'class': 'scratch'},
            retained_inputs=self.review['evidence'], verified_outputs=self.review['evidence'])
        dep_relative = dependency.relative_to(self.repo).as_posix()
        plan = retention.make_plan(self.repo, [self.relative, dep_relative], 'prune',
            {self.relative: dependent_review, dep_relative: dependency_review}, 0, 'fixture')
        self.assertFalse(any(e['eligible'] for e in plan['entries']))
        self.assertTrue(self.path.exists())
        self.assertTrue(dependency.exists())

    def test_reference_attestation_never_overrides_unknown_inputs(self):
        self.review['reference_scope_complete'] = False
        self.assertFalse(self.assess()['eligible'])
        self.review['reference_scope_complete'] = True
        (self.path.parent / 'opaque.sqlite').write_bytes(b'opaque')
        self.assertFalse(self.assess()['eligible'])

    def test_budget_override_and_mutation_lease(self):
        with self.assertRaisesRegex(ValueError, 'budget-note'):
            retention.budget(0, '')
        with retention.lease(self.repo):
            with self.assertRaisesRegex(ValueError, 'lease'):
                with retention.lease(self.repo):
                    self.fail('lease must exclude another writer')

    def test_prune_needs_retained_inputs_outputs_and_regeneration(self):
        self.review['class'] = 'derived'
        self.assertFalse(self.assess('prune')['eligible'])
        record = self.review['evidence'][0]
        self.review.update(verified_outputs=[record], retained_inputs=[record],
                           regeneration='fixture regenerated byte-identically; tool/version recorded in completion')
        self.assertTrue(self.assess('prune')['eligible'])


if __name__ == '__main__':
    unittest.main()
