#!/usr/bin/env python3
"""Source-qualified sequential planning phases; CPU service, not FPS attribution."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from analyze_cpu_supply import load

PHASES = {1: 'observer', 2: 'graph', 3: 'lookahead', 4: 'natural_frontier',
          5: 'optimizer', 6: 'diagnostics_prefix', 7: 'replay_selection'}


def analyze(rows):
    active, complete = {}, []
    clipped = aborted = 0
    for row in rows:
        if row['kind'] != 'framegraph_planning_phase':
            continue
        key = (row['owner'], row['tid'], row['id'])
        phase = row['value']
        if phase not in (0, 8, *PHASES):
            raise ValueError('unknown planning phase')
        if phase == 1:
            if key in active:
                raise ValueError('duplicate planning start')
            active[key] = (row, [])
            continue
        if key not in active:
            # Bypass sources emit only phase 0; a leading nonzero event means
            # the bounded observer window clipped the source's start.
            clipped += phase != 0
            continue
        if phase == 8:
            del active[key]
            aborted += 1
            continue
        previous, spans = active[key]
        if phase and phase <= previous['value']:
            raise ValueError('duplicate or reversed planning phase')
        if ((phase and phase != previous['value'] + 1) or
                (phase == 0 and previous['value'] not in (1, 7))):
            raise ValueError('missing interior planning phase')
        wall = row['wall_ns'] - previous['wall_ns']
        cpu = row['cpu_ns'] - previous['cpu_ns']
        if wall < 0 or cpu < 0 or cpu > wall + 100000:
            raise ValueError('invalid planning clocks')
        spans.append(dict(phase=PHASES[previous['value']], wall_ns=wall, cpu_ns=cpu))
        if phase == 0:
            del active[key]
            complete.append(dict(owner=key[0], tid=key[1], source=key[2],
                                 cpu_ns=sum(s['cpu_ns'] for s in spans),
                                 wall_ns=sum(s['wall_ns'] for s in spans), phases=spans))
        else:
            active[key] = (row, spans)
    complete.sort(key=lambda s: s['cpu_ns'], reverse=True)
    return dict(sources=complete, clipped_events=clipped, aborted_sources=aborted,
                incomplete_sources=len(active),
                exclusions='Wrapper/alias setup and encode excluded; no FPS inference.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    meta, rows = load(args.trace)
    if not any(r['kind'] == 'framegraph_planning_phase' for r in rows):
        raise ValueError('planning split absent; no cost attribution available')
    result = json.dumps(dict(trace=str(args.trace), metadata=meta, **analyze(rows)), indent=2)
    if args.output:
        args.output.write_text(result + '\n')
    else:
        print(result)


if __name__ == '__main__':
    main()
