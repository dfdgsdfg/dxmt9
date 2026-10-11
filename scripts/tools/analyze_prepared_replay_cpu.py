#!/usr/bin/env python3
"""Cold V4 caller CPU scopes, not production ReplayOffloadWorker or FPS."""
import argparse
import json
from pathlib import Path

import analyze_cpu_supply as supply

KINDS = ('interval', 'raw_envelope', 'raw_replay_call', 'readback_oracle')


def provider_partition(rows, calls, required):
    groups = {}
    for r in rows:
        if r['kind'] != 'provider_replay_phase':
            continue
        key = (r['owner'], r['id'])
        phases = groups.setdefault(key, {})
        if r['value'] not in range(4) or r['value'] in phases:
            raise ValueError('unknown/duplicate provider phase')
        phases[r['value']] = r
    if not groups:
        if required:
            raise ValueError('missing provider phases')
        return None
    if len(groups) != len(calls) or len({owner for owner, _ in groups}) != 1:
        raise ValueError('provider owner/count mismatch')
    joined, totals = {}, [0, 0, 0]
    for (_, event), phases in groups.items():
        if set(phases) != set(range(4)) or event <= 0:
            raise ValueError('incomplete provider phases/identity')
        points = [phases[i] for i in range(4)]
        a, b = points[0], points[-1]
        matches = [c for c in calls if c['tid'] == a['tid'] and
            c['begin'] <= a['wall_ns'] <= b['wall_ns'] <= c['end'] and
            c['cpu_begin'] <= a['cpu_ns'] <= b['cpu_ns'] <= c['cpu_end']]
        if len(matches) != 1 or matches[0]['id'] in joined:
            raise ValueError('provider does not uniquely nest in raw replay call')
        deltas = []
        for p, q in zip(points, points[1:]):
            wall, cpu = q['wall_ns']-p['wall_ns'], q['cpu_ns']-p['cpu_ns']
            if p['tid'] != q['tid'] or min(wall, cpu) < 0 or cpu > wall+100000:
                raise ValueError('invalid provider clocks/thread/order')
            deltas.append(cpu)
        joined[matches[0]['id']] = dict(event_ordinal=event,
            preparation_cpu_ms=deltas[0]/1e6, planned_replay_cpu_ms=deltas[1]/1e6,
            settlement_cpu_ms=deltas[2]/1e6)
        totals = [x+y for x,y in zip(totals,deltas)]
    residual = sum(c['cpu_ns'] for c in calls)-sum(totals)
    if residual < 0:
        raise ValueError('negative provider residual')
    return dict(calls=len(joined), preparation_cpu_ms=totals[0]/1e6,
        planned_replay_cpu_ms=totals[1]/1e6, settlement_cpu_ms=totals[2]/1e6,
        call_boundary_residual_cpu_ms=residual/1e6,
        raws=[dict(id=i, **joined[i]) for i in sorted(joined)],
        qualifications='Planned replay still includes V4 per-record qualification. Settlement includes publication, ACK and snapshot/coverage. Boundary residual includes import and scope/resource destruction. No exclusive renderer or removable-cost claim.')


def slot_validation_partition(rows, provider, required):
    kinds = ('slot_validation_extent', 'slot_validation_begin', 'slot_validation_end')
    attempts = {}
    for r in rows:
        if r['kind'] not in kinds:
            continue
        group = attempts.setdefault(r['id'], {})
        if r['kind'] in group or r['id'] <= 0 or r['owner'] == 0:
            raise ValueError('duplicate/invalid Slot validation identity')
        group[r['kind']] = r
    if not attempts:
        if required:
            raise ValueError('missing Slot validation scopes')
        return None
    if provider is None:
        raise ValueError('Slot validation requires provider scopes')
    phases = {}
    for r in rows:
        if r['kind'] == 'provider_replay_phase':
            phases.setdefault(r['id'], {})[r['value']] = r
    spans = []
    for identity, group in attempts.items():
        if set(group) != set(kinds):
            raise ValueError('incomplete Slot validation scope')
        extent, a, b = (group[k] for k in kinds)
        if len({(p['owner'], p['tid']) for p in (extent, a, b)}) != 1:
            raise ValueError('Slot validation owner/thread mismatch')
        if min(extent['value'], a['value']) < 0 or b['value'] not in (0, 1):
            raise ValueError('invalid Slot validation extent/result')
        for p, q in ((extent, a), (a, b)):
            wall, cpu = q['wall_ns']-p['wall_ns'], q['cpu_ns']-p['cpu_ns']
            if min(p['wall_ns'], p['cpu_ns'], wall, cpu) < 0 or cpu > wall+100000:
                raise ValueError('invalid Slot validation clock/order')
        matches = [r for r in provider['raws']
            if phases[r['event_ordinal']][1]['tid'] == a['tid'] and
            phases[r['event_ordinal']][1]['wall_ns'] <= extent['wall_ns'] <= b['wall_ns'] <= phases[r['event_ordinal']][2]['wall_ns'] and
            phases[r['event_ordinal']][1]['cpu_ns'] <= extent['cpu_ns'] <= b['cpu_ns'] <= phases[r['event_ordinal']][2]['cpu_ns']]
        if len(matches) != 1:
            raise ValueError('Slot validation does not uniquely nest in planned replay')
        spans.append(dict(id=identity, raw_id=matches[0]['id'],
            event_ordinal=matches[0]['event_ordinal'], owner=a['owner'], tid=a['tid'],
            begin=extent['wall_ns'], end=b['wall_ns'],
            cpu_begin=extent['cpu_ns'], cpu_end=b['cpu_ns'],
            cpu_ms=(b['cpu_ns']-a['cpu_ns'])/1e6,
            headers=a['value'], stored_params=extent['value'], accepted=b['value']))
    spans.sort(key=lambda s:s['begin'])
    if any(a['end'] > b['begin'] or a['cpu_end'] > b['cpu_begin']
           for a,b in zip(spans,spans[1:])):
        raise ValueError('overlapping Slot validation scopes')

    def summarize(items):
        return dict(calls=len(items), accepted=sum(s['accepted'] for s in items),
            rejected=sum(not s['accepted'] for s in items),
            cpu_ms=sum(s['cpu_ms'] for s in items),
            accepted_command_visits=sum(s['headers'] for s in items if s['accepted']),
            stored_param_extent_sum=sum(s['stored_params'] for s in items),
            max_command_extent=max((s['headers'] for s in items),default=0))
    return dict(**summarize(spans),
        raws=[dict(id=r['id'],event_ordinal=r['event_ordinal'],
                   **summarize([s for s in spans if s['raw_id']==r['id']]))
              for r in provider['raws']],
        qualifications='Observer-inclusive CPU, not removable cost or FPS. Successful command visits are exact; stored DrawParam extents are not visits. Slot address is diagnostic only, not lifetime identity. Only synchronous observed provider planned replay is covered.')


def analyze(meta, rows, expected_raw_count, require_provider=False, require_slot_validation=False):
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
    provider = provider_partition(rows, calls, require_provider)
    return dict(scope_complete=True, owner=root['owner'], tid=root['tid'], phases=totals,
        provider=provider,
        slot_validation=slot_validation_partition(rows, provider, require_slot_validation),
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
    p.add_argument('--require-provider', action='store_true')
    p.add_argument('--require-slot-validation', action='store_true')
    args=p.parse_args()
    meta,rows=supply.load(args.trace)
    print(json.dumps(analyze(meta,rows,args.expected_raw_count,args.require_provider,
                            args.require_slot_validation),indent=2))


if __name__=='__main__':
    main()
