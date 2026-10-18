"""R-HARN-TRACE-18.6/18.7: qualified referenced archival stays readable."""
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
sys.path.insert(0, str(ROOT / 'scripts/tools'))
from scripts.tools import retain_trace_artifacts as retention
from scripts.tools import retention_readers as readers
from scripts.tools import retention_resolver as resolver
import analyze_cpu_supply as supply
import summarize_wait_trace as wait_reader
from scripts.check import audit_perf_docs_sources as audit

ZSTD = shutil.which('zstd')


@unittest.skipUnless(ZSTD, 'zstd required')
class ReaderContractTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name).resolve()
        self.source = self.repo / 'traces/cold-run/supply.csv'
        self.source.parent.mkdir(parents=True)
        self.source.write_text('# cpu-supply-v2 first=1 last=2000 closed=1 overflow=0 incomplete=0 cpu_clock=1 pid=42\n'
            'wall_ns,cpu_ns,tid,owner,id,value,frame,event\n' + '1000,500,1,1,1,0,1,0\n' * 2000)
        self.relative = self.source.relative_to(self.repo).as_posix()
        self.completion = self.source.parent / 'completion.json'
        self.completion.write_text('{"closed":true,"returncode":0}')
        self.doc = self.repo / 'docs/perfomance/domain/leaf.md'
        self.doc.parent.mkdir(parents=True)
        self.doc.write_text('---\nstatus: accepted-verdict\nupdated: 2020-01-01\nsource: ' + self.relative + '\n---\n')
        old = (datetime.now(timezone.utc) - timedelta(days=100)).isoformat()
        self.review = {'owner_closed': True, 'recovery_resolved': True, 'reviewer': 'test',
            'reason': 'closed fixture', 'completed_at': old, 'last_used_at': old,
            'outcome': 'passed', 'reference_scope_complete': True,
            'evidence': [{'path': self.completion.relative_to(self.repo).as_posix(),
                          'sha256': retention.sha256(self.completion)}]}
        self.handles = patch.object(retention, 'open_handles', return_value=[])
        self.handles.start()
        self.addCleanup(self.handles.stop)
        self.root = patch.object(resolver, 'ROOT', self.repo)
        self.root.start()
        self.addCleanup(self.root.stop)

    def plan(self, action, review=None):
        value = retention.make_plan(self.repo, [self.relative], action,
            {self.relative: review or self.review}, 0, 'tiny fixture')
        return retention.save_plan(self.repo, value)

    def qualification(self, contract='cpu-supply-v2'):
        plan = self.plan('archive-copy')
        retention.apply_plan(self.repo, plan, ZSTD)
        qualification = readers.qualify(self.repo, plan.parent / 'receipt.json', contract, ZSTD)
        self.review.update(reader_contract=contract, reader_scope_complete=True,
            analysis_use_evidence=[{'path': self.doc.relative_to(self.repo).as_posix(),
                                    'sha256': retention.sha256(self.doc)}],
            reader_qualification={'path': qualification.relative_to(self.repo).as_posix(),
                                  'sha256': retention.sha256(qualification)})
        return qualification

    def test_referenced_archive_reader_and_source_audit_parity(self):
        original = supply.load(self.source)
        qualification = self.qualification()
        self.assertTrue(readers.validate_qualification(self.repo, self.source,
            retention.snapshot_tree(self.source), self.review))
        plan = self.plan('archive')
        self.assertTrue(retention.read_json(plan)['entries'][0]['eligible'])
        receipt = retention.apply_plan(self.repo, plan, ZSTD)
        self.assertFalse(self.source.exists())
        self.assertEqual(receipt['actions'][0]['availability'], 'archived')
        self.assertEqual(supply.load(self.source), original)
        self.assertEqual(audit.audit_paths([self.doc], self.repo), [])
        self.assertFalse(self.source.exists())
        self.assertEqual(resolver.verify_available(self.source.parent, repo=self.repo), 1)
        self.assertTrue(qualification.exists())

    def test_wait_trace_contract_preserves_parser_and_summary(self):
        self.source.write_text(
            '# wait-trace-v1 pid=1 first=1 last=2 closed=1 overflow=0 incomplete=0 '
            'unknown=0 owners=0 capacity=32768 mutex_min_ns=0 storage=1 cv_entries=0\n'
            'site,begin_ns,acquired_ns,end_ns,tid,owner,target,query,first_frame,last_frame,'
            'queue,waits,state_before,state_after,raw_before,raw_after\n' +
            '4,1,2,3,1,0,0,0,1,1,5,0,0,0,0,0\n' * 1000 + '# end rows=1000\n')
        expected = wait_reader.summarize(*wait_reader.load(self.source))
        self.qualification('wait-trace-v1')
        retention.apply_plan(self.repo, self.plan('archive'), ZSTD)
        self.assertFalse(self.source.exists())
        self.assertEqual(wait_reader.summarize(*wait_reader.load(self.source)), expected)
        self.assertEqual(audit.audit_paths([self.doc], self.repo), [])

    def test_reference_and_pin_protection_without_contract(self):
        self.assertFalse(retention.assess(self.repo, self.relative, 'archive', self.review)['eligible'])
        self.qualification()
        store = retention.store_path(self.repo)
        retention.write_json(store / 'pins.json', {'pins': [{'path': self.relative, 'reason': 'baseline'}]})
        self.assertFalse(retention.assess(self.repo, self.relative, 'archive', self.review)['eligible'])
        self.assertFalse(retention.assess(self.repo, self.relative, 'delete', self.review)['eligible'])

    def test_code_drift_fails_before_removal(self):
        self.qualification()
        plan = self.plan('archive')
        real_identity = readers.implementation_identity('cpu-supply-v2')
        with patch.object(readers, 'implementation_identity', return_value={**real_identity, 'changed': 'digest'}):
            with self.assertRaisesRegex(ValueError, 'stale plan'):
                retention.apply_plan(self.repo, plan, ZSTD)
        self.assertTrue(self.source.exists())

    def test_qualification_tampering_and_corrupt_archive_fail_closed(self):
        qualification = self.qualification()
        value = retention.read_json(qualification)
        value['source_sha256'] = '0' * 64
        retention.write_json(qualification, value)
        self.assertFalse(retention.assess(self.repo, self.relative, 'archive', self.review)['eligible'])
        # Even updating the review hash cannot admit a proof for different bytes.
        self.review['reader_qualification']['sha256'] = retention.sha256(qualification)
        self.assertFalse(retention.assess(self.repo, self.relative, 'archive', self.review)['eligible'])

    def test_later_recorded_use_cannot_be_backdated(self):
        recent = dict(self.review, last_used_at=datetime.now(timezone.utc).isoformat())
        self.plan('archive-copy', recent)
        self.assertFalse(retention.assess(self.repo, self.relative, 'archive', self.review)['eligible'])

    def test_containing_bundle_use_cannot_be_ignored(self):
        parent = self.source.parent.relative_to(self.repo).as_posix()
        recent = dict(self.review, last_used_at=datetime.now(timezone.utc).isoformat())
        value = retention.make_plan(self.repo, [parent], 'archive-copy', {parent: recent}, 0, 'fixture')
        retention.save_plan(self.repo, value)
        assessment = retention.assess(self.repo, self.relative, 'archive', self.review)
        self.assertTrue(any('recorded analysis use' in reason for reason in assessment['blockers']))

    def test_recent_use_still_blocks_qualified_archive(self):
        self.qualification()
        self.review['last_used_at'] = datetime.now(timezone.utc).isoformat()
        assessment = retention.assess(self.repo, self.relative, 'archive', self.review)
        self.assertFalse(assessment['eligible'])
        self.assertTrue(any('below 14' in b for b in assessment['blockers']))

    def test_corrupt_archived_source_fails_reader_and_directory_audit(self):
        self.qualification()
        receipt = retention.apply_plan(self.repo, self.plan('archive'), ZSTD)
        archive = Path(receipt['actions'][0]['archive']['archive_path'])
        archive.write_bytes(b'corrupt')
        with self.assertRaises(ValueError):
            supply.load(self.source)
        self.assertTrue(audit.audit_paths([self.doc], self.repo))
        with self.assertRaises(ValueError):
            resolver.verify_available(self.source.parent, repo=self.repo)


if __name__ == '__main__':
    unittest.main()
