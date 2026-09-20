import importlib.util
from pathlib import Path
import tempfile
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('supply', ROOT/'scripts/tools/analyze_cpu_supply.py')
supply = importlib.util.module_from_spec(spec)
spec.loader.exec_module(supply)
sys.path.insert(0, str(ROOT/'scripts/tools'))
import join_replay_wait_trace as joiner
import summarize_guest_pc_window as guest
import analyze_publication_progress as progress


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
    def test_encode_phase_partition(self):
        names = ('encode_begin',) + progress.PHASES + ('encode_end',)
        events = {name: dict(tid=3, wall_ns=1000000+i*200000,
                            cpu_ns=100000+i*100000)
                  for i, name in enumerate(names)}
        result = progress.encode_phases(events)
        self.assertTrue(result['qualified'])
        self.assertAlmostEqual(sum(v['cpu_ms'] for k, v in result.items()
                                   if k != 'qualified'), .5)
        self.assertAlmostEqual(result['backend']['wall_ms'], .2)
        missing = dict(events)
        del missing['encode_backend_end']
        self.assertFalse(progress.encode_phases(missing)['qualified'])
        self.assertFalse(progress.encode_phases({})['qualified'])
        for replacement in ({'tid': 4}, {'wall_ns': 1}, {'cpu_ns': 1},
                            {'cpu_ns': 10**12}):
            invalid = dict(events)
            invalid['encode_prefetch_end'] = dict(events['encode_prefetch_end'],
                                                  **replacement)
            with self.assertRaises(ValueError):
                progress.encode_phases(invalid)

    def test_publication_progress(self):
        meta, rows = fixture()
        meta['pid'] = 17
        # END of one query closes a populated one-command scope.
        for kind, identity, value in [('query_issue', 54, 1),
                                      ('scope_boundary', 2, (1 << 63) | (1 << 32)),
                                      ('publish_reason', 2, 9), ('publish_commands', 2, 1)]:
            rows.append(dict(kind=kind, wall_ns=23_000_001, cpu_ns=11_500_001,
                             tid=2, owner=2, id=identity, value=value, frame=2))
        for r in list(rows):
            if r['kind']=='replay_begin':
                rows.append(dict(r, kind='replay_ledger', value=9, wall_ns=r['wall_ns']+1))
            if r['kind']=='encode_end':
                for offset, kind in enumerate(['submit','completion_dequeue','completion_observed','retire'], 1):
                    rows.append(dict(r, kind=kind, wall_ns=r['wall_ns']+offset*1000))
        rows.sort(key=lambda r:r['wall_ns'])
        receipt = dict(site=0, waits=1, owner=9, query=54, first_frame=2,
                       completed_before=1, raw_before=2, acquired_ns=1, end_ns=2)
        r = progress.analyze(meta, rows, {'pid':17}, [receipt])
        self.assertEqual(r['target_missing_source_stages'], 0)
        self.assertEqual(r['target_sources'][0]['reason'], 9)
        self.assertEqual(r['target_sources'][0]['commands'], 1)
        self.assertAlmostEqual(r['target_sources'][0]['encode_cpu_ms'], .95)
        self.assertFalse(r['target_sources'][0]['encode_phases']['qualified'])
        self.assertEqual(r['scope_boundaries'][0],
                         dict(sequence=2, published=True, before=1, after=0))
        self.assertEqual(r['target_sources'][0]['stages_ms']['submit__completion_dequeue'], .001)
        with self.assertRaises(ValueError):
            progress.analyze(meta, rows, {'pid':17}, [receipt], query=55)
        incomplete = progress.analyze(meta, [r for r in rows if r['kind']!='retire'],
                                      {'pid':17}, [receipt])
        self.assertEqual(incomplete['target_missing_source_stages'], 1)
        with self.assertRaises(ValueError):
            progress.analyze(meta, rows+[next(r for r in rows if r['kind']=='submit')],
                             {'pid':17}, [receipt])
        for replacement in ({'tid': 999}, {'cpu_ns': 0}, {'cpu_ns': 10**12}):
            invalid = [dict(r, **replacement) if r['kind']=='encode_end' else r
                       for r in rows]
            with self.assertRaises(ValueError):
                progress.analyze(meta, invalid, {'pid':17}, [receipt])
        observed = list(rows)
        for index, kind in enumerate(progress.PHASES):
            t = 24_200_000 + index*400_000
            observed.append(dict(kind=kind, wall_ns=t, cpu_ns=t//2+1,
                                 tid=3, owner=2, id=2, value=0, frame=2))
        observed.sort(key=lambda r: r['wall_ns'])
        phases = progress.analyze(meta, observed, {'pid':17}, [receipt])[
            'target_sources'][0]['encode_phases']
        self.assertTrue(phases['qualified'])
        self.assertAlmostEqual(phases['prefetch']['cpu_ms'], .2)
        self.assertAlmostEqual(phases['backend']['cpu_ms'], .2)
        self.assertAlmostEqual(sum(v['cpu_ms'] for k, v in phases.items()
                                   if k != 'qualified'), .95)
        with self.assertRaises(ValueError):
            progress.analyze(meta, observed+[next(r for r in observed
                if r['kind']=='encode_backend_begin')], {'pid':17}, [receipt])

    def test_guest_pc_window_is_not_cpu(self):
        def group(frame, samples):
            return (f'[dxmt9-perf-frame frame={frame} wall_ms=1]\n'
                    f'[dxmt9-pe-sampler] presents={frame} samples={samples} '
                    'suspend_failures=0 ctx_failures=0 resume_failures=0 selfpc_coverage=1\n'
                    f'[dxmt9-pe-sampler] module=xrEngine.exe samples={samples}\n'
                    '[dxmt9-pe-sampler] selfpc_module=d3d9.dll\n'
                    '[dxmt9-pe-sampler] selfpc_overflow=0\n'
                    '[dxmt9-pe-sampler] selfpc_summary mode=full rows=0 used=0 '
                    'self_samples=0 reported_samples=0 omitted_samples=0 overflow_samples=0 complete=1\n')
        text = group(1800, 10)+group(2000, 20)
        self.assertEqual(guest.summarize(text, 1800, 2100)['samples'], 10)
        truncated_modules = text.replace('module=xrEngine.exe samples=10',
                                         'module=xrEngine.exe samples=9').replace(
                                         'module=xrEngine.exe samples=20',
                                         'module=xrEngine.exe samples=17')
        residual = guest.summarize(truncated_modules, 1800, 2100)
        self.assertEqual(next(r['samples'] for r in residual['modules']
                              if r['module'] == 'unreported-module-tail'), 2)
        with self.assertRaises(ValueError):
            guest.summarize(group(1800, 10)+group(2000, 20).replace(
                'module=xrEngine.exe', 'module=xrRender_R2.dll'), 1800, 2100)
        with self.assertRaises(ValueError):
            guest.summarize(text.replace('complete=1', 'complete=0'), 1800, 2100)
        with self.assertRaises(ValueError):
            guest.summarize(text+'target_thread_mismatch', 1800, 2100)

    def test_receipt_replay_join(self):
        meta, rows = fixture()
        meta['pid'] = 17
        for kind, t in [('commit_wait_begin', 22_000_000), ('commit_wait_end', 23_000_000)]:
            rows.append(dict(kind=kind, event=supply.EVENTS.index(kind),
                wall_ns=t, cpu_ns=t//2+1, tid=2, owner=2, id=2, value=31, frame=2))
        for r in list(rows):
            if r['kind'] == 'replay_begin':
                rows.append(dict(r, kind='replay_ledger', event=18,
                                 wall_ns=r['wall_ns']+1, value=9))
        rows.sort(key=lambda r: r['wall_ns'])
        receipt = dict(site=0, waits=1, owner=9, query=54, first_frame=2,
                       completed_before=1, raw_before=2, acquired_ns=1, end_ns=2)
        result = joiner.join({'pid':17}, [receipt], meta, rows)
        self.assertEqual(result['matched_targets'], 1)
        target = result['receipts'][0]['target']
        self.assertEqual(target['publications'][0]['sequence'], 2)
        self.assertEqual(target['wall_ns'], 2_900_000)
        self.assertEqual(target['queue_wait_ns'], 1_000_000)
        with self.assertRaises(ValueError):
            joiner.join({'pid':17}, [receipt], meta,
                        [r for r in rows if r['kind'] != 'commit_wait_end'])
        receipt['owner'] = 10
        self.assertEqual(joiner.join({'pid':17}, [receipt], meta, rows)['matched_targets'], 0)
        with self.assertRaises(ValueError):
            joiner.join({'pid':18}, [receipt], meta, rows)
        with self.assertRaises(ValueError):
            joiner.join({'pid':17}, [receipt], meta, rows +
                        [next(r for r in rows if r['kind']=='replay_ledger')])

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
