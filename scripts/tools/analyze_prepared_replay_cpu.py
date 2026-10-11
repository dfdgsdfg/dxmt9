#!/usr/bin/env python3
"""Cold V4 caller CPU scopes, not production ReplayOffloadWorker or FPS."""
import argparse
import json
from pathlib import Path

import analyze_cpu_supply as supply

KINDS = ('interval', 'raw_envelope', 'raw_replay_call', 'readback_oracle')


def analyze(meta, rows, expected_raw_count):
    if (meta.get('closed'), meta.get('overflow'), meta.get('incomplete'),
            meta.get('cpu_clock')) != (1, 0, 0, 1):
        raise ValueError('incomplete/unqualified trace')
    if expected_raw_count <= 0:
        raise ValueError('expected raw count must be positive')
    active, spans = {}, []
    for r in sorted(rows, key=lambda r: r['wall_ns']):
        if r['kind'] not in ('prepared_replay_phase_begin', 'prepared_replay_phase_end'):
            continue
        if r['value'] not in range(len(KINDS)):
            raise ValueError('unknown prepared phase')
        key = (r['owner'], r['id'], r['value'])
        if r['kind'].endswith('_begin'):
            if key in active:
                raise ValueError('duplicate prepared begin')
            active[key] = r
            continue
        if key not in active:
            raise ValueError('unmatched prepared end')
        a = active.pop(key)
        wall, cpu = r['wall_ns']-a['wall_ns'], r['cpu_ns']-a['cpu_ns']
        if a['tid'] != r['tid'] or min(wall, cpu) < 0 or cpu > wall+100000:
            raise ValueError('invalid prepared clocks/thread')
        spans.append(dict(owner=r['owner'], tid=r['tid'], id=r['id'], phase=r['value'],
            begin=a['wall_ns'], end=r['wall_ns'], cpu_begin=a['cpu_ns'], cpu_end=r['cpu_ns'],
            wall_ns=wall, cpu_ns=cpu))
    if active:
        raise ValueError('unclosed prepared scope')
    roots = [s for s in spans if s['phase']==0]
    if len(roots)!=1:
        raise ValueError('expected one original interval')
    root = roots[0]

    def enclosed(parent, child):
        return (parent['owner']==child['owner'] and parent['tid']==child['tid'] and
            parent['begin'] <= child['begin'] <= child['end'] <= parent['end'] and
            parent['cpu_begin'] <= child['cpu_begin'] <= child['cpu_end'] <= parent['cpu_end'])

    if any(not enclosed(root,s) for s in spans):
        raise ValueError('prepared scope outside original interval')
    envelopes = sorted((s for s in spans if s['phase']==1), key=lambda s:s['id'])
    calls = sorted((s for s in spans if s['phase']==2), key=lambda s:s['id'])
    expected_ids = list(range(expected_raw_count))
    if [s['id'] for s in envelopes]!=expected_ids or [s['id'] for s in calls]!=expected_ids:
        raise ValueError('missing/duplicate/noncanonical raw identity')
    if any(not enclosed(p,c) for p,c in zip(envelopes,calls)):
        raise ValueError('replay call escapes raw envelope')
    readbacks = [s for s in spans if s['phase']==3]
    if len(readbacks)!=1:
        raise ValueError('expected one terminal readback oracle')
    siblings = sorted(envelopes+readbacks, key=lambda s:s['begin'])
    if siblings[-1] is not readbacks[0]:
        raise ValueError('readback oracle precedes original raws')
    if any(a['end']>b['begin'] or a['cpu_end']>b['cpu_begin'] for a,b in zip(siblings,siblings[1:])):
        raise ValueError('overlapping sibling scopes')
    totals = {name:dict(calls=sum(s['phase']==i for s in spans),
        cpu_ms=sum(s['cpu_ns'] for s in spans if s['phase']==i)/1e6,
        wall_ms=sum(s['wall_ns'] for s in spans if s['phase']==i)/1e6)
        for i,name in enumerate(KINDS)}
    envelope_cpu = sum(s['cpu_ns'] for s in envelopes)
    call_cpu = sum(s['cpu_ns'] for s in calls)
    other_cpu = root['cpu_ns']-envelope_cpu-sum(s['cpu_ns'] for s in readbacks)
    if envelope_cpu < call_cpu or other_cpu < 0:
        raise ValueError('negative prepared residual')
    return dict(scope_complete=True, owner=root['owner'], tid=root['tid'], phases=totals,
        raw_envelope_residual_cpu_ms=(envelope_cpu-call_cpu)/1e6,
        interval_other_cpu_ms=other_cpu/1e6,
        interval_excluding_readback_cpu_ms=(root['cpu_ns']-readbacks[0]['cpu_ns'])/1e6,
        raws=[dict(id=e['id'], envelope_cpu_ms=e['cpu_ns']/1e6,
                   replay_call_cpu_ms=c['cpu_ns']/1e6) for e,c in zip(envelopes,calls)],
        qualifications='Clock/scope coverage only. Caller-thread CPU includes V4 qualification and original interval mutations/definitions/control. Excludes setup, finish and cleanup. Not worker CPU, exclusive renderer work, GPU correctness, FPS or a removable budget.')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('trace', type=Path)
    p.add_argument('--expected-raw-count', required=True, type=int)
    args=p.parse_args()
    meta,rows=supply.load(args.trace)
    print(json.dumps(analyze(meta,rows,args.expected_raw_count),indent=2))


if __name__=='__main__':
    main()
