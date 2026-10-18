"""R-HARN-TRACE-15: producer records, fragment coverage and structural provenance."""
from pathlib import Path
import json
import sqlite3
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parent))
from test_tracing_store import StoreTests


class SharedDiagnosticTests(StoreTests):
    def add_report(self, schema, payload, source='query-report', encoding=1,
                   exact='0', fragment_bytes=191, omit=None):
        data = payload.encode()
        parts = [data[i:i + fragment_bytes] for i in range(0, len(data), fragment_bytes)] or [b'']
        record = str(len(self.bundle['events']))
        for i, part in enumerate(parts):
            if i == omit:
                continue
            order = str(len(self.bundle['events']) + 1)
            e = dict(self.bundle['events'][0], event_id='report-' + order, source_record=order,
                     source_order=order, kind='instant', attrs={
                         'name': schema, 'observation_source': source, 'observation_record': record,
                         'acquisition_version': '1', 'diagnostic_schema': schema,
                         'diagnostic_payload': part.hex(), 'diagnostic_part': str(i),
                         'diagnostic_parts': str(len(parts)), 'diagnostic_encoding': str(encoding),
                         'diagnostic_value': exact})
            self.bundle['events'].append(e)
        total = str(len(self.bundle['events']))
        self.bundle['coverage']['events'] = {'observed': total, 'population': total}

    def test_fragments_utf8_exact_u64_and_repeated_keys(self):
        text = 'x' * 190 + '한글 samples=11 omitted=3 samples=7'
        self.add_report('query-v1', text)
        self.add_report('counter', 'id:latest', source='perf-report', encoding=2, exact=str(2**64-1))
        self.publish()
        with sqlite3.connect(self.output) as db:
            self.assertEqual(db.execute('SELECT payload_bytes FROM observation_report ORDER BY id').fetchone()[0], text.encode())
            self.assertEqual(db.execute('SELECT value_exact FROM observation_report WHERE encoding=2').fetchone()[0], str(2**64-1))
            self.assertEqual(db.execute("SELECT value_exact FROM observation_field WHERE key='samples' ORDER BY ordinal").fetchall(), [('11',), ('7',)])
            self.assertEqual(db.execute('SELECT DISTINCT status FROM observation_report').fetchall(), [('complete',)])

    def test_partial_fragments_never_materialize_fields(self):
        self.add_report('query-v1', 'samples=11 ' * 50, omit=1)
        self.publish()
        with sqlite3.connect(self.output) as db:
            self.assertEqual(db.execute('SELECT status FROM observation_report').fetchall(), [('incomplete',)])
            self.assertEqual(db.execute('SELECT count(*) FROM observation_field').fetchone()[0], 0)
            self.assertEqual(db.execute("SELECT status FROM qualification WHERE scope LIKE 'shared-report:%'").fetchall(), [('unqualified',)])

    def test_structures_have_hash_and_event_link_without_timeline_guess(self):
        artifact = self.root / 'dag.json'
        artifact.write_text(json.dumps({'passes': [{'id': 2**64-1, 'edges': [1, 2]}]}))
        self.add_report('dxmt9-artifact-v1', str(artifact), source='renderer')
        self.add_report('dxmt9-artifact-v1', str(artifact), source='renderer')
        self.publish()
        with sqlite3.connect(self.output) as db:
            self.assertEqual(db.execute("SELECT value_json FROM structural_value WHERE path='/passes/0/id'").fetchone()[0], str(2**64-1))
            self.assertEqual(db.execute("SELECT count(*) FROM artifact WHERE role='renderer-structure'").fetchone()[0], 1)
            self.assertEqual(db.execute("SELECT count(*) FROM artifact_link WHERE relation='produced'").fetchone()[0], 2)
            self.assertEqual(db.execute('PRAGMA foreign_key_check').fetchall(), [])

    def test_invalid_clock_stays_unqualified(self):
        self.add_report('wait-row-v1', 'begin=100 end=200 qualified=0', source='wait-detail')
        self.publish()
        with sqlite3.connect(self.output) as db:
            self.assertEqual(db.execute("SELECT status FROM qualification WHERE scope LIKE 'endpoint-clock:%'").fetchall(), [('unqualified',)])

    def test_bad_hex_rejects_publication(self):
        self.add_report('query-v1', 'calls=1')
        self.bundle['events'][-1]['attrs']['diagnostic_payload'] = 'xz'
        with self.assertRaises(ValueError):
            self.publish()
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
