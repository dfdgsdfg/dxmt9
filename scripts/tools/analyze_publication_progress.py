#!/usr/bin/env python3
"""Ordinary 1:1 source progress, not GPU timestamps or additive frame costs."""
import argparse
import collections
import json
from pathlib import Path

import analyze_cpu_supply as supply
import join_replay_wait_trace as joiner
import summarize_wait_trace as waits

STAGES = ('publish', 'encode_begin', 'encode_end', 'submit',
          'completion_dequeue', 'completion_observed', 'retire')
PHASES = ('encode_prefetch_begin', 'encode_prefetch_end',
          'encode_backend_begin', 'encode_backend_end')
FRAMEGRAPH = ('framegraph_encode_begin', 'framegraph_encode_end')


def framegraph_phases(events, backend_events):
    begin, end = events.get('encode_backend_begin'), events.get('encode_backend_end')
    if not begin or not end or not begin.get('value'):
        return dict(qualified=False, reason='missing ordinary backend binding')
    candidates = backend_events.get((begin['value'], begin['id']), [])
    # Backend object/sequence alone is not unique across shared-session calls.
    # Require one pair fully nested in this queue/TID-bound ordinary call.
    local = [r for r in candidates if begin['wall_ns'] <= r['wall_ns'] <= end['wall_ns']]
    counts = collections.Counter(r['kind'] for r in local)
    if any(counts[k] > 1 for k in FRAMEGRAPH):
        raise ValueError('duplicate framegraph encode endpoints')
    if any(counts[k] != 1 for k in FRAMEGRAPH):
        return dict(qualified=False, reason='missing nested framegraph encode endpoints')
    a = next(r for r in local if r['kind']==FRAMEGRAPH[0])
    b = next(r for r in local if r['kind']==FRAMEGRAPH[1])
    if a['value'] not in (1, 2, 3) or a['value'] != b['value']:
        raise ValueError('invalid framegraph disposition')
    endpoints = (begin, a, b, end)
    if len({r['tid'] for r in endpoints}) != 1:
        raise ValueError('framegraph endpoint thread mismatch')
    parts = []
    for x, y in zip(endpoints, endpoints[1:]):
        wall, cpu = y['wall_ns']-x['wall_ns'], y['cpu_ns']-x['cpu_ns']
        if wall < 0 or cpu < 0 or cpu > wall+100000:
            raise ValueError('invalid framegraph endpoint clocks/order')
        parts.append(dict(wall_ms=wall/1e6, cpu_ms=cpu/1e6))
    return dict(qualified=True, disposition=a['value'], **dict(zip(
        ('planning_setup', 'encode_chunk', 'cleanup_return'), parts)))


def encode_phases(events):
    """Disjoint intervals on one encode thread, never aggregate counter deltas."""
    missing = [name for name in PHASES if name not in events]
    if missing:
        return dict(qualified=False, missing=missing)
    names = ('encode_begin',) + PHASES + ('encode_end',)
    endpoints = [events[name] for name in names]
    if len({r['tid'] for r in endpoints}) != 1:
        raise ValueError('encode phase thread mismatch')
    intervals = []
    for a, b in zip(endpoints, endpoints[1:]):
        wall, cpu = b['wall_ns']-a['wall_ns'], b['cpu_ns']-a['cpu_ns']
        if wall < 0 or cpu < 0 or cpu > wall+100000:
            raise ValueError('invalid encode phase clocks/order')
        intervals.append(dict(wall_ms=wall/1e6, cpu_ms=cpu/1e6))
    return dict(qualified=True, **dict(zip(
        ('source_prepare', 'prefetch', 'context_prepare', 'backend', 'return_relock'),
        intervals)))


def analyze(meta, rows, wait_meta, wait_rows, query=None):
    joined = joiner.join(wait_meta, wait_rows, meta, rows)
    pending = [r for r in joined['receipts'] if r['target'] and
               (query is None or r['query'] == query)]
    if not pending:
        raise ValueError('no matched pending receipt')
    target = max(pending, key=lambda r: r['receipt_ns'])
    events = collections.defaultdict(dict)
    backend_events = collections.defaultdict(list)
    for r in rows:
        if r['kind'] in FRAMEGRAPH:
            backend_events[(r['owner'], r['id'])].append(r)
            continue
        if r['kind'] not in STAGES + PHASES + ('publish_reason', 'publish_commands'):
            continue
        key = (r['owner'], r['id'])
        if r['kind'] in events[key]:
            raise ValueError('duplicate ordinary source stage; shared CB unsupported')
        events[key][r['kind']] = r
    sources = []
    for key, e in events.items():
        if not all(s in e for s in STAGES):
            continue
        if any(e[b]['wall_ns'] < e[a]['wall_ns'] for a, b in zip(STAGES, STAGES[1:])):
            raise ValueError('nonmonotonic source progress')
        begin, end = e['encode_begin'], e['encode_end']
        encode_cpu = end['cpu_ns']-begin['cpu_ns']
        if (begin['tid'] != end['tid'] or encode_cpu < 0 or
                encode_cpu > end['wall_ns']-begin['wall_ns']+100000):
            raise ValueError('encode CPU endpoints must share a monotone thread clock')
        sources.append(dict(owner=key[0], sequence=key[1],
            reason=e.get('publish_reason', {}).get('value'),
            commands=e.get('publish_commands', {}).get('value'),
            encode_cpu_ms=encode_cpu/1e6,
            encode_phases=encode_phases(e),
            framegraph_phases=framegraph_phases(e, backend_events),
            stages_ms={a+'__'+b: (e[b]['wall_ns']-e[a]['wall_ns'])/1e6
                       for a, b in zip(STAGES, STAGES[1:])}))
    keys = {(r['queue_owner'], r['sequence']) for r in target['target']['publications']}
    target_sources = [s for s in sources if (s['owner'], s['sequence']) in keys]
    spans = supply.pair(rows, 'replay_begin', 'replay_end')
    span = next(s for s in spans if s['begin']['id'] == target['target_raw'] and
                s['begin']['tid'] == target['target']['worker_tid'])
    a, b = span['begin']['wall_ns'], span['end']['wall_ns']
    local = [r for r in rows if r['tid'] == span['begin']['tid'] and a <= r['wall_ns'] <= b]
    issues = [dict(query=r['id'], flags=r['value']) for r in local if r['kind']=='query_issue']
    scopes = [dict(sequence=r['id'], published=bool(r['value'] >> 63),
                   before=(r['value'] >> 32) & 0x7fffffff, after=r['value'] & 0xffffffff)
              for r in local if r['kind']=='scope_boundary']
    blocked = supply.pair(rows, 'commit_wait_begin', 'commit_wait_end')
    relief = []
    for s in blocked:
        x, y = s['begin'], s['end']
        if x['tid'] != span['begin']['tid'] or not a <= x['wall_ns'] <= y['wall_ns'] <= b:
            continue
        retired = [r['id'] for r in rows if r['kind']=='retire' and r['owner']==x['owner'] and
                   x['wall_ns'] <= r['wall_ns'] <= y['wall_ns']]
        relief.append(dict(wall_ms=s['wall_ns']/1e6, before=x['value'], after=y['value'],
                           retirements_in_interval=retired))
    return dict(target=target, target_sources=target_sources,
        target_missing_source_stages=len(keys)-len(target_sources),
        query_issues=issues, scope_boundaries=scopes, cap_waits=relief,
        complete_sources=sources, incomplete_source_count=len(events)-len(sources),
        qualification='Ordinary source stages only; encode CPU uses same-thread endpoints and includes prefetch/backend work, not only draw encoding; retirement in wait interval is not OS wake identity; elapsed stages across sources overlap')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('supply', type=Path)
    p.add_argument('waits', type=Path)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--query', type=int, help='Select a recorded query identity; absent means longest receipt')
    a = p.parse_args()
    try:
        result = analyze(*supply.load(a.supply), *waits.load(a.waits), query=a.query)
        with a.output.open('x') as stream:
            stream.write(json.dumps(result, indent=2)+'\n')
        print(json.dumps({k:v for k,v in result.items() if k not in ('complete_sources','target_sources','target')}, indent=2))
    except (ValueError, KeyError, OSError, StopIteration) as error:
        p.exit(1, f'unqualified progress: {error}\n')
