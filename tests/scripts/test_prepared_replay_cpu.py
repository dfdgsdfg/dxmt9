import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'scripts/tools'))
import analyze_prepared_replay_cpu as probe


class PreparedReplayCpu(unittest.TestCase):
    def setUp(self):
        self.meta=dict(closed=1,overflow=0,incomplete=0,cpu_clock=1)
        self.rows=[]
        for phase,identity,begin,end in ((0,11,100,1000),(1,0,200,400),
                (2,0,220,380),(3,99,800,900)):
            for suffix,clock in (('begin',begin),('end',end)):
                self.rows.append(dict(kind='prepared_replay_phase_'+suffix,
                    owner=1,tid=2,id=identity,value=phase,wall_ns=clock*1000,cpu_ns=clock*500))

    def test_exact_partition(self):
        r=probe.analyze(self.meta,self.rows,1)
        self.assertEqual(r['raw_envelope_residual_cpu_ms'],0.02)
        self.assertEqual(r['interval_other_cpu_ms'],0.3)
        self.assertEqual(r['interval_excluding_readback_cpu_ms'],0.4)

    def test_missing_each_endpoint(self):
        for i in range(len(self.rows)):
            with self.subTest(i=i), self.assertRaises(ValueError):
                probe.analyze(self.meta,self.rows[:i]+self.rows[i+1:],1)

    def test_identity_thread_and_clock_negatives(self):
        for field,value in (('owner',7),('tid',7),('id',7),('value',9),('cpu_ns',0),('wall_ns',0)):
            rows=copy.deepcopy(self.rows)
            rows[5][field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):
                probe.analyze(self.meta,rows,1)

    def test_metadata_and_count(self):
        for field,value in (('closed',0),('overflow',1),('incomplete',1),('cpu_clock',0)):
            meta=dict(self.meta,**{field:value})
            with self.subTest(field=field),self.assertRaises(ValueError):
                probe.analyze(meta,self.rows,1)
        with self.assertRaises(ValueError):
            probe.analyze(self.meta,self.rows,2)

    def test_overlapping_readback(self):
        rows=copy.deepcopy(self.rows)
        rows[6].update(wall_ns=300000,cpu_ns=150000)
        with self.assertRaises(ValueError):
            probe.analyze(self.meta,rows,1)

    def test_duplicate_interval_or_raw(self):
        for indices in ((0,1),(2,3),(4,5)):
            with self.subTest(indices=indices),self.assertRaises(ValueError):
                probe.analyze(self.meta,self.rows+[self.rows[i] for i in indices],1)

    def test_readback_must_be_terminal(self):
        rows=copy.deepcopy(self.rows)
        rows[6].update(wall_ns=110000,cpu_ns=55000)
        rows[7].update(wall_ns=150000,cpu_ns=75000)
        with self.assertRaises(ValueError):
            probe.analyze(self.meta,rows,1)

    def test_child_escapes_envelope(self):
        rows=copy.deepcopy(self.rows)
        rows[4].update(wall_ns=190000,cpu_ns=95000)
        with self.assertRaises(ValueError):
            probe.analyze(self.meta,rows,1)

    def provider_rows(self):
        return [dict(kind='provider_replay_phase', owner=3, tid=2, id=42,
            value=i, wall_ns=t*1000, cpu_ns=t*500)
            for i,t in enumerate((230,250,340,370))]

    def test_provider_partition(self):
        p=probe.analyze(self.meta,self.rows+self.provider_rows(),1,True)['provider']
        self.assertEqual(p['preparation_cpu_ms'],0.01)
        self.assertEqual(p['planned_replay_cpu_ms'],0.045)
        self.assertEqual(p['settlement_cpu_ms'],0.015)
        self.assertEqual(p['call_boundary_residual_cpu_ms'],0.01)
        self.assertEqual(p['raws'][0]['event_ordinal'],42)

    def test_provider_missing_endpoints(self):
        with self.assertRaises(ValueError):
            probe.analyze(self.meta,self.rows,1,True)
        providers=self.provider_rows()
        for i in range(4):
            with self.subTest(i=i),self.assertRaises(ValueError):
                probe.analyze(self.meta,self.rows+providers[:i]+providers[i+1:],1)

    def test_provider_bad_identity_clock_or_thread(self):
        for field,value in (('owner',9),('id',0),('tid',7),('value',9),
                ('wall_ns',210000),('cpu_ns',0)):
            providers=self.provider_rows()
            providers[1][field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):
                probe.analyze(self.meta,self.rows+providers,1,True)

    def test_provider_duplicate_or_outside(self):
        providers=self.provider_rows()
        with self.assertRaises(ValueError):
            probe.analyze(self.meta,self.rows+providers+[providers[0]],1,True)
        providers[-1].update(wall_ns=390000,cpu_ns=195000)
        with self.assertRaises(ValueError):
            probe.analyze(self.meta,self.rows+providers,1,True)


if __name__=='__main__':
    unittest.main()
