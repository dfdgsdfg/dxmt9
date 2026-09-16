import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('supply', ROOT/'scripts/tools/analyze_cpu_supply.py')
supply = importlib.util.module_from_spec(spec)
spec.loader.exec_module(supply)


def fixture():
    rows = []
    def row(kind, t, tid, owner=1, identity=0, value=0, frame=1):
        rows.append(dict(kind=kind, event=supply.EVENTS.index(kind), wall_ns=t,
                         cpu_ns=t//2+1, tid=tid, owner=owner, id=identity,
                         value=value, frame=frame))
    for i in range(1, 6):
        t = i*10_000_000
        row('import_begin', t, 1)
        row('raw_ready', t+1_000_000, 1, identity=i)
        row('queue_ready', t+1_000_001, 1, identity=1, value=1)
        row('import_end', t+1_000_002, 1)
        row('replay_begin', t+1_100_000, 2, identity=i)
        row('queue_empty', t+1_100_001, 2)
        row('publish', t+3_000_000, 2, owner=2, identity=i, value=i)
        row('replay_end', t+4_000_000, 2)
        row('encode_begin', t+4_100_000, 3, owner=2, identity=i)
        row('present', t+5_000_000, 3, owner=0, identity=i, frame=i)
        row('encode_end', t+6_000_000, 3, owner=2, identity=i)
    rows.sort(key=lambda r: r['wall_ns'])
    return dict(first=1, last=5, closed=1, overflow=0, incomplete=0), rows


class TestSupply(unittest.TestCase):
    def test_arrival_limited(self):
        jobs = [(0, 0, 2), (10, 10, 12), (20, 20, 22)]
        for factor in (1, .5, 0):
            modeled = supply.serial_schedule(jobs, factor)
            self.assertEqual(modeled[-1][1]-modeled[0][1], 20)

    def test_saturated(self):
        jobs = [(0, 0, 10), (1, 10, 20), (2, 20, 30)]
        self.assertEqual(supply.serial_schedule(jobs, .5)[-1][1], 15)

    def test_retained_scheduler_gap(self):
        self.assertEqual(supply.serial_schedule([(0, 4, 10)], .5), [(4, 7)])

    def test_diagnostic_pipeline(self):
        meta, rows = fixture()
        r = supply.analyze(meta, rows, trim=1)
        self.assertEqual(r['frames'], 2)
        self.assertAlmostEqual(sum(r['queue_fraction'].values()), 1)
        self.assertEqual(r['fixed_arrival_sensitivity']['0.0']['modeled_throughput_change_percent'], 0)
        self.assertGreater(r['fixed_arrival_sensitivity']['0.0']['present_advance_median_ms'], 0)

    def test_reject_missing_present(self):
        meta, rows = fixture()
        with self.assertRaises(ValueError):
            supply.analyze(meta, [r for r in rows if not(r['kind']=='present' and r['id']==3)], 1)

    def test_reject_stale_identity(self):
        meta, rows = fixture()
        next(r for r in rows if r['kind']=='publish')['value'] = 999
        with self.assertRaises(ValueError):
            supply.analyze(meta, rows, 1)

    def test_reject_missing_stage(self):
        meta, rows = fixture()
        with self.assertRaises(ValueError):
            supply.analyze(meta, [r for r in rows if not r['kind'].startswith('encode_')], 1)

    def test_reject_counter_rollback(self):
        meta, rows = fixture()
        next(r for r in rows if r['kind']=='replay_end')['cpu_ns'] = 1
        with self.assertRaises(ValueError):
            supply.analyze(meta, rows, 1)

    def test_reject_missing_interior_encode_endpoint(self):
        for kind in ('encode_begin', 'encode_end'):
            with self.subTest(kind=kind):
                meta, rows = fixture()
                rows = [r for r in rows if not (r['kind']==kind and r['id']==3)]
                with self.assertRaises(ValueError):
                    supply.analyze(meta, rows, 1)

    def test_reject_missing_interior_present_owner(self):
        meta, rows = fixture()
        rows = [r for r in rows if not (r['kind'].startswith('encode_') and r['id']==3)]
        with self.assertRaises(ValueError):
            supply.analyze(meta, rows, 1)

    def test_reject_thread_counter_rollback_between_scopes(self):
        meta, rows = fixture()
        for r in rows:
            if r['tid']==1 and 30_000_000 <= r['wall_ns'] <= 32_000_000:
                r['cpu_ns'] -= 10_000_000
        with self.assertRaises(ValueError):
            supply.analyze(meta, rows, 1)

    def test_reject_loss(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)/'trace.csv'
            for fields in ('closed=0 overflow=0 incomplete=0',
                           'closed=1 overflow=1 incomplete=0',
                           'closed=1 overflow=0 incomplete=1'):
                p.write_text('# cpu-supply-v2 first=1 last=5 cpu_clock=1 '+fields+'\n')
                with self.assertRaises(ValueError):
                    supply.load(p)

    def test_reject_unqualified_posix_clock(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'trace.csv'
            p.write_text('# cpu-supply-v1 first=1 last=5 closed=1 overflow=0 incomplete=0\n')
            with self.assertRaises(ValueError):
                supply.load(p)

    def test_reject_duplicate_ready_identity(self):
        meta, rows=fixture()
        duplicate=next(r for r in rows if r['kind']=='raw_ready').copy()
        rows.append(duplicate)
        rows.sort(key=lambda r:r['wall_ns'])
        with self.assertRaises(ValueError):
            supply.analyze(meta, rows, 1)

    def test_scene_window(self):
        log='\n'.join(f'[dxmt9-perf-frame frame={i} wall_ms=30 draw_calls=1500]' for i in range(3,6))
        self.assertEqual(supply.qualify_scene(log,3,6)['count'],3)
        with self.assertRaises(ValueError):
            supply.qualify_scene(log.replace('draw_calls=1500','draw_calls=0'),3,6)
        with self.assertRaises(ValueError):
            supply.qualify_scene(log,3,7)


if __name__ == '__main__':
    unittest.main()
