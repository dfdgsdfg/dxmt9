#!/usr/bin/env python3
"""Pair diagnostic attempts; never infer mutex holders from time overlap alone."""
import argparse
import json
from pathlib import Path

from analyze_cpu_supply import load


def site_id(name):
    value = 14695981039346656037
    for byte in name.encode():
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value


SITES = {site_id(name): name for name in (
    'ensure_writer_slot', 'commit_current_chunk/pre_wait',
    'commit_current_chunk/post_wait', 'run_encode_loop/dequeue',
    'run_encode_loop/submit', 'run_encode_loop/inline_reclaim',
    'run_finish_loop/dequeue', 'run_finish_loop/retire',
    'map_buffer/pre_commit', 'map_buffer/finalize',
    'make_encode_context',
    'generate_texture_mip_sublevels',
    'upload_transient_buffer',
    'upload_transient_buffer_batch',
    'reserve_transient_buffer',
    'find_reordered_index_buffer',
    'remember_rejected_reordered_index_buffer',
    'get_or_create_reordered_index_buffer',
    'stop_threads',
    'set_skip_draw_resource_marking',
    'mark_chunk_resources',
    'mark_chunk_resources_and_capture_buffer_bindings',
    'abort_cpu_ready_arena_batch_detach',
    'abort_cpu_ready_arena_batch_finish',
    'abort_cpu_ready_arena_source_detach',
    'abort_cpu_ready_arena_source_finish',
    'submit_readback',
    'defer_present_boundary',
    'abort_present_ordinal_waits',
    'note_present_dequeued',
    'begin_replay_source_publication_capture',
    'finish_replay_source_publication_capture',
    'abandon_replay_source_publication_capture',
    'fail_stop_replay_source_execution',
    'arm_cpu_ready_next_source_intent',
    'cancel_cpu_ready_next_source_intent',
    'direct_commit/raw_resource_mark',
    'direct_commit/span_resource_mark',
    'direct_commit/retain_digest',
    'compat_batch/binding_snapshot',
    'compat_batch/reserve_mark_append',
    'compat_batch/resource_mark',
    'compat_batch/append',
)}


def held_segments(rows):
    pending, holds, incomplete = {}, [], 0
    seen = set()
    for row in sorted(rows, key=lambda r: r['wall_ns']):
        if row['kind'] not in ('queue_mutex_hold_begin', 'queue_mutex_hold_end'):
            continue
        key = row['owner'], row['tid'], row['id']
        endpoint = key, row['kind']
        if endpoint in seen or not row['owner'] or not row['id']:
            raise ValueError('invalid/duplicate mutex segment identity')
        seen.add(endpoint)
        if row['kind'] == 'queue_mutex_hold_begin':
            pending[key] = row
        else:
            begin = pending.pop(key, None)
            if begin is None:
                incomplete += 1
                continue
            holds.append(dict(owner=row['owner'], tid=row['tid'],
                              begin_ns=begin['wall_ns'], end_ns=row['wall_ns'],
                              site=SITES.get(row['value'], f"unknown:{row['value']}")))
    return holds, incomplete + len(pending)


def analyze(rows):
    holds, incomplete_holds = held_segments(rows)
    groups = {}
    prefixes = ('stateblock_wait_', 'mark_ticket_')
    for row in rows:
        if not row['kind'].startswith(prefixes):
            continue
        key = (row['owner'], row['tid'], row['id'])
        if not row['id']:
            raise ValueError('zero diagnostic attempt')
        events = groups.setdefault(key, {})
        if row['kind'] in events:
            raise ValueError('duplicate attempt endpoint')
        events[row['kind']] = row
    complete, incomplete = [], 0
    for key, events in groups.items():
        stateblock = any(k.startswith(prefixes[0]) for k in events)
        prefix = prefixes[0] if stateblock else prefixes[1] + 'wait_'
        begin, end = events.get(prefix + 'begin'), events.get(prefix + 'end')
        if begin is None or end is None:
            incomplete += 1
            continue
        wall = end['wall_ns'] - begin['wall_ns']
        cpu = end['cpu_ns'] - begin['cpu_ns']
        if wall < 0 or cpu < 0:
            raise ValueError('inverted wait clocks')
        item = dict(owner=key[0], tid=key[1], attempt=key[2],
                    kind='stateblock_space' if stateblock else 'mark_ticket',
                    begin_ns=begin['wall_ns'], end_ns=end['wall_ns'],
                    wall_ns=wall, cpu_ns=cpu)
        if stateblock:
            capacity = events.get('stateblock_wait_capacity')
            adopted = events.get('stateblock_wait_adopted')
            item.update(depth_begin=begin['value'], depth_end=end['value'],
                        capacity=capacity['value'] if capacity else None,
                        replay_seq=adopted['value'] if adopted and adopted['value'] else None)
            # No adoption can mean rejection OR a clipped endpoint.
            item['adoption_observed'] = adopted is not None
        else:
            hold = events.get('mark_ticket_hold_end')
            item['mark_ticket'] = begin['value']
            item['hold_body_ns'] = None
            if hold:
                if hold['wall_ns'] < end['wall_ns']:
                    raise ValueError('inverted hold')
                item['hold_body_ns'] = hold['wall_ns'] - end['wall_ns']
            intersections = []
            for segment in holds:
                if segment['owner'] != key[0] or segment['tid'] == key[1]:
                    continue
                a = max(begin['wall_ns'], segment['begin_ns'])
                b = min(end['wall_ns'], segment['end_ns'])
                if b > a:
                    intersections.append(dict(segment, overlap_ns=b-a,
                                              overlap_begin_ns=a, overlap_end_ns=b))
            item['observed_holders'] = intersections
            # Union, not sum: nested diagnostic segments may overlap.
            covered, frontier = 0, begin['wall_ns']
            for segment in sorted(intersections, key=lambda h: h['overlap_begin_ns']):
                a, b = segment['overlap_begin_ns'], segment['overlap_end_ns']
                covered += max(0, b-max(a, frontier))
                frontier = max(frontier, b)
            item['observed_held_overlap_ns'] = covered
            item['unattributed_ns'] = wall-covered
        complete.append(item)
    return dict(waits=complete, incomplete_attempts=incomplete,
                incomplete_holds=incomplete_holds, held_segments=holds,
                holder_attribution='same mutex, different TID, measured held-body overlap; partial coverage, not causal/exclusive cost',
                timing='observer-inclusive; CV wait includes mutex reacquisition')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    _, rows = load(args.trace)
    result = json.dumps(analyze(rows), indent=2) + '\n'
    if args.output:
        args.output.write_text(result)
    else:
        print(result, end='')


if __name__ == '__main__':
    main()
