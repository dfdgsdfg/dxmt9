#!/usr/bin/env python3
"""Publication shadow costs, not execution eligibility or FPS evidence."""
import argparse
import collections
import json
from pathlib import Path

import analyze_cpu_supply as supply

ORDER = ('begin source_index source_generation storage_generation storage_pages '
         'allocated appended sealed status identities accesses ranges unresolved end').split()


def analyze(rows):
    groups = collections.defaultdict(list)
    for row in rows:
        if row['kind'].startswith('prepared_'):
            groups[(row['owner'], row['id'])].append(row)
    results = []
    for (queue, sequence), events in groups.items():
        indexed = {row['kind']: row for row in events}
        screen_row = indexed.get('prepared_screen')
        screen = screen_row['value'] if screen_row else None
        if screen is not None and screen not in range(4):
            raise ValueError('unknown preparation screen')
        skipped = screen is not None and screen != 0
        order = ORDER[:5] + ['screen', 'end'] if skipped else (
            ORDER[:5] + ['screen'] + ORDER[5:] if screen_row else ORDER)
        capacity_row = indexed.get('prepared_storage_capacity')
        if capacity_row and not skipped:
            order = order.copy()
            order.insert(order.index('allocated') + 1, 'storage_capacity')
        expected = {'prepared_' + name for name in order}
        if len(indexed) != len(events) or set(indexed) != expected:
            raise ValueError(f'incomplete/duplicate shadow source {queue}/{sequence}')
        if [row['kind'] for row in events] != ['prepared_' + name for name in order]:
            raise ValueError('out-of-order shadow stages')
        if len({row['tid'] for row in events}) != 1:
            raise ValueError('shadow source crossed threads')
        for before, after in zip(events, events[1:]):
            if (before['wall_ns'] > after['wall_ns'] or
                    before['cpu_ns'] > after['cpu_ns']):
                raise ValueError('nonmonotonic shadow clocks')
        def value(name):
            return indexed['prepared_' + name]['value']
        def interval(first, last):
            a, b = indexed['prepared_' + first], indexed['prepared_' + last]
            if not a['cpu_ns'] or not b['cpu_ns']:
                raise ValueError('missing native thread CPU clock')
            return {'cpu_ns': b['cpu_ns'] - a['cpu_ns'],
                    'wall_ns': b['wall_ns'] - a['wall_ns']}
        status = None if skipped else value('status')
        capacity = capacity_row['value'] if capacity_row else None
        if capacity is not None and (capacity < 0 or
                value('allocated') not in (0, capacity) or
                (status == 0 and capacity == 0)):
            raise ValueError('allocation/capacity mismatch')
        if status is not None and not 0 <= status <= 8:
            raise ValueError('unknown prepared failure')
        if (not sequence or not value('source_generation') or
                not value('storage_generation') or not (value('storage_pages') & 0xffffffff)):
            raise ValueError('invalid source identity')
        results.append({
            'queue': queue, 'sequence': sequence,
            'source_index': value('source_index'),
            'source_generation': value('source_generation'),
            'storage_generation': value('storage_generation'),
            'first_page': value('storage_pages') >> 32,
            'page_count': value('storage_pages') & 0xffffffff,
            'commands': indexed['prepared_begin']['value'],
            'screen': screen, 'skipped': skipped,
            'allocation_bytes': 0 if skipped else value('allocated'), 'failure': status,
            'storage_capacity_bytes': capacity,
            'storage_reused': (bool(capacity and not value('allocated'))
                               if capacity is not None else None),
            'identities': None if skipped else value('identities'),
            'accesses': None if skipped else value('accesses'),
            'ranges': None if skipped else value('ranges'),
            'unresolved_visited': None if skipped else value('unresolved'),
            'allocation': None if skipped else interval(
                'screen' if screen_row else 'storage_pages', 'allocated'),
            'append': None if skipped else interval(
                'storage_capacity' if capacity_row else 'allocated', 'appended'),
            'seal': None if skipped else interval('appended', 'sealed'),
            'total': interval('begin', 'end'),
        })
    if not results:
        raise ValueError('no publication shadow hook evidence')
    return {'qualified_shadow_sources': len(results),
            'failures': dict(collections.Counter(r['failure'] for r in results if not r['skipped'])),
            'skips': dict(collections.Counter(r['screen'] for r in results if r['skipped'])),
            'scope': 'ordinary publication under queue mutex; extra synchronous '
                     'walk, clocks and allocation; no async consumption or FPS claim',
            'sources': results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    _, rows = supply.load(args.trace)
    result = analyze(rows)
    args.output.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
