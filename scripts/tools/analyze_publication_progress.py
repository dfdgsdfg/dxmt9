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
CHUNK_BODY = ('chunk_body_begin', 'chunk_body_end')
COMMAND_EVENTS = ('chunk_command_begin', 'chunk_command_end',
                  'chunk_split_begin', 'chunk_split_end')


def command_phases(begin, end, candidates):
    if begin.get('value') != 1:
        return dict(qualified=False, reason='command split disabled or source excluded')
    rows = sorted((r for r in candidates if r['kind'] in COMMAND_EVENTS and
                   begin['wall_ns'] <= r['wall_ns'] <= end['wall_ns']),
                  key=lambda r:r['wall_ns'])
    if not rows:
        return dict(qualified=False, reason='no command endpoints')
    totals = {k:dict(wall_ms=0.0, cpu_ms=0.0, calls=0)
              for k in ('draw_run', 'non_draw', 'split_policy')}
    active = None
    previous = begin
    for r in rows + [end]:
        if (r['tid'] != begin['tid'] or r['cpu_ns'] < previous['cpu_ns'] or
                r['wall_ns'] < previous['wall_ns']):
            raise ValueError('nonmonotone/cross-thread command trace')
        previous = r
        if r is end:
            break
        if r['kind'].endswith('_begin'):
            if active is not None:
                raise ValueError('overlapping command scopes')
            active = r
            continue
        if (active is None or r['kind'] != active['kind'].replace('_begin','_end') or
                r['value'] != active['value']):
            raise ValueError('unpaired command scope')
        if active['kind']=='chunk_command_begin' and active['value'] not in (0,1):
            raise ValueError('invalid command classification')
        if active['kind']=='chunk_split_begin' and active['value'] != 0:
            raise ValueError('invalid split classification')
        key = ('split_policy' if active['kind']=='chunk_split_begin' else
               'draw_run' if active['value'] else 'non_draw')
        wall, cpu = r['wall_ns']-active['wall_ns'], r['cpu_ns']-active['cpu_ns']
        if cpu > wall+100000:
            raise ValueError('invalid command CPU clock')
        totals[key]['wall_ms'] += wall/1e6
        totals[key]['cpu_ms'] += cpu/1e6
        totals[key]['calls'] += 1
        active = None
    if active is not None:
        raise ValueError('unclosed command scope')
    if totals['split_policy']['calls'] != totals['draw_run']['calls']+totals['non_draw']['calls']:
        raise ValueError('command/split coverage mismatch')
    residual = {k:(end[field]-begin[field])/1e6-sum(v[k] for v in totals.values())
                for k,field in (('wall_ms','wall_ns'),('cpu_ms','cpu_ns'))}
    if any(v < -1e-9 for v in residual.values()):
        raise ValueError('negative command dispatch residual')
    return dict(qualified=True, **totals, dispatch_residual=residual)


def chunk_phases(begin, end, bindings, chunk_events):
    if len(bindings) > 1:
        raise ValueError('duplicate framegraph pool binding')
    if not bindings:
        return dict(qualified=False, reason='missing pool binding')
    binding = bindings[0]
    if (not binding['value'] or binding['tid'] != begin['tid'] or
            binding['wall_ns'] > begin['wall_ns']):
        raise ValueError('invalid pool binding')
    local = [r for r in chunk_events.get((binding['value'], begin['id']), [])
             if begin['wall_ns'] <= r['wall_ns'] <= end['wall_ns']]
    counts = collections.Counter(r['kind'] for r in local)
    if any(counts[k] > 1 for k in CHUNK_BODY):
        raise ValueError('duplicate chunk body endpoints')
    if any(counts[k] != 1 for k in CHUNK_BODY):
        return dict(qualified=False, reason='missing chunk body endpoints')
    endpoints = (begin, next(r for r in local if r['kind']==CHUNK_BODY[0]),
                 next(r for r in local if r['kind']==CHUNK_BODY[1]), end)
    if len({r['tid'] for r in endpoints}) != 1:
        raise ValueError('chunk body endpoint thread mismatch')
    parts = []
    for x, y in zip(endpoints, endpoints[1:]):
        wall, cpu = y['wall_ns']-x['wall_ns'], y['cpu_ns']-x['cpu_ns']
        if wall < 0 or cpu < 0 or cpu > wall+100000:
            raise ValueError('invalid chunk body clocks/order')
        parts.append(dict(wall_ms=wall/1e6, cpu_ms=cpu/1e6))
    return dict(qualified=True,
        command_phases=command_phases(endpoints[1], endpoints[2], local), **dict(zip(
        ('setup_partition', 'command_body', 'finalize_return'), parts)))


def framegraph_phases(events, backend_events, chunk_events=None):
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
    bindings = [r for r in local if r['kind']=='framegraph_pool_binding']
    return dict(qualified=True, disposition=a['value'],
        chunk_phases=chunk_phases(a, b, bindings, chunk_events or {}), **dict(zip(
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
    chunk_events = collections.defaultdict(list)
    for r in rows:
        if r['kind'] in CHUNK_BODY + COMMAND_EVENTS:
            chunk_events[(r['owner'], r['id'])].append(r)
            continue
        if r['kind'] in FRAMEGRAPH + ('framegraph_pool_binding',):
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
            framegraph_phases=framegraph_phases(e, backend_events, chunk_events),
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
