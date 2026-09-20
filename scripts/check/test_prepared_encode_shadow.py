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
        for field in ['tid', 'cpu_ns']:
            bad = self.rows()
            bad[6][field] = 0
            with self.assertRaises(ValueError):
                shadow.analyze(bad)

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
                    self.assertEqual(result['qualified_shadow_sources'], 18)
                    self.assertEqual(result['failures'], {0: 18})
                    self.assertTrue(all(r['commands'] == 64 for r in result['sources']))
                else:
                    self.assertFalse(any(r['kind'].startswith('prepared_') for r in rows))


if __name__ == '__main__':
    unittest.main()
