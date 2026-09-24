#!/usr/bin/env python3
"""Strict publication-shadow transcript qualification counterexamples."""
import sys
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import analyze_prepared_encode_shadow as shadow

PROBE = sys.argv.pop(1) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else None


class ShadowTest(unittest.TestCase):
    def rows(self):
        return [dict(kind='prepared_' + name, owner=12, id=4, tid=7,
                     wall_ns=100 + i, cpu_ns=50 + i, value={
                         'begin': 64, 'allocated': 149576, 'status': 0,
                         'source_generation': 2, 'storage_generation': 3,
                         'storage_pages': (2 << 32) | 4,
                     }.get(name, 0)) for i, name in enumerate(shadow.ORDER)]

    def test_cost_and_failure_population(self):
        rows = self.rows()
        result = shadow.analyze(rows)
        self.assertEqual(result['sources'][0]['total']['cpu_ns'], 13)
        self.assertEqual(result['sources'][0]['allocation']['cpu_ns'], 1)
        rows[8]['value'] = 3
        self.assertEqual(shadow.analyze(rows)['failures'], {3: 1})

    def test_invalid_evidence(self):
        rows = self.rows()
        for bad in [[], rows[:-1], rows + [rows[-1]], rows[::-1]]:
            with self.assertRaises(ValueError):
                shadow.analyze(bad)

    def test_screen_schema(self):
        rows = self.rows()
        row = dict(rows[4], kind='prepared_screen', value=1)
        skipped = rows[:5] + [row, rows[-1]]
        result = shadow.analyze(skipped)
        self.assertEqual(result['skips'], {1: 1})
        self.assertEqual(result['failures'], {})
        self.assertIsNone(result['sources'][0]['allocation'])
        self.assertEqual(result['sources'][0]['allocation_bytes'], 0)
        with self.assertRaises(ValueError):
            shadow.analyze(skipped[:-1])
        with self.assertRaises(ValueError):
            shadow.analyze(rows[:5] + [row] + rows[5:])
        row['value'] = 0
        self.assertFalse(shadow.analyze(rows[:5] + [row] + rows[5:])['sources'][0]['skipped'])
        for field in ['tid', 'cpu_ns']:
            bad = self.rows()
            bad[6][field] = 0
            with self.assertRaises(ValueError):
                shadow.analyze(bad)

    def test_storage_reuse_schema(self):
        rows = self.rows()
        capacity = dict(rows[5], kind='prepared_storage_capacity', value=149576)
        rows.insert(6, capacity)
        result = shadow.analyze(rows)['sources'][0]
        self.assertFalse(result['storage_reused'])
        rows[5]['value'] = 0
        result = shadow.analyze(rows)['sources'][0]
        self.assertTrue(result['storage_reused'])
        self.assertEqual(result['allocation_bytes'], 0)
        self.assertEqual(result['storage_capacity_bytes'], 149576)
        for invalid in (0, -1):
            capacity['value'] = invalid
            with self.assertRaises(ValueError):
                shadow.analyze(rows)
        capacity['value'] = 149576
        rows[5]['value'] = 1
        with self.assertRaises(ValueError):
            shadow.analyze(rows)

    def test_cardinality_schema(self):
        rows = self.rows()
        rows[8]['value'] = 3
        extra = [dict(rows[-1], kind='prepared_' + name, value={
            'cardinality_commands': 64, 'cardinality_unresolved': 1,
            'cardinality_accesses': 600, 'cardinality_identities': 300,
        }.get(name, 0)) for name in shadow.CARDINALITY]
        complete = rows[:-1] + extra + rows[-1:]
        self.assertEqual(shadow.analyze(complete)['sources'][0]['cardinality']['identities'], 300)
        for bad in (complete[:-1], rows[:-1] + extra[:-1] + rows[-1:],
                    rows[:-1] + extra + [extra[-1]] + rows[-1:]):
            with self.assertRaises(ValueError):
                shadow.analyze(bad)
        extra[2]['value'] = 63
        with self.assertRaises(ValueError):
            shadow.analyze(complete)
        extra[2]['value'] = 64
        extra[1]['value'] = 2
        with self.assertRaises(ValueError):
            shadow.analyze(complete)
        for row in extra[2:-1]:
            row['value'] = 0
        self.assertIsNone(shadow.analyze(complete)['sources'][0]['cardinality']['identities'])

    @unittest.skipUnless(PROBE, 'native publication probe not supplied')
    def test_live_publication_on_off(self):
        with tempfile.TemporaryDirectory(prefix='dxmt9-prepared-shadow-') as directory:
            reference_publications = None
            for enabled in (0, 1):
                path = Path(directory) / f'{enabled}.csv'
                env = dict(os.environ, DXMT_PERF_COUNTERS='1',
                           DXMT9_CPU_SUPPLY_TRACE=str(path),
                           DXMT9_CPU_SUPPLY_TRACE_FIRST_PRESENT='1',
                           DXMT9_CPU_SUPPLY_TRACE_LAST_PRESENT='2',
                           DXMT9_CPU_SUPPLY_PREPARED_OBSERVER=str(enabled))
                subprocess.run([PROBE, '--publication-shadow-probe'], env=env,
                               capture_output=True, text=True, check=True, timeout=30)
                _, rows = shadow.supply.load(path)
                publications = [(r['kind'], r['id'], r['value']) for r in rows
                                if r['kind'] in ('publish', 'publish_commands')]
                self.assertTrue(publications)
                if reference_publications is None:
                    reference_publications = publications
                else:
                    self.assertEqual(publications, reference_publications)
                if enabled:
                    result = shadow.analyze(rows)
                    self.assertEqual(result['qualified_shadow_sources'], 21)
                    self.assertEqual(result['failures'], {0: 18})
                    self.assertEqual(result['skips'], {3: 2, 1: 1})
                    attempts = [r for r in result['sources'] if not r['skipped']]
                    self.assertEqual(sum(r['allocation_bytes'] for r in attempts), 151624)
                    self.assertEqual(sum(r['storage_reused'] for r in attempts), 17)
                    self.assertTrue(all(r['storage_capacity_bytes'] == 151624 for r in attempts))
                    self.assertTrue(all(r['commands'] == 64 for r in result['sources']
                                        if not r['skipped']))
                else:
                    self.assertFalse(any(r['kind'].startswith('prepared_') for r in rows))


if __name__ == '__main__':
    unittest.main()
