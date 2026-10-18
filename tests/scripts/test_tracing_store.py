"""R-HARN-TRACE-12: SQL preserves semantics, provenance and failed evidence."""
from pathlib import Path
import json
import sqlite3
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(Path(__file__).parent))
from test_tracing_normalize import complete_bundle
from scripts.tools.tracing_store import publish_store, digest, artifact_identity


class StoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'bundle.json'
        self.output = self.root / 'observations.sqlite'
        self.bundle = complete_bundle()

    def publish(self, artifacts=None):
        self.source.write_text(json.dumps(self.bundle))
        return publish_store(self.output, run_id='run-1', attempt_id='attempt-1',
            provenance={'workload_outcome': {'status': 'fail'}, 'revision': 'fixture'},
            bundles=[('engine', self.source)], artifacts=artifacts or [])

    def test_roundtrip_exact_identity_values_and_provenance(self):
        counter = dict(self.bundle['events'][0], event_id='counter', source_record='counter',
                       source_order='3', kind='counter',
                       attrs={'name': 'cpu', 'counter_value': '18446744073709551615',
                              'unit': 'ns', 'owner_id': '18446744073709551615'})
        self.bundle['events'].append(counter)
        self.bundle['coverage']['events'] = {'observed': '3', 'population': '3'}
        receipt = self.publish()
        self.assertEqual(receipt['sha256'], digest(self.output))
        with sqlite3.connect(self.output) as db:
            self.assertEqual(db.execute('PRAGMA integrity_check').fetchall(), [('ok',)])
            self.assertEqual(db.execute('PRAGMA foreign_key_check').fetchall(), [])
            self.assertEqual(db.execute('SELECT ts,dur FROM slice').fetchall(), [(9007199254741000,123)])
            self.assertEqual(db.execute('SELECT value_exact FROM counter').fetchone()[0], '18446744073709551615')
            self.assertEqual(json.loads(db.execute("SELECT value_json FROM provenance WHERE key='workload_outcome'").fetchone()[0])['status'], 'fail')
            record = json.loads(db.execute("SELECT record_json FROM source_event WHERE source_id='counter'").fetchone()[0])
            self.assertEqual(record['attrs']['owner_id'], '18446744073709551615')

    def test_shared_projection_keys_survive_native_and_legacy_sql(self):
        event = dict(self.bundle['events'][0], event_id='projection', source_record='native:1',
                     source_order='3', kind='counter',
                     attrs={'counter_value': '99', 'unit': 'ns',
                            'observation_source': 'cpu-supply', 'observation_record': '1',
                            'acquisition_version': '1',
                            'observation_timestamp_kind': 'cpu-query-start'})
        self.bundle['events'].append(event)
        self.bundle['coverage']['events'] = {'observed': '3', 'population': '3'}
        self.source.write_text(json.dumps(self.bundle))
        legacy = self.root / 'cpu.csv'
        legacy.write_text('# cpu-supply-v2 first=1 last=2 closed=1 overflow=0 incomplete=0 '
                          'cpu_clock=1 pid=42 acquisition_version=1 trace_attempted=1 trace_accepted=1\n'
                          'wall_ns,cpu_ns,tid,owner,id,value,frame,event\n'
                          '10,99,7,0,0,0,1,0\n')
        publish_store(self.output, run_id='run-1', attempt_id='attempt-1', provenance={},
                      bundles=[('engine', self.source), ('cpu-supply', legacy)], artifacts=[])
        with sqlite3.connect(self.output) as db:
            records = [json.loads(row[0]) for row in db.execute('SELECT record_json FROM source_event')]
            matching = [r for r in records if r['attrs'].get('observation_source') == 'cpu-supply']
            self.assertEqual(len(matching), 2)
            self.assertTrue(all(r['attrs']['observation_record'] == '1' for r in matching))
            self.assertTrue(all(r['attrs']['acquisition_version'] == '1' for r in matching))
            # Keys survive, but unknown legacy process/clock identity must not be auto-joined.
            self.assertEqual(db.execute('SELECT COUNT(*) FROM artifact_link').fetchone()[0], 0)
            self.assertIn(('unqualified',), db.execute('SELECT alignment FROM clock').fetchall())

    def test_partial_scope_and_unqualified_clock_remain_visible(self):
        self.bundle['events'].pop()
        self.bundle['window']['closed'] = False
        self.bundle['clocks'][0]['alignment'] = 'unqualified'
        self.publish()
        with sqlite3.connect(self.output) as db:
            self.assertEqual(db.execute('SELECT dur,status FROM slice').fetchone(), (None,'missing_end'))
            self.assertEqual(db.execute('SELECT status FROM qualification').fetchone()[0], 'unqualified')
            self.assertEqual(db.execute('SELECT alignment FROM clock').fetchone()[0], 'unqualified')

    def test_collision_and_symlink_refused(self):
        self.publish()
        before = self.output.read_bytes()
        with self.assertRaises(FileExistsError):
            self.publish()
        self.assertEqual(self.output.read_bytes(), before)
        self.output = self.root / 'alias.sqlite'
        self.output.symlink_to(self.root / 'observations.sqlite')
        with self.assertRaisesRegex(ValueError, 'symlink'):
            self.publish()

    def test_tamper_and_dangling_link_refused_without_output(self):
        artifact = self.root / 'capture.bin'
        artifact.write_bytes(b'capture')
        ref = {'role':'capture', 'path':str(artifact), 'sha256':'0'*64}
        with self.assertRaisesRegex(ValueError, 'digest mismatch'):
            self.publish([ref])
        self.assertFalse(self.output.exists())
        ref.update(sha256=digest(artifact), event_id='absent', evidence='fixture binding')
        with self.assertRaisesRegex(ValueError, 'unknown event'):
            self.publish([ref])
        self.assertFalse(self.output.exists())
        ref['event_id'] = 'b'
        self.publish([ref])
        with sqlite3.connect(self.output) as db:
            self.assertEqual(db.execute('SELECT count(*) FROM artifact_link').fetchone()[0],1)

    def test_missing_optional_artifact_retained(self):
        self.publish([{'role':'gpu-capture', 'path':str(self.root/'absent'), 'required':False}])
        with sqlite3.connect(self.output) as db:
            self.assertEqual(db.execute("SELECT status FROM artifact WHERE role='gpu-capture'").fetchone()[0], 'missing')

    def test_diagnostic_log_has_source_lines_without_fabricated_timestamps(self):
        log = self.root / 'run.log'
        log.write_text('[dxmt9-perf-frame frame=7 wall_ms=1.25 draw_calls=99]\n')
        self.publish([{'role':'log', 'path':str(log), 'format':'dxmt9-log'}])
        with sqlite3.connect(self.output) as db:
            self.assertEqual(db.execute('SELECT line_number,category FROM diagnostic_row').fetchall(),
                             [(1,'dxmt9-perf-frame')])
            self.assertEqual(db.execute("SELECT value_exact FROM diagnostic_value WHERE key='wall_ms'").fetchone()[0],'1.25')
            self.assertEqual(db.execute('SELECT COUNT(*) FROM source_event').fetchone()[0],2)

    def test_capture_directory_digest_and_test_verdicts(self):
        capture = self.root / 'capture.gputrace'
        capture.mkdir()
        (capture/'payload').write_bytes(b'GPU')
        sha, size, kind = artifact_identity(capture)
        self.assertEqual((size,kind),(3,'tree-sha256-v1'))
        self.publish([{'role':'gpu','path':str(capture),'sha256':sha}])
        self.output = self.root / 'tampered.sqlite'
        (capture/'payload').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError,'digest mismatch'):
            self.publish([{'role':'gpu','path':str(capture),'sha256':sha}])

    def test_run_mismatch_and_malformed_counter_rejected(self):
        self.bundle['run_id'] = 'another-run'
        with self.assertRaisesRegex(ValueError, 'run identity mismatch'):
            self.publish()
        self.bundle['run_id'] = 'run-1'
        self.bundle['events'][0].update(kind='counter', attrs={'value': 'NaN'})
        with self.assertRaises(ValueError):
            self.publish()
        self.assertFalse(self.output.exists())

    def test_legacy_cpu_cpu_time_is_preserved_without_clock_alignment(self):
        self.source.write_text('# cpu-supply-v2 first=1 last=2 closed=1 overflow=0 incomplete=0 cpu_clock=1 pid=42\n'
                               'wall_ns,cpu_ns,tid,owner,id,value,frame,event\n1000,20,7,0,1,0,1,0\n')
        publish_store(self.output, run_id='run-1', attempt_id='attempt-1', provenance={},
                      bundles=[('cpu',self.source)], artifacts=[])
        with sqlite3.connect(self.output) as db:
            row = db.execute("SELECT value_json FROM args WHERE key='thread_cpu_counter_ns'").fetchone()
            self.assertEqual(json.loads(row[0]), '20')
            self.assertEqual(db.execute('SELECT status FROM qualification').fetchone()[0], 'unqualified')


if __name__ == '__main__':
    unittest.main()
