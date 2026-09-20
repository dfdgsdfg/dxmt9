#!/usr/bin/env python3
"""Join receipt prefixes to replay service and nested queue-space observations.

Whole raw service spans, not wait-overlap CPU or removable frame cost. Queue
publication is attributed to its caller interval, not exclusive Slot content.
"""
import argparse
import bisect
import collections
import json
from pathlib import Path

import analyze_cpu_supply as supply
import summarize_wait_trace as wait


def join(wait_meta, wait_rows, supply_meta, rows):
    if supply_meta.get('pid') != wait_meta['pid']:
        raise ValueError('companion process identity mismatch or absent')
    presents = [r for r in rows if r['kind'] == 'present']
    if [r['id'] for r in presents] != list(range(supply_meta['first'], supply_meta['last']+1)):
        raise ValueError('missing companion window endpoints')
    if len(presents) < 3:
        raise ValueError('insufficient interior window')
    interior = (presents[1]['wall_ns'], presents[-2]['wall_ns'])
    bindings = {}
    for r in rows:
        if r['kind'] != 'replay_ledger':
            continue
        key = (r['owner'], r['tid'], r['id'])
        if not r['value'] or key in bindings:
            raise ValueError('unknown or duplicate replay ledger binding')
        bindings[key] = r['value']
    replay = supply.pair(rows, 'replay_begin', 'replay_end', interior=interior)
    space = (supply.pair(rows, 'writer_wait_begin', 'writer_wait_end', interior=interior) +
             supply.pair(rows, 'commit_wait_begin', 'commit_wait_end', interior=interior))
    pubs_by_tid, waits_by_tid = collections.defaultdict(list), collections.defaultdict(list)
    for r in rows:
        if r['kind'] == 'publish':
            pubs_by_tid[r['tid']].append(r)
    for s in space:
        waits_by_tid[s['begin']['tid']].append(s)
    for values in waits_by_tid.values():
        values.sort(key=lambda s: s['begin']['wall_ns'])
    pub_times = {tid: [r['wall_ns'] for r in values] for tid, values in pubs_by_tid.items()}
    wait_times = {tid: [s['begin']['wall_ns'] for s in values] for tid, values in waits_by_tid.items()}
    by_raw = {}
    for s in replay:
        a, b = s['begin'], s['end']
        ledger = bindings.get((a['owner'], a['tid'], a['id']))
        if ledger is None:
            continue
        key = (ledger, a['id'])
        if key in by_raw:
            raise ValueError('ambiguous raw incarnation')
        tid = a['tid']
        def select(values, times):
            clocks = times.get(tid, [])
            return values.get(tid, [])[bisect.bisect_left(clocks, a['wall_ns']):
                                       bisect.bisect_right(clocks, b['wall_ns'])]
        nested = [w for w in select(waits_by_tid, wait_times)
                  if w['end']['wall_ns'] <= b['wall_ns']]
        pubs = select(pubs_by_tid, pub_times)
        by_raw[key] = dict(raw=a['id'], worker_tid=a['tid'],
            wall_ns=s['wall_ns'], cpu_ns=s['cpu_ns'],
            queue_wait_ns=sum(w['wall_ns'] for w in nested),
            publications=[dict(queue_owner=r['owner'], sequence=r['id'],
                               declared_raw=r['value']) for r in pubs])
    result = []
    for r in wait_rows:
        if r['site'] != 0 or not r['waits']:
            continue
        first, last = r['completed_before'] + 1, r['raw_before']
        if last < first:
            raise ValueError('pending receipt without an outstanding raw prefix')
        matched = [v for (owner, raw), v in by_raw.items()
                   if owner == r['owner'] and first <= raw <= last]
        target = by_raw.get((r['owner'], last))
        result.append(dict(query=r['query'], owner=r['owner'],
            frame=r['first_frame'], first_raw=first, target_raw=last,
            receipt_ns=r['end_ns']-r['acquired_ns'],
            matched_raw_count=len(matched), target=target,
            observed_prefix_wall_ns=sum(v['wall_ns'] for v in matched),
            observed_prefix_cpu_ns=sum(v['cpu_ns'] for v in matched),
            observed_prefix_queue_wait_ns=sum(v['queue_wait_ns'] for v in matched),
            qualification='Observed whole raw spans; missing/clipped raw work remains unknown'))
    return dict(receipts=result, matched_targets=sum(r['target'] is not None for r in result),
        receipt_count=len(result),
        qualification='Identity join, not exclusive wait ownership or additive CPU/FPS costs')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('waits', type=Path)
    parser.add_argument('supply', type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(join(*wait.load(args.waits), *supply.load(args.supply)), indent=2))
    except (ValueError, KeyError, OSError) as error:
        parser.exit(1, f'unqualified join: {error}\n')
