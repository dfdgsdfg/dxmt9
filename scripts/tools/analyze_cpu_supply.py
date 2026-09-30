#!/usr/bin/env python3
"""Bounded CPU supply trace analysis; conditional replay model, never FPS proof."""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
import hashlib
import json
from pathlib import Path
import re
import statistics

EVENTS = ('present import_begin import_end raw_ready mutation_ready stateblock_ready '
          'replay_begin replay_end queue_empty queue_reserved queue_ready '
          'pop_attempt pop_wait pop_wake publish encode_begin encode_end submit '
          'replay_ledger writer_wait_begin writer_wait_end '
          'commit_wait_begin commit_wait_end publish_reason publish_commands '
          'scope_boundary query_issue completion_dequeue completion_observed retire '
          'encode_prefetch_begin encode_prefetch_end encode_backend_begin encode_backend_end '
          'framegraph_encode_begin framegraph_encode_end '
          'framegraph_pool_binding chunk_body_begin chunk_body_end '
          'chunk_command_begin chunk_command_end chunk_split_begin chunk_split_end '
          'encode_draw_begin encode_draw_end '
          'parallel_observe_begin parallel_observe_counts parallel_observe_draws '
          'parallel_observe_children parallel_observe_reject parallel_observe_end '
          'parallel_observe_status parallel_observe_imbalance '
          'parallel_observe_resources parallel_access_range parallel_access_counts '
          'parallel_access_touches parallel_access_reuse parallel_access_flags '
          'prepared_begin prepared_allocated prepared_appended prepared_sealed '
          'prepared_status prepared_identities prepared_accesses prepared_ranges '
          'prepared_unresolved prepared_end prepared_source_index '
          'prepared_source_generation prepared_storage_generation '
          'prepared_storage_pages prepared_screen prepared_storage_capacity '
          'prepared_cardinality_begin prepared_cardinality_status prepared_cardinality_commands '
          'prepared_cardinality_unresolved prepared_cardinality_accesses '
          'prepared_cardinality_identities prepared_cardinality_end '
          'framegraph_planning_phase '
          'stateblock_wait_begin stateblock_wait_end stateblock_wait_capacity '
          'stateblock_wait_adopted mark_ticket_wait_begin mark_ticket_wait_end '
          'mark_ticket_hold_end queue_mutex_hold_begin queue_mutex_hold_end '
          'queue_submit_phase queue_submit_validation_end queue_submit_summary_family '
          'session_release_post session_release_raw session_release_policy '
          'session_release_queue session_release_action_begin session_release_ack '
          'session_release_return').split()


def load(path):
    with Path(path).open() as stream:
        header = stream.readline().strip()
        if not header.startswith('# cpu-supply-v2 '):
            raise ValueError('unknown/unqualified CPU clock schema; v1 POSIX CPU clock rejected')
        meta = {k: int(v) for k, v in (s.split('=') for s in header.split()[2:])}
        if meta.get('cpu_clock') != 1 or meta['closed'] != 1 or meta['overflow'] or meta['incomplete']:
            raise ValueError(f'incomplete trace: {meta}')
        rows = [{k: int(v) for k, v in r.items()} for r in csv.DictReader(stream)]
    for r in rows:
        if not 0 <= r['event'] < len(EVENTS) or min(r.values()) < 0:
            raise ValueError('invalid event')
        r['kind'] = EVENTS[r['event']]
        if not r['wall_ns'] or not r['cpu_ns'] or not r['tid']:
            raise ValueError('clock/TID unavailable')
    rows.sort(key=lambda r: r['wall_ns'])
    return meta, rows


def pair(rows, start, stop, *, by_id=False, interior=None):
    active, spans = {}, []
    for r in rows:
        if r['kind'] not in (start, stop):
            continue
        key = (r['owner'], r['tid'], r['id'] if by_id else 0)
        if r['kind'] == start:
            if key in active:
                raise ValueError(f'nested/duplicate {start}')
            active[key] = r
        elif key in active:
            a = active.pop(key)
            wall, cpu = r['wall_ns']-a['wall_ns'], r['cpu_ns']-a['cpu_ns']
            if wall < 0 or cpu < 0 or cpu > wall + 100000:
                raise ValueError('invalid paired clocks')
            spans.append(dict(begin=a, end=r, wall_ns=wall, cpu_ns=cpu))
        elif interior and interior[0] <= r['wall_ns'] <= interior[1]:
            raise ValueError(f'interior {stop} has no matching start')
    if interior and any(a['wall_ns'] <= interior[1] for a in active.values()):
        raise ValueError(f'unclosed {start} crosses the interior window')
    # Leading/trailing clipped scopes are deliberately excluded.
    return spans


def serial_schedule(jobs, factor=1.0, keep_gaps=True):
    """Fixed arrivals; scale only measured service, preserve observed release gaps.

    Includes neither arrival feedback nor a change in blocked-time causes. Used
    for sensitivity, not as a realizable renderer or an unconditional bound.
    """
    previous_actual = previous_model = 0
    result = []
    for arrival, start, end in jobs:
        if not arrival <= start <= end or start < previous_actual:
            raise ValueError('non-FIFO or inverted job')
        gap = start - max(arrival, previous_actual) if keep_gaps else 0
        modeled_start = max(arrival, previous_model) + gap
        modeled_end = modeled_start + (end-start)*factor
        result.append((modeled_start, modeled_end))
        previous_actual, previous_model = end, modeled_end
    return result


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered)-1, int((len(ordered)-1)*fraction))]


def qualify_scene(log, first_present, last_present):
    # perf frame N is emitted after encoded Present N+1. Interval (a,b]
    # therefore corresponds to frame-delta rows [a,b).
    selected = []
    for line in log.splitlines():
        if '[dxmt9-perf-frame ' not in line:
            continue
        fields = dict(re.findall(r'(\w+)=([0-9.]+)', line))
        frame = int(fields['frame'])
        if first_present <= frame < last_present:
            if float(fields['wall_ms']) <= 0 or int(fields['draw_calls']) <= 100:
                raise ValueError('selected window includes menu/loading/non-scene frame')
            selected.append(frame)
    if selected != list(range(first_present, last_present)):
        raise ValueError('scene frame coverage missing/duplicated')
    return dict(first_frame=selected[0], last_frame=selected[-1], count=len(selected),
                predicate='every selected frame: positive wall_ms and draw_calls > 100')


def analyze(meta, rows, trim=10):
    presents = [r for r in rows if r['kind'] == 'present']
    expected = list(range(meta['first'], meta['last']+1))
    if [r['id'] for r in presents] != expected:
        raise ValueError('missing/duplicate Present endpoint')
    first, last = meta['first']+trim, meta['last']-trim
    if first >= last:
        raise ValueError('empty interior window')
    lo = next(r['wall_ns'] for r in presents if r['id'] == first)
    hi = next(r['wall_ns'] for r in presents if r['id'] == last)
    elapsed, frames = hi-lo, last-first
    interior = (lo, hi)
    replay = pair(rows, 'replay_begin', 'replay_end', interior=interior)
    imports = pair(rows, 'import_begin', 'import_end', interior=interior)
    encode = pair(rows, 'encode_begin', 'encode_end', by_id=True, interior=interior)
    waits = pair(rows, 'pop_wait', 'pop_wake', interior=interior)
    if not replay or not imports or not encode:
        raise ValueError('missing supported stage; session/batched encode is not covered')
    stage_tids = {name: sorted({s['begin']['tid'] for s in spans})
                  for name, spans in [('app', imports), ('replay', replay), ('encode', encode)]}
    if any(len(tids) != 1 for tids in stage_tids.values()) or len({v[0] for v in stage_tids.values()}) != 3:
        raise ValueError('requires one distinct app, replay and encode thread')
    envelopes = {}
    for name, tids in stage_tids.items():
        thread_rows = [r for r in rows if r['tid'] == tids[0]]
        if any(b['cpu_ns'] < a['cpu_ns'] for a, b in zip(thread_rows, thread_rows[1:])):
            raise ValueError('thread CPU counter rollback')
        samples = [r for r in rows if r['tid'] == tids[0] and lo <= r['wall_ns'] <= hi]
        if len(samples) < 2:
            raise ValueError('thread envelope lacks interior endpoints')
        a, b = samples[0], samples[-1]
        wall, cpu = b['wall_ns']-a['wall_ns'], b['cpu_ns']-a['cpu_ns']
        if wall <= 0 or cpu < 0 or cpu > wall + 100000:
            raise ValueError('invalid thread envelope clocks')
        envelopes[name] = dict(tid=tids[0], wall_ms=wall/1e6, cpu_ms=cpu/1e6,
                               cores=cpu/wall, window_coverage=wall/elapsed,
                               cpu_ms_per_present=cpu/frames/1e6)

    def measured(spans):
        complete = [s for s in spans if lo <= s['begin']['wall_ns'] <= s['end']['wall_ns'] <= hi]
        wall = sum(s['wall_ns'] for s in complete)
        cpu = sum(s['cpu_ns'] for s in complete)
        return dict(calls=len(complete), wall_ms_per_present=wall/frames/1e6,
                    cpu_ms_per_present=cpu/frames/1e6, wall_fraction=wall/elapsed,
                    wall_minus_cpu_ms_per_present=(wall-cpu)/frames/1e6)

    states = [r for r in rows if r['kind'] in
              ('queue_empty', 'queue_reserved', 'queue_ready')]
    if len({r['owner'] for r in states}) != 1:
        raise ValueError('requires one replay queue')
    occupancy, depth_ns, ready_ns = collections.Counter(), 0, 0
    state_segments = []
    for a, b in zip(states, states[1:]+[dict(wall_ns=hi)]):
        duration = max(0, min(b['wall_ns'], hi)-max(a['wall_ns'], lo))
        occupancy[a['kind']] += duration
        depth_ns += duration*a['id']
        ready_ns += duration*a['value']
        if duration:
            state_segments.append((max(a['wall_ns'], lo), min(b['wall_ns'], hi), a['kind']))
    if sum(occupancy.values()) != elapsed:
        raise ValueError('queue state coverage incomplete')
    wait_causes = collections.Counter()
    state_ends = [s[1] for s in state_segments]
    for s in waits:
        index = bisect.bisect_right(state_ends, s['begin']['wall_ns'])
        while index < len(state_segments):
            begin, end, kind = state_segments[index]
            if begin >= s['end']['wall_ns']:
                break
            ns = max(0, min(end, s['end']['wall_ns'])-max(begin, s['begin']['wall_ns']))
            wait_causes[kind] += ns
            index += 1

    # Ready-to-pop age is distinct from total waiting and FIFO placeholders.
    def unique(kind_set):
        result = {}
        for r in rows:
            if r['kind'] not in kind_set:
                continue
            key = (r['owner'], r['id'])
            if key in result:
                raise ValueError('duplicate identity or reused owner/sequence')
            result[key] = r
        return result
    ready = unique({'raw_ready', 'mutation_ready', 'stateblock_ready'})
    jobs, replay_spans = [], []
    for s in replay:
        a = s['begin']
        r = ready.get((a['owner'], a['id']))
        if r is None and lo <= a['wall_ns'] <= hi:
            raise ValueError('interior replay has no ready identity')
        if r:
            jobs.append((r['wall_ns'], a['wall_ns'], s['end']['wall_ns']))
            replay_spans.append(s)
    ages = [(b-a)/1e6 for a, b, _ in jobs if lo <= b <= hi]
    pubs = unique({'publish'})
    # Attach publication only to the actual same-thread enclosing replay job.
    # Earlier raw contributors stay ordered through the serial replay prefix.
    replay_starts = [s['begin']['wall_ns'] for s in replay_spans]
    modeled_replay = {f: serial_schedule(jobs, f) for f in (1.0, 0.5, 0.0)}
    modeled_pubs = {f: {} for f in modeled_replay}
    linked, unlinked = 0, 0
    for key, r in pubs.items():
        index = bisect.bisect_right(replay_starts, r['wall_ns'])-1
        linked_job = (index >= 0 and r['tid'] == replay_spans[index]['begin']['tid']
                      and r['wall_ns'] <= replay_spans[index]['end']['wall_ns'])
        if linked_job:
            # Nonzero rawOrdinal must agree with the enclosing FIFO item.
            if r['value'] and r['value'] != replay_spans[index]['begin']['id']:
                raise ValueError('publication raw identity disagrees with replay scope')
            linked += 1
        else:
            unlinked += 1
        for f in modeled_pubs:
            modeled_pubs[f][key] = (modeled_replay[f][index][0] +
                (r['wall_ns']-replay_starts[index])*f) if linked_job else r['wall_ns']

    # Preserve measured encode service and dispatch gaps. Submit/callback tail
    # is in the gap; it is not declared removable. GPU feedback is fixed, not
    # simulated. Present offsets within an encode scope are preserved.
    predictions = {f: {} for f in modeled_pubs}
    previous_actual = 0
    previous_modeled = {f: 0 for f in modeled_pubs}
    pub_ages = []
    encode_waits = collections.Counter()
    for s in encode:
        a, b = s['begin'], s['end']
        key = (a['owner'], a['id'])
        if key not in pubs:
            if lo <= a['wall_ns'] <= hi:
                raise ValueError('interior encode has no publication')
            continue
        actual_pub = pubs[key]['wall_ns']
        if actual_pub > a['wall_ns'] or a['wall_ns'] < previous_actual:
            raise ValueError('invalid encode publication/order')
        gap = a['wall_ns'] - max(actual_pub, previous_actual)
        if previous_actual and lo <= previous_actual <= a['wall_ns'] <= hi:
            encode_waits['unpublished_input_ns'] += max(0, actual_pub-previous_actual)
            encode_waits['ready_dispatch_or_tail_ns'] += gap
        if lo <= a['wall_ns'] <= hi:
            pub_ages.append((a['wall_ns']-actual_pub)/1e6)
        members = [p for p in presents if p['tid'] == a['tid'] and
                   a['wall_ns'] <= p['wall_ns'] <= b['wall_ns']]
        for f in predictions:
            begin = max(modeled_pubs[f][key], previous_modeled[f]) + gap
            previous_modeled[f] = begin+s['wall_ns']
            for p in members:
                predictions[f][p['id']] = begin+p['wall_ns']-a['wall_ns']
        previous_actual = b['wall_ns']
    sensitivities = {}
    for f, prediction in predictions.items():
        if any(identity not in prediction for identity in range(first, last+1)):
            raise ValueError('counterfactual interior Present coverage incomplete')
        modeled_elapsed = prediction[last]-prediction[first]
        advance = [p['wall_ns']-prediction[p['id']] for p in presents
                   if first <= p['id'] <= last and p['id'] in prediction]
        if f == 1 and max(abs(x) for x in advance) > 1:
            raise ValueError('factor-one model does not reproduce trace')
        sensitivities[str(f)] = dict(window_ms=modeled_elapsed/1e6,
            modeled_throughput_change_percent=(elapsed/modeled_elapsed-1)*100,
            present_advance_median_ms=statistics.median(advance)/1e6,
            present_advance_p95_ms=percentile(advance, .95)/1e6)
    return dict(schema_version=1, qualification='diagnostic; conditional fixed-arrival model, not actual FPS benefit or unconditional ceiling',
        frames=frames, first_present=first, last_present=last, wall_ms=elapsed/1e6,
        observed_window_fps=frames*1e9/elapsed, threads=envelopes,
        scopes={name: measured(s) for name, s in [('import', imports), ('replay', replay),
                                                ('encode', encode), ('input_wait', waits)]},
        queue_fraction={k: v/elapsed for k, v in occupancy.items()},
        input_wait_state_ms_per_present={k: v/frames/1e6 for k, v in wait_causes.items()},
        encode_gap_ms_per_present={k: v/frames/1e6 for k, v in encode_waits.items()},
        average_depth=depth_ns/elapsed, average_ready_prefix=ready_ns/elapsed,
        ready_to_pop_ms=dict(median=percentile(ages, .5), p95=percentile(ages, .95), maximum=max(ages)),
        publish_to_encode_ms=dict(median=percentile(pub_ages, .5), p95=percentile(pub_ages, .95)),
        publication_binding=dict(linked=linked, unlinked_fixed=unlinked),
        fixed_arrival_sensitivity=sensitivities,
        limitations=['App CPU includes game/Wine/PE/synchronous Unix; not PE-exclusive.',
          'Thread envelopes have slightly different endpoints; scopes are nested, not additive.',
          'Replay scope includes mutation/StateBlock, release and queue reacquisition.',
          'Half/zero scale elapsed service and publication offsets, not just removable CPU.',
          'Producer arrivals, dispatch gaps, encode work and GPU feedback are held fixed.',
          'No counterfactual producer speedup from reduced lock/CPU contention is modeled.',
          'Saturated replay+encode and whole-pipeline end-to-end bound remain separate gates.'])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('trace', type=Path)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--frame-log', type=Path, required=True)
    p.add_argument('--trim', type=int, default=10)
    a = p.parse_args()
    meta, rows = load(a.trace)
    result = analyze(meta, rows, a.trim)
    result['scene_window'] = qualify_scene(a.frame_log.read_text(errors='replace'),
                                         result['first_present'], result['last_present'])
    result['frame_log'] = dict(path=str(a.frame_log),
        sha256=hashlib.sha256(a.frame_log.read_bytes()).hexdigest())
    result['input'] = dict(path=str(a.trace), sha256=hashlib.sha256(a.trace.read_bytes()).hexdigest())
    result['analyzer_sha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    a.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
