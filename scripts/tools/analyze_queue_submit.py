#!/usr/bin/env python3
"""Sequential source-qualified submit costs; observer-inclusive, not FPS proof."""
import argparse
import json
from pathlib import Path
from analyze_cpu_supply import load

PHASES = {1: 'validation_and_summary', 2: 'lifecycle_and_prepare',
          3: 'capture_and_signpost', 4: 'driver_commit',
          5: 'post_commit_and_watchdog', 6: 'pending_mutex_acquire',
          7: 'pending_handoff_and_unlock'}


def analyze(rows):
    groups = {}
    validation_ends = {}
    families = []
    for row in rows:
        key = row['owner'], row['tid'], row['id']
        if row['kind'] == 'queue_submit_validation_end':
            if key in validation_ends:
                raise ValueError('duplicate validation endpoint')
            validation_ends[key] = row
        if row['kind'] == 'queue_submit_summary_family':
            if row['value'] not in (1, 2, 3):
                raise ValueError('unknown summary family')
            families.append(row)
        if row['kind'] == 'queue_submit_phase':
            if row['value'] not in (0, *PHASES):
                raise ValueError('unknown submit phase')
            groups.setdefault((row['owner'], row['tid'], row['id']), []).append(row)
    complete, clipped = [], 0
    for key, events in groups.items():
        values = [r['value'] for r in events]
        if len(set(values)) != len(values):
            raise ValueError('duplicate submit phase/identity')
        if values != sorted(values, key=lambda v: v or 8):
            raise ValueError('reversed submit phase')
        if values != [1, 2, 3, 4, 5, 6, 7, 0]:
            if values[0] == 1 and values[-1] == 0:
                raise ValueError('missing interior submit phase')
            clipped += 1  # Includes rejected and no-completion-binding paths.
            continue
        spans = []
        for a, b in zip(events, events[1:]):
            wall, cpu = b['wall_ns']-a['wall_ns'], b['cpu_ns']-a['cpu_ns']
            if wall < 0 or cpu < 0 or cpu > wall + 100000:
                raise ValueError('invalid submit clocks')
            spans.append(dict(phase=PHASES[a['value']], wall_ns=wall, cpu_ns=cpu,
                              begin_ns=a['wall_ns'], end_ns=b['wall_ns']))
        split = None
        boundary = validation_ends.get(key)
        if boundary:
            start, end = events[:2]
            if not (start['wall_ns'] <= boundary['wall_ns'] <= end['wall_ns']
                    and start['cpu_ns'] <= boundary['cpu_ns'] <= end['cpu_ns']):
                raise ValueError('validation endpoint outside first phase')
            members = [f for f in families if (f['owner'], f['tid']) == key[:2]
                       and boundary['wall_ns'] <= f['wall_ns'] <= end['wall_ns']]
            split = dict(validation_wall_ns=boundary['wall_ns']-start['wall_ns'],
                         validation_cpu_ns=boundary['cpu_ns']-start['cpu_ns'],
                         summary_wall_ns=end['wall_ns']-boundary['wall_ns'],
                         summary_cpu_ns=end['cpu_ns']-boundary['cpu_ns'],
                         expected_sources=boundary['value'],
                         families=[dict(source=f['id'], family={1:'legacy',2:'arena',3:'receipt'}[f['value']])
                                   for f in members],
                         family_coverage_complete=len(members)==boundary['value'])
        complete.append(dict(owner=key[0], tid=key[1], source=key[2], phases=spans,
                             validation_summary_split=split))
    return dict(sources=complete, incomplete_or_rejected_sources=clipped,
                limitations='Observer-inclusive. Null/test, failed, unbound and clipped paths excluded. No exclusive mutex/FPS attribution.')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('trace', type=Path)
    p.add_argument('--output', type=Path)
    args = p.parse_args()
    meta, rows = load(args.trace)
    result = json.dumps(dict(metadata=meta, **analyze(rows)), indent=2)+'\n'
    if args.output:
        args.output.write_text(result)
    else:
        print(result, end='')
