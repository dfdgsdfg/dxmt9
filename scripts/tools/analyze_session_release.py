#!/usr/bin/env python3
"""Ordered-release transport timing, not GPU completion or a reuse proof."""
import argparse
import collections
import json
from pathlib import Path

import analyze_cpu_supply as supply

PREFIX = 'session_release_'
STAGES = ('post', 'raw', 'policy', 'queue', 'action_begin', 'ack', 'return')


def analyze(meta, rows, *, require_close=False):
    close_schema = require_close or any(r['kind'] == PREFIX + 'close' for r in rows)
    groups = collections.defaultdict(dict)
    for row in rows:
        if not row['kind'].startswith(PREFIX):
            continue
        stage = row['kind'][len(PREFIX):]
        if stage not in (*STAGES, 'close', 'reuse') or not row['owner'] or not row['id']:
            raise ValueError('invalid release identity/stage')
        group = groups[row['owner'], row['id']]
        if stage in group:
            raise ValueError('duplicate release stage or reused owner identity')
        group[stage] = row
    complete, clipped, terminal, stopped, consumer_only = [], 0, 0, 0, 0
    for (owner, ordinal), group in groups.items():
        close = group.pop('close', None)
        reuse = group.pop('reuse', None)
        if reuse is not None:
            if close is not None or 'action_begin' in group or 'ack' in group:
                raise ValueError('reuse cannot also execute an Encode close')
            if reuse['value'] != 0:
                raise ValueError('invalid reuse value')
            # One producer-side linearization point replaces both endpoints.
            # These are timing aliases, not fabricated Encode events.
            group['action_begin'] = reuse
            group['ack'] = reuse
        action = group.get('action_begin')
        if action and action['value'] & 4 and 'post' not in group:
            terminal += 1
            continue
        # Encode also posts internal producer-wait/cap releases, without a
        # synchronous posting caller. Do not invent a posting endpoint.
        if set(group) == {'action_begin', 'ack'}:
            consumer_only += 1
            continue
        if set(group) != set(STAGES):
            if group.get('return', {}).get('value') == 0:
                stopped += 1
                continue
            if all(r['frame'] in (meta['first'], meta['last']) for r in group.values()):
                clipped += 1
                continue
            raise ValueError('interior release missing stages')
        ordered = [group[s] for s in STAGES]
        if any(a['wall_ns'] > b['wall_ns'] for a, b in zip(ordered, ordered[1:])):
            raise ValueError('release stage order')
        post, raw, policy, queue, begin, ack, returned = ordered
        if not queue['value'] or action['value'] > 3 or returned['value'] not in (0, 1):
            raise ValueError('invalid release values')
        if not returned['value']:
            stopped += 1
            continue
        reason, operation, writer = (policy['value'] & 255,
                                    (policy['value'] >> 8) & 255,
                                    policy['value'] >> 16)
        if reason > 6 or operation > 2 or writer > 1 or ack['value'] not in (0, 1):
            raise ValueError('invalid release policy/completion')
        if operation != 0 and ack['value'] != 1:
            raise ValueError('submission action lacks submission acknowledgement')
        close_outcome = None
        if reuse is not None:
            if (reason, operation, writer) != (2, 0, 1) or reuse['tid'] != post['tid']:
                raise ValueError('invalid reuse policy/thread')
            close_outcome = 'reused'
        if operation == 0 and close_schema and close is None and reuse is None:
            raise ValueError('missing close outcome')
        if close is not None:
            if (operation != 0 or close['tid'] != begin['tid'] or
                    not begin['wall_ns'] <= close['wall_ns'] <= ack['wall_ns'] or
                    not begin['cpu_ns'] <= close['cpu_ns'] <= ack['cpu_ns'] or
                    close['value'] not in (0, 1, 3) or
                    (close['value'] == 3) != (begin['value'] != 3)):
                raise ValueError('invalid close outcome')
            close_outcome = {0: 'no_active_pass', 1: 'closed', 3: 'no_session'}[close['value']]
        if any(r['tid'] != post['tid'] for r in (raw, policy, queue, returned)):
            raise ValueError('posting thread changed')

        def cpu(a, b):
            elapsed = b['cpu_ns'] - a['cpu_ns']
            if a['tid'] != b['tid'] or elapsed < 0 or elapsed > b['wall_ns'] - a['wall_ns'] + 100000:
                raise ValueError('invalid same-thread CPU endpoints')
            return elapsed

        complete.append(dict(owner=owner, ordinal=ordinal, queue=queue['value'],
            raw=raw['value'], sequence=post['value'], reason=reason,
            action=operation, writer=writer, close_outcome=close_outcome,
            reused=reuse is not None,
            pending_record=None if reuse is not None else bool(begin['value'] & 1),
            pending_session=None if reuse is not None else bool(begin['value'] & 2), post_ns=post['wall_ns'],
            return_ns=returned['wall_ns'],
            post_to_action_ns=begin['wall_ns'] - post['wall_ns'],
            action_to_ack_ns=ack['wall_ns'] - begin['wall_ns'],
            ack_to_return_ns=returned['wall_ns'] - ack['wall_ns'],
            roundtrip_ns=returned['wall_ns'] - post['wall_ns'],
            posting_cpu_ns=cpu(post, returned), action_cpu_ns=cpu(begin, ack)))
    complete.sort(key=lambda r: r['post_ns'])
    previous = {}
    for record in complete:
        key = record['queue']
        prior = previous.get(key)
        record['consecutive_same_prefix'] = bool(prior and
            record['owner'] == prior['owner'] and record['ordinal'] == prior['ordinal'] + 1 and
            record['sequence'] == prior['sequence'] and record['raw'] >= prior['raw'] and
            record['post_ns'] >= prior['return_ns'] and
            record['action'] == prior['action'] == 0 and
            record['reason'] == prior['reason'] == 2 and
            record['writer'] == prior['writer'] == 1)
        previous[key] = record
    def summary(population):
        return dict(count=len(population), **{
            key.removesuffix('_ns') + '_ms': sum(r[key] for r in population) / 1e6
            for key in ('roundtrip_ns', 'post_to_action_ns', 'action_to_ack_ns',
                        'ack_to_return_ns', 'posting_cpu_ns', 'action_cpu_ns')})
    return dict(complete=summary(complete),
        reused=summary([r for r in complete if r['reused']]),
        consecutive_same_prefix=summary([r for r in complete if r['consecutive_same_prefix']]),
        clipped=clipped, terminal=terminal, stopped=stopped,
        consumer_only=consumer_only, releases=complete,
        close_outcomes=dict(collections.Counter(r['close_outcome'] or 'unknown'
            for r in complete if r['action'] == 0)),
        qualification='Elapsed scopes include diagnostic clocks, scheduling and mutex reacquisition. '
        'Same-prefix classification is not an epoch/pass-state witness or permission to skip release. '
        'Return precedes any separate SubmitAndWait GPU completion wait; no queue acquisition, '
        'Reused rows use one producer-side ack point, not an Encode action; its action-to-ack scope is zero. '
        'writer commit or full-ring wait coverage. Terminal, consumer-only and clipped groups excluded.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--require-close-outcomes', action='store_true')
    args = parser.parse_args()
    try:
        result = analyze(*supply.load(args.trace), require_close=args.require_close_outcomes)
        if not result['complete']['count']:
            raise ValueError('no complete release evidence')
        with args.output.open('x') as stream:
            stream.write(json.dumps(result, indent=2) + '\n')
        print(json.dumps({k: v for k, v in result.items() if k != 'releases'}, indent=2))
    except (ValueError, KeyError, OSError) as error:
        parser.exit(1, f'unqualified release trace: {error}\n')
