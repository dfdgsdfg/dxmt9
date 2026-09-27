import importlib.util
from pathlib import Path
import tempfile
import re
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
import analyze_framegraph_planning as planning
import analyze_app_queue_waits as app_waits
import analyze_queue_submit as submit


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
    def test_submit_partition(self):
        rows = [dict(kind='queue_submit_phase', owner=1, tid=2, id=3,
                     value=p, wall_ns=100+i*100, cpu_ns=50+i*50)
                for i, p in enumerate([1, 2, 3, 4, 5, 6, 7, 0])]
        result = submit.analyze(rows)
        self.assertEqual(len(result['sources']), 1)
        self.assertEqual(result['sources'][0]['phases'][3]['phase'], 'driver_commit')
        self.assertEqual(result['sources'][0]['phases'][3]['wall_ns'], 100)
        for invalid in (rows[:3]+rows[4:], rows+rows, rows[::-1]):
            with self.assertRaises(ValueError):
                submit.analyze(invalid)
        for clipped in (rows[1:], rows[:-1]):
            self.assertEqual(submit.analyze(clipped)['incomplete_or_rejected_sources'], 1)
        self.assertEqual(len(submit.analyze(rows+[dict(r, owner=9) for r in rows])['sources']), 2)
        boundary = dict(rows[0], kind='queue_submit_validation_end',
                        wall_ns=150, cpu_ns=75, value=1)
        family = dict(boundary, kind='queue_submit_summary_family', wall_ns=160, value=2)
        detailed = submit.analyze(rows+[boundary, family])['sources'][0]['validation_summary_split']
        self.assertEqual(detailed['summary_wall_ns'], 50)
        self.assertEqual(detailed['validation_wall_ns'], 50)
        self.assertEqual(detailed['families'][0]['family'], 'arena')
        self.assertTrue(detailed['family_coverage_complete'])
        self.assertFalse(submit.analyze(rows+[boundary])['sources'][0]
                         ['validation_summary_split']['family_coverage_complete'])
        for extra in ([boundary,boundary], [dict(boundary,wall_ns=300)],
                      [dict(family,value=9)]):
            with self.assertRaises(ValueError):
                submit.analyze(rows+extra)

    def test_mutex_events_are_not_replay_occupancy(self):
        meta, rows = fixture()
        expected = supply.analyze(meta, rows, 1)
        rows.append(dict(kind='queue_mutex_hold_begin', owner=99, tid=4,
                         id=900, value=0, wall_ns=25_000_000, cpu_ns=10,
                         frame=2, event=supply.EVENTS.index('queue_mutex_hold_begin')))
        rows.sort(key=lambda r: r['wall_ns'])
        self.assertEqual(supply.analyze(meta, rows, 1), expected)

    def test_supply_event_schema_order(self):
        header = (ROOT/'src/dxmt9/dxmt9_cpu_supply_trace.hpp').read_text()
        body = re.search(r'enum class Event[^\{]*\{(.*?)\};', header, re.S)[1]
        names = [s.strip() for s in body.split(',') if s.strip()]
        snake = [re.sub(r'(?<!^)(?=[A-Z])', '_', name).lower()
                 .replace('state_block', 'stateblock') for name in names]
        self.assertEqual(snake, supply.EVENTS)

    def test_app_queue_wait_attempts(self):
        def row(kind, wall, value=0, identity=1):
            return dict(kind=kind, wall_ns=wall, cpu_ns=wall//2,
                        tid=2, owner=3, id=identity, value=value)
        rows = [row('stateblock_wait_capacity', 90, 32),
                row('stateblock_wait_begin', 100, 32),
                row('stateblock_wait_end', 200, 31),
                row('stateblock_wait_adopted', 220, 99)]
        result = app_waits.analyze(rows)
        self.assertEqual(result['waits'][0]['replay_seq'], 99)
        self.assertEqual(result['waits'][0]['wall_ns'], 100)
        self.assertEqual(app_waits.analyze(rows[2:])['incomplete_attempts'], 1)
        self.assertIsNone(app_waits.analyze(rows[:3])['waits'][0]['replay_seq'])
        self.assertIsNone(app_waits.analyze(rows[:3] + [dict(rows[3], value=0)])
                          ['waits'][0]['replay_seq'])
        with self.assertRaises(ValueError):
            app_waits.analyze(rows + [rows[0]])
        other = [dict(r, id=2) for r in rows]
        self.assertEqual(len(app_waits.analyze(rows + other)['waits']), 2)
        mark = [row('mark_ticket_wait_begin', 100, 8),
                row('mark_ticket_wait_end', 300, 8),
                row('mark_ticket_hold_end', 350, 8)]
        self.assertEqual(app_waits.analyze(mark)['waits'][0]['hold_body_ns'], 50)

    def test_app_mutex_holder_identity_and_clipping(self):
        def row(kind, t, tid=2, owner=3, identity=1, value=0):
            return dict(kind=kind, wall_ns=t, cpu_ns=t//2,
                        tid=tid, owner=owner, id=identity, value=value)
        wait = [row('mark_ticket_wait_begin', 100), row('mark_ticket_wait_end', 300)]
        held = [row('queue_mutex_hold_begin', 150, tid=4, identity=2),
                row('queue_mutex_hold_end', 250, tid=4, identity=2,
                    value=app_waits.site_id('commit_current_chunk/post_wait'))]
        result = app_waits.analyze(wait+held)['waits'][0]
        self.assertEqual(result['observed_held_overlap_ns'], 100)
        self.assertEqual(result['unattributed_ns'], 100)
        self.assertEqual(result['observed_holders'][0]['site'], 'commit_current_chunk/post_wait')
        for foreign in ([dict(r, owner=9) for r in held],
                        [dict(r, tid=2) for r in held], held[:1], held[1:]):
            self.assertEqual(app_waits.analyze(wait+foreign)['waits'][0]
                             ['observed_held_overlap_ns'], 0)
        nested = [dict(r, id=3) for r in held]
        self.assertEqual(app_waits.analyze(wait+held+nested)['waits'][0]
                         ['observed_held_overlap_ns'], 100)
        with self.assertRaises(ValueError):
            app_waits.analyze(held+held)

    def test_planning_partition(self):
        rows = [dict(kind='framegraph_planning_phase', owner=1, tid=2, id=3,
                     value=p, wall_ns=1000+i*100, cpu_ns=500+i*50)
                for i, p in enumerate([1, 2, 3, 4, 5, 6, 7, 0])]
        result = planning.analyze(rows)
        self.assertEqual(len(result['sources']), 1)
        self.assertEqual(result['sources'][0]['cpu_ns'], 350)
        self.assertEqual(len(result['sources'][0]['phases']), 7)
        for malformed in (rows[:3]+rows[4:], rows[:3]+rows[2:]):
            with self.assertRaises(ValueError):
                planning.analyze(malformed)
        self.assertEqual(planning.analyze(rows[:-1])['incomplete_sources'], 1)
        self.assertFalse(planning.analyze(rows[2:])['sources'])
        self.assertFalse(planning.analyze([rows[-1]])['sources'])
        self.assertEqual(planning.analyze(
            rows[:4]+[dict(rows[4], value=8)])['aborted_sources'], 1)
        foreign = [dict(r, id=4) for r in rows]
        self.assertEqual(len(planning.analyze(rows+foreign)['sources']), 2)

    def test_parallel_access_transcript(self):
        begin = dict(tid=3,wall_ns=100,cpu_ns=100)
        end = dict(tid=3,wall_ns=1000,cpu_ns=1000)
        values = [(4<<32)|304, (266<<32)|300, 600, (8<<32)|250, 1|8|16|32]
        rows = [dict(begin,kind=k,value=v,wall_ns=200+i*100,cpu_ns=200+i*100)
                for i,(k,v) in enumerate(zip(progress.ACCESS_EVENTS,values))]
        result = progress.parallel_accesses(begin,end,rows,1,1)[0]
        self.assertEqual(result['distinct'],266)
        self.assertTrue(result['exact'])
        self.assertEqual(result['adjacent_reuse'],250)
        self.assertIsNone(progress.parallel_accesses(begin,end,[],1,1))
        for invalid in (rows[:-1], rows+rows,
                        [dict(rows[0],tid=4)]+rows[1:],
                        rows[:1]+[dict(rows[1],value=(200<<32)|300)]+rows[2:],
                        rows[:3]+[dict(rows[3],value=(8<<32)|599)]+rows[4:],
                        rows[:-1]+[dict(rows[-1],value=3|8|16|32)]):
            with self.assertRaises(ValueError):
                progress.parallel_accesses(begin,end,invalid,1,1)
        clipped = [dict(rows[0],value=(4<<32)|5004),dict(rows[1],value=(4096<<32)|5000),
                           dict(rows[2],value=6000),rows[3],dict(rows[4],value=2|8|16|32)]
        self.assertFalse(progress.parallel_accesses(begin,end,clipped,1,1)[0]['exact'])

    def test_parallel_observer_is_structural_and_fail_closed(self):
        begin = dict(kind='framegraph_encode_begin', tid=3, wall_ns=1000, cpu_ns=500)
        end = dict(begin, kind='framegraph_encode_end', wall_ns=10000, cpu_ns=5000)
        rows = [dict(begin, kind='parallel_observe_'+kind, value=value,
                     wall_ns=2000+i*500, cpu_ns=1000+i*250)
                for i,(kind,value) in enumerate((
                    ('begin',0), ('counts',(2<<32)|2), ('draws',256),
                    ('children',4), ('status',1<<32), ('imbalance',0), ('end',2)))]
        result = progress.parallel_observation(begin,end,rows)
        self.assertEqual(result['structural_eligible'],2)
        self.assertIn('not eligible-draw CPU',result['qualification'])
        self.assertFalse(progress.parallel_observation(begin,end,[])['qualified'])
        sized = rows[:-1] + [dict(rows[-1], kind='parallel_observe_resources',
                                 value=(60<<32)|80)] + [dict(rows[-1], wall_ns=6000, cpu_ns=3000)]
        sizes = progress.parallel_observation(begin,end,sized)['accepted_pass_resources']
        self.assertEqual(sizes,dict(max_pass_reads=60,total_pass_reads=80))
        self.assertIsNone(result['accepted_pass_resources'])
        for bad in ((60<<32)|59, (60<<32)|121):
            invalid = sized[:-2]+[dict(sized[-2],value=bad)]+sized[-1:]
            with self.assertRaises(ValueError):
                progress.parallel_observation(begin,end,invalid)
        rejected = [dict(r, value=(1 if r['kind']=='parallel_observe_begin' else
                    (18 if r['kind']=='parallel_observe_status' else 0))) for r in rows]
        result = progress.parallel_observation(begin,end,rejected)
        self.assertTrue(result['query_scoped'])
        self.assertEqual(result['structural_eligible'],0)
        self.assertEqual(result['fallback'],'query_state')
        for changed in (rows[:-1], rows+[rows[-1]],
                        [dict(r,tid=4) if i==2 else r for i,r in enumerate(rows)],
                        [dict(rows[0],value=1)]+rows[1:],
                        rows[:-1]+[dict(rows[-1],value=3)]):
            with self.assertRaises(ValueError):
                progress.parallel_observation(begin,end,changed)

    def test_parallel_observer_storage_is_not_executable(self):
        source = (ROOT/'src/dxmt9/dxmt9_draw_encoder_chunk.mm').read_text()
        block = source.split('if (observeSerialParallel) {',1)[1].split('\n    } else {',1)[0]
        self.assertIn('snapshotInput, diagnosticSnapshots',block)
        self.assertNotIn('snapshotInput, parallelPassShadows',block)
        self.assertNotIn('tryEncodeParallelPass(',block)
        self.assertNotIn('countParallelPassShadow(',block)
        self.assertIn('IdentitySerial &&',source)
        self.assertIn('ScopedParallelAccessObserver scope(accessObserver)',block)
        header = (ROOT/'src/dxmt9/dxmt9_parallel_render_pass.hpp').read_text()
        enum = header.split('enum class SealedParallelPassSnapshotFallback',1)[1].split('{',1)[1].split('}',1)[0]
        names = [s.strip() for s in enum.split(',') if s.strip() and s.strip() != 'Count']
        self.assertEqual([re.sub(r'(?<!^)(?=[A-Z])','_',n).lower() for n in names],
                         progress.PARALLEL_REASONS)

    def test_parallel_resource_detail_reasons(self):
        begin = dict(kind='framegraph_encode_begin', tid=3, wall_ns=1000, cpu_ns=500)
        end = dict(begin, kind='framegraph_encode_end', wall_ns=10000, cpu_ns=5000)
        for detail in ('raw_read_set_incomplete', 'raw_write_set_incomplete',
                       'pass_read_union_overflow'):
            index = progress.PARALLEL_REASONS.index(detail)
            values = [('begin',0), ('counts',(1<<32)|1), ('draws',0),
                      ('children',0), ('status',(1<<32)|13), ('imbalance',0),
                      ('reject',(13<<32)|1), ('reject',(index<<32)|1), ('end',0)]
            rows = [dict(begin, kind='parallel_observe_'+kind, value=value,
                         wall_ns=2000+i*500, cpu_ns=1000+i*250)
                    for i,(kind,value) in enumerate(values)]
            result = progress.parallel_observation(begin,end,rows)
            self.assertEqual(result['fallback'],'resource_set_incomplete')
            self.assertEqual(result['rejections'],
                             {'resource_set_incomplete':1, detail:1})
            with self.assertRaises(ValueError):
                progress.parallel_observation(begin,end,rows[:6]+rows[7:])

    def test_parallel_summary_does_not_price_draw_subset(self):
        observation = dict(qualified=True, query_scoped=False, structural_eligible=1,
                           structural_draws=128, observer_cpu_ms=2, rejections={})
        source = dict(encode_cpu_ms=20, framegraph_phases=dict(
            chunk_phases=dict(parallel_observation=observation)))
        summary = progress.parallel_summary([source,dict(encode_cpu_ms=100)])
        group = summary['groups']['structural_candidate_sources']
        self.assertEqual(group['inclusive_encode_cpu_ms'],20)
        self.assertEqual(group['observer_cpu_ms'],2)
        self.assertEqual(summary['unqualified_sources'],1)
        self.assertIn('not predicted savings',summary['qualification'])

    def test_nested_encode_draw_partition(self):
        a = dict(kind='chunk_body_begin',tid=3,wall_ns=1000000,cpu_ns=500000,value=3)
        b = dict(a,kind='chunk_body_end',wall_ns=3000000,cpu_ns=1500000)
        rows = []
        for kind,value,t in (
                ('chunk_command_begin',1,1100000),('encode_draw_begin',7,1200000),
                ('encode_draw_end',7,1500000),('chunk_command_end',1,1900000),
                ('chunk_split_begin',0,2000000),('chunk_split_end',0,2100000)):
            rows.append(dict(a,kind=kind,value=value,wall_ns=t,cpu_ns=t//2))
        p = progress.command_phases(a,b,rows)
        self.assertTrue(p['draw_phases']['qualified'])
        self.assertAlmostEqual(p['draw_phases']['encode_draw']['cpu_ms'],.15)
        self.assertAlmostEqual(p['draw_phases']['preparation_pass_residual']['cpu_ms'],.25)
        self.assertFalse(progress.command_phases(dict(a,value=1),b,rows)[
            'draw_phases']['qualified'])
        for bad in (rows[:2]+rows[3:], rows[:1]+rows[2:], rows+[rows[1]],
                    [rows[0],dict(rows[1],tid=4)]+rows[2:],
                    rows[:2]+[dict(rows[2],value=9)]+rows[3:],
                    rows[:2]+[dict(rows[2],cpu_ns=1)]+rows[3:],
                    [dict(rows[0],value=0)]+rows[1:3]+[dict(rows[3],value=0)]+rows[4:]):
            with self.assertRaises(ValueError):
                progress.command_phases(a,b,bad)

    def test_command_body_cost_partition(self):
        a = dict(kind='chunk_body_begin', tid=3, wall_ns=1000000,
                 cpu_ns=500000, value=1)
        b = dict(a, kind='chunk_body_end', wall_ns=3000000, cpu_ns=1500000)
        rows = []
        for i,(kind,value) in enumerate((
                ('chunk_command_begin',1),('chunk_command_end',1),
                ('chunk_split_begin',0),('chunk_split_end',0),
                ('chunk_command_begin',0),('chunk_command_end',0),
                ('chunk_split_begin',0),('chunk_split_end',0))):
            t = 1100000+i*200000
            rows.append(dict(a,kind=kind,value=value,wall_ns=t,cpu_ns=t//2))
        p = progress.command_phases(a,b,rows)
        self.assertTrue(p['qualified'])
        self.assertAlmostEqual(p['draw_run']['cpu_ms'],.1)
        self.assertAlmostEqual(p['non_draw']['cpu_ms'],.1)
        self.assertEqual(p['split_policy']['calls'],2)
        self.assertAlmostEqual(sum(p[k]['cpu_ms'] for k in
            ('draw_run','non_draw','split_policy','dispatch_residual')),1.0)
        self.assertFalse(progress.command_phases(dict(a,value=0),b,rows)['qualified'])
        self.assertFalse(progress.command_phases(a,b,[])['qualified'])
        for bad in (rows[:-1], rows[1:], rows+[rows[0]],
                    [dict(rows[0],tid=4)]+rows[1:],
                    [rows[0],dict(rows[1],value=0)]+rows[2:],
                    [rows[0],dict(rows[1],cpu_ns=1)]+rows[2:],
                    rows[:2]+rows[4:]):
            with self.assertRaises(ValueError):
                progress.command_phases(a,b,bad)

    def test_chunk_body_partition(self):
        a = dict(kind='framegraph_encode_begin', owner=99, id=7, value=3,
                 tid=3, wall_ns=1000000, cpu_ns=100000)
        b = dict(a, kind='framegraph_encode_end', wall_ns=2000000, cpu_ns=600000)
        binding = dict(a, kind='framegraph_pool_binding', value=88, wall_ns=999000)
        x = dict(a, kind='chunk_body_begin', owner=88, value=0,
                 wall_ns=1200000, cpu_ns=200000)
        y = dict(x, kind='chunk_body_end', wall_ns=1800000, cpu_ns=500000)
        phases = progress.chunk_phases(a, b, [binding], {(88,7):[x,y]})
        self.assertTrue(phases['qualified'])
        self.assertAlmostEqual(phases['setup_partition']['cpu_ms'], .1)
        self.assertAlmostEqual(phases['command_body']['cpu_ms'], .3)
        self.assertAlmostEqual(phases['finalize_return']['cpu_ms'], .1)
        self.assertFalse(progress.chunk_phases(a,b,[],{})['qualified'])
        for data in ({}, {(88,7):[x]}, {(89,7):[x,y]}, {(88,8):[x,y]}):
            self.assertFalse(progress.chunk_phases(a,b,[binding],data)['qualified'])
        for invalid in ([x,x,y], [dict(x,tid=4),y], [dict(x,wall_ns=1900000),y],
                        [x,dict(y,cpu_ns=1)], [x,dict(y,cpu_ns=10**12)]):
            with self.assertRaises(ValueError):
                progress.chunk_phases(a,b,[binding],{(88,7):invalid})
        for bindings in ([binding,binding], [dict(binding,value=0)],
                         [dict(binding,tid=4)], [dict(binding,wall_ns=1500000)]):
            with self.assertRaises(ValueError):
                progress.chunk_phases(a,b,bindings,{(88,7):[x,y]})

    def test_framegraph_partition(self):
        begin = dict(kind='encode_backend_begin', owner=2, id=7, value=99,
                     tid=3, wall_ns=1000000, cpu_ns=100000)
        end = dict(begin, kind='encode_backend_end', wall_ns=2000000, cpu_ns=600000)
        events = {r['kind']: r for r in (begin, end)}
        a = dict(begin, kind='framegraph_encode_begin', owner=99, value=2,
                 wall_ns=1200000, cpu_ns=200000)
        b = dict(a, kind='framegraph_encode_end', wall_ns=1800000, cpu_ns=500000)
        for disposition in (1, 2, 3):
            phases = progress.framegraph_phases(events, {(99,7):[
                dict(a, value=disposition), dict(b, value=disposition)]})
            self.assertTrue(phases['qualified'])
            self.assertAlmostEqual(phases['planning_setup']['cpu_ms'], .1)
            self.assertAlmostEqual(phases['encode_chunk']['cpu_ms'], .3)
            self.assertAlmostEqual(phases['cleanup_return']['cpu_ms'], .1)
        for candidates in ({}, {(99,7):[a]}, {(98,7):[a,b]}, {(99,8):[a,b]}):
            self.assertFalse(progress.framegraph_phases(events, candidates)['qualified'])
        self.assertFalse(progress.framegraph_phases({}, {})['qualified'])
        for invalid in ([a,a,b], [dict(a,tid=4),b], [a,dict(b,value=3)],
                        [dict(a,wall_ns=1900000),b], [a,dict(b,cpu_ns=1)],
                        [a,dict(b,cpu_ns=10**12)]):
            with self.assertRaises(ValueError):
                progress.framegraph_phases(events, {(99,7):invalid})

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
        standalone = progress.analyze(meta,rows,{'pid':17},[],source_only=True)
        self.assertIsNone(standalone['target'])
        self.assertEqual(len(standalone['target_sources']),1)
        with self.assertRaises(ValueError):
            progress.analyze(meta,rows,{'pid':17},[])
        with self.assertRaises(ValueError):
            progress.analyze(meta,rows,{'pid':17},[],query=54,source_only=True)
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
        nested = [dict(r, value=99) if r['kind']=='encode_backend_begin' else r
                  for r in observed]
        for kind, t in zip(progress.FRAMEGRAPH, (25_100_000, 25_300_000)):
            nested.append(dict(kind=kind, wall_ns=t, cpu_ns=t//2+1,
                               tid=3, owner=99, id=2, value=2, frame=2))
        nested.sort(key=lambda r: r['wall_ns'])
        fg = progress.analyze(meta, nested, {'pid':17}, [receipt])[
            'target_sources'][0]['framegraph_phases']
        self.assertTrue(fg['qualified'])
        self.assertAlmostEqual(fg['encode_chunk']['cpu_ms'], .1)
        self.assertFalse(fg['chunk_phases']['qualified'])
        for kind, owner, value, t in (
                ('framegraph_pool_binding',99,88,25_090_000),
                ('chunk_body_begin',88,0,25_150_000),
                ('chunk_body_end',88,0,25_250_000)):
            nested.append(dict(kind=kind, wall_ns=t, cpu_ns=t//2+1,
                               tid=3, owner=owner, id=2, value=value, frame=2))
        nested.sort(key=lambda r:r['wall_ns'])
        chunk = progress.analyze(meta,nested,{'pid':17},[receipt])[
            'target_sources'][0]['framegraph_phases']['chunk_phases']
        self.assertTrue(chunk['qualified'])
        self.assertAlmostEqual(chunk['command_body']['cpu_ms'], .05)
        detailed = [dict(r,value=1) if r['kind']=='chunk_body_begin' else r
                    for r in nested]
        for i,(kind,value) in enumerate((('chunk_command_begin',1),
                ('chunk_command_end',1),('chunk_split_begin',0),('chunk_split_end',0))):
            t = 25_160_000+i*10_000
            detailed.append(dict(kind=kind,wall_ns=t,cpu_ns=t//2+1,
                                  tid=3,owner=88,id=2,value=value,frame=2))
        detailed.sort(key=lambda r:r['wall_ns'])
        command = progress.analyze(meta,detailed,{'pid':17},[receipt])[
            'target_sources'][0]['framegraph_phases']['chunk_phases']['command_phases']
        self.assertTrue(command['qualified'])
        self.assertAlmostEqual(command['draw_run']['cpu_ms'], .005)
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
