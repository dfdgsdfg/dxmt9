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
DRAW_EVENTS = ('encode_draw_begin', 'encode_draw_end')
DRAW_RUN_PHASES = ('uniform_setup', 'flush_blit_hazard_setup', 'pass_entry',
                   'up_setup', 'binding_plan_setup')


def draw_run_phases(begin, end, candidates):
    """Optional sequential milestones; loop remainder includes per-draw setup."""
    kinds = COMMAND_EVENTS + DRAW_EVENTS + ('draw_run_phase',)
    rows = [r for r in candidates if r['kind'] in kinds and
            begin['wall_ns'] <= r['wall_ns'] <= end['wall_ns']]
    rows.sort(key=lambda r: r['wall_ns'])
    if not any(r['kind'] == 'draw_run_phase' for r in rows):
        return dict(qualified=False, reason='no DrawRun milestones; legacy or skipped commands')
    totals = {k: dict(wall_ms=0.0, cpu_ms=0.0) for k in (*DRAW_RUN_PHASES, 'loop_residual')}
    parent = previous_phase = draw = None
    phase = 0
    draw_wall = draw_cpu = 0
    commands = unclassified = 0
    saw_draw = False
    previous = begin
    for row in rows + [end]:
        if (row['tid'] != begin['tid'] or row['wall_ns'] < previous['wall_ns'] or
                row['cpu_ns'] < previous['cpu_ns']):
            raise ValueError('invalid DrawRun milestone thread/clocks')
        previous = row
        kind = row['kind']
        if kind == 'chunk_command_begin':
            if parent is not None:
                raise ValueError('nested DrawRun milestone parent')
            parent = previous_phase = row
            phase = 0
            draw_wall = draw_cpu = 0
            saw_draw = False
        elif kind == 'draw_run_phase':
            if parent is None or parent['value'] != 1 or draw is not None or saw_draw or row['value'] != phase or phase >= 5:
                raise ValueError('invalid DrawRun milestone order/parent')
            wall, cpu = row['wall_ns']-previous_phase['wall_ns'], row['cpu_ns']-previous_phase['cpu_ns']
            if cpu > wall + 100000:
                raise ValueError('invalid DrawRun milestone CPU')
            totals[DRAW_RUN_PHASES[phase]]['wall_ms'] += wall/1e6
            totals[DRAW_RUN_PHASES[phase]]['cpu_ms'] += cpu/1e6
            previous_phase, phase = row, phase + 1
        elif kind == 'encode_draw_begin':
            if parent is None or parent['value'] != 1 or draw is not None or phase not in (0, 5):
                raise ValueError('draw before completed DrawRun milestones')
            draw = row
            saw_draw = True
        elif kind == 'encode_draw_end':
            if draw is None or row['value'] != draw['value']:
                raise ValueError('unpaired DrawRun nested draw')
            draw_wall += row['wall_ns']-draw['wall_ns']
            draw_cpu += row['cpu_ns']-draw['cpu_ns']
            draw = None
        elif kind == 'chunk_command_end':
            if parent is None or draw is not None or row['value'] != parent['value'] or phase not in (0, 5):
                raise ValueError('incomplete DrawRun milestones')
            if parent['value'] == 1:
                if phase == 0:
                    unclassified += 1
                else:
                    wall = row['wall_ns']-previous_phase['wall_ns']-draw_wall
                    cpu = row['cpu_ns']-previous_phase['cpu_ns']-draw_cpu
                    if min(wall, cpu) < 0:
                        raise ValueError('negative DrawRun loop remainder')
                    totals['loop_residual']['wall_ms'] += wall/1e6
                    totals['loop_residual']['cpu_ms'] += cpu/1e6
                    commands += 1
            parent = None
    if parent is not None or draw is not None:
        raise ValueError('unclosed DrawRun milestone parent')
    return dict(qualified=True, commands=commands, unclassified_commands=unclassified, phases=totals)


PARALLEL_EVENTS = tuple(k for k in supply.EVENTS if k.startswith('parallel_observe_'))
ACCESS_EVENTS = ('parallel_access_range', 'parallel_access_counts',
                 'parallel_access_touches', 'parallel_access_reuse', 'parallel_access_flags')
PARALLEL_REASONS = ('none plan_missing plan_not_validated replay_invalid '
    'unsealed_start unsealed_end coordinator_command non_child_draw_run '
    'no_two_child_work planner_invariant child_capacity pass_capacity '
    'attachment_mismatch resource_set_incomplete resource_identity_proof '
    'resource_hazard render_route pass_action_epoch query_state update_texture_state '
    'capture_state initializer_state ordered_control_state sidecar_state '
    'first_draw_snapshot raw_read_set_incomplete raw_write_set_incomplete '
    'pass_read_union_overflow').split()

def parallel_accesses(begin, end, rows, candidates, overflow_count, eligible=None):
    local = sorted((r for r in rows if r['kind'] in ACCESS_EVENTS),key=lambda r:r['wall_ns'])
    if not local:
        return None
    if len(local) != candidates*len(ACCESS_EVENTS):
        raise ValueError('incomplete parallel access transcript')
    previous, previous_end = begin, 0
    passes = []
    for index in range(0,len(local),len(ACCESS_EVENTS)):
        group = local[index:index+len(ACCESS_EVENTS)]
        if tuple(r['kind'] for r in group) != ACCESS_EVENTS:
            raise ValueError('parallel access event order/duplicate')
        for r in group:
            if (r['tid'] != begin['tid'] or r['wall_ns'] < previous['wall_ns'] or
                    r['cpu_ns'] < previous['cpu_ns'] or r['wall_ns'] > end['wall_ns'] or
                    r['cpu_ns'] > end['cpu_ns']):
                raise ValueError('parallel access clocks/thread')
            previous = r
        interval, counts, touches, reuse, flags = (r['value'] for r in group)
        first, last = interval >> 32, interval & 0xffffffff
        unique, commands = counts >> 32, counts & 0xffffffff
        maximum_uses, adjacent = reuse >> 32, reuse & 0xffffffff
        exact, exhausted, incomplete = bool(flags&1), bool(flags&2), bool(flags&4)
        overflow, rejected, sealed = bool(flags&8), bool(flags&16), bool(flags&32)
        if (flags & ~63 or exact != (not exhausted and not incomplete) or
                first < previous_end or last <= first or not commands or commands > last-first or
                unique > 4096 or unique > touches or maximum_uses > commands or
                maximum_uses > touches or adjacent > touches-unique or
                (touches and not maximum_uses) or
                (exhausted and unique != 4096) or (overflow and not rejected) or
                (not rejected and not sealed) or
                (exact and overflow and unique <= 256)):
            raise ValueError('invalid parallel access counts/range')
        previous_end = last
        passes.append(dict(begin=first,end=last,commands=commands,distinct=unique,
            touches=touches,maximum_uses=maximum_uses,adjacent_reuse=adjacent,
            exact=exact,diagnostic_exhausted=exhausted,incomplete=incomplete,
            production_overflow=overflow,rejected=rejected,sealed=sealed))
    if sum(p['production_overflow'] for p in passes) != overflow_count:
        raise ValueError('parallel access overflow count mismatch')
    if eligible is not None and sum(not p['rejected'] for p in passes) != eligible:
        raise ValueError('parallel access eligibility count mismatch')
    return passes


def parallel_observation(begin, end, rows):
    local = sorted((r for r in rows if r['kind'] in PARALLEL_EVENTS),
                   key=lambda r:r['wall_ns'])
    if not local:
        return dict(qualified=False, reason='parallel observer disabled or outside window')
    counts = collections.Counter(r['kind'] for r in local)
    required = set(PARALLEL_EVENTS)-{'parallel_observe_reject', 'parallel_observe_resources'}
    if any(counts[k] != 1 for k in required):
        raise ValueError('incomplete/duplicate parallel observation')
    if (local[0]['kind'] != 'parallel_observe_begin' or
            local[-1]['kind'] != 'parallel_observe_end'):
        raise ValueError('parallel observation endpoint order')
    previous = begin
    for row in local+[end]:
        if (row['tid'] != begin['tid'] or row['wall_ns'] < previous['wall_ns'] or
                row['cpu_ns'] < previous['cpu_ns']):
            raise ValueError('parallel observation clocks/thread')
        previous = row
    values = {r['kind'].removeprefix('parallel_observe_'):r['value'] for r in local}
    status, fallback = values['status'] >> 32, values['status'] & 0xffffffff
    if status not in (0,1) or fallback >= len(PARALLEL_REASONS) or values['begin'] not in (0,1):
        raise ValueError('invalid parallel observation classification')
    rejects = {}
    for row in local:
        if row['kind'] != 'parallel_observe_reject': continue
        reason, count = row['value'] >> 32, row['value'] & 0xffffffff
        if not 0 < reason < len(PARALLEL_REASONS) or not count or reason in rejects:
            raise ValueError('invalid/duplicate parallel rejection')
        rejects[reason] = count
    candidates, sealed = values['counts'] >> 32, values['counts'] & 0xffffffff
    # Detail buckets overlap the coarse bucket and are pass-deduplicated.
    # Old transcripts with only the coarse bucket remain readable.
    coarse = rejects.get(PARALLEL_REASONS.index('resource_set_incomplete'), 0)
    for detail in ('raw_read_set_incomplete', 'raw_write_set_incomplete',
                   'pass_read_union_overflow'):
        if rejects.get(PARALLEL_REASONS.index(detail), 0) > coarse:
            raise ValueError('parallel resource detail exceeds coarse rejection')
    eligible = values['end']
    resource_sizes = None
    if 'resources' in values:
        maximum, total = values['resources'] >> 32, values['resources'] & 0xffffffff
        if (counts['parallel_observe_resources'] != 1 or maximum > total or
                total > maximum*eligible or (not eligible and maximum)):
            raise ValueError('invalid parallel resource cardinalities')
        resource_sizes = dict(max_pass_reads=maximum, total_pass_reads=total)
    if (eligible > sealed or sealed > candidates or values['children'] < 2*eligible or
            (eligible and values['draws'] < values['children']) or
            values['imbalance'] > values['draws'] or
            (not status and (candidates or sealed or eligible or rejects)) or
            (not eligible and (values['draws'] or values['children'] or values['imbalance'])) or
            (values['begin'] and eligible)):
        raise ValueError('invalid parallel candidate counts')
    wall = local[-1]['wall_ns']-local[0]['wall_ns']
    cpu = local[-1]['cpu_ns']-local[0]['cpu_ns']
    if cpu > wall+100000: raise ValueError('invalid parallel observer CPU clock')
    return dict(qualified=True, query_scoped=bool(values['begin']), considered=bool(status),
        fallback=PARALLEL_REASONS[fallback], candidates=candidates, sealed=sealed,
        structural_eligible=eligible, structural_draws=values['draws'],
        children=values['children'], max_child_draw_imbalance=values['imbalance'],
        accepted_pass_resources=resource_sizes,
        pass_accesses=parallel_accesses(local[0],local[-1],rows,candidates,
            rejects.get(PARALLEL_REASONS.index('pass_read_union_overflow'),0),eligible),
        rejections={PARALLEL_REASONS[k]:v for k,v in rejects.items()},
        observer_cpu_ms=cpu/1e6, observer_wall_ms=wall/1e6,
        qualification='Structural only; excludes executor economics/binding gates; source CPU is not eligible-draw CPU')


def draw_phases(begin, end, candidates, draw_run):
    if not (begin.get('value',0) & 2):
        return dict(qualified=False, reason='draw split disabled')
    rows = sorted((r for r in candidates if r['kind'] in COMMAND_EVENTS+DRAW_EVENTS and
                   begin['wall_ns'] <= r['wall_ns'] <= end['wall_ns']),
                  key=lambda r:r['wall_ns'])
    parent, active, previous = None, None, begin
    total = dict(wall_ms=0.0, cpu_ms=0.0, calls=0)
    for r in rows+[end]:
        if (r['tid'] != begin['tid'] or r['cpu_ns'] < previous['cpu_ns'] or
                r['wall_ns'] < previous['wall_ns']):
            raise ValueError('invalid draw endpoint thread/order')
        previous = r
        kind = r['kind']
        if kind=='chunk_command_begin':
            parent = r
        elif kind=='chunk_command_end':
            if active is not None:
                raise ValueError('draw crosses parent boundary')
            parent = None
        elif kind=='encode_draw_begin':
            if active is not None or parent is None or parent['value'] != 1:
                raise ValueError('draw outside DrawRun or nested draw')
            active = r
        elif kind=='encode_draw_end':
            if active is None or r['value'] != active['value']:
                raise ValueError('unpaired draw endpoint')
            wall, cpu = r['wall_ns']-active['wall_ns'], r['cpu_ns']-active['cpu_ns']
            if cpu > wall+100000:
                raise ValueError('invalid draw CPU clock')
            total['wall_ms'] += wall/1e6
            total['cpu_ms'] += cpu/1e6
            total['calls'] += 1
            active = None
    if active is not None:
        raise ValueError('unclosed draw endpoint')
    residual = {k:draw_run[k]-total[k] for k in ('wall_ms','cpu_ms')}
    if any(v < -1e-9 for v in residual.values()):
        raise ValueError('negative DrawRun residual')
    return dict(qualified=True, encode_draw=total, preparation_pass_residual=residual)


def command_phases(begin, end, candidates):
    if not (begin.get('value',0) & 1):
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
    return dict(qualified=True, **totals, dispatch_residual=residual,
                draw_run_phases=draw_run_phases(begin, end, candidates),
                draw_phases=draw_phases(begin,end,candidates,totals['draw_run']))


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
        parallel_observation=parallel_observation(begin, end, local),
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


def parallel_summary(sources):
    qualified = []
    for source in sources:
        observation = source.get('framegraph_phases',{}).get('chunk_phases',{}).get('parallel_observation',{})
        if observation.get('qualified'):
            qualified.append((source,observation))
    groups = {}
    for name, predicate in (
            ('query_scoped',lambda o:o['query_scoped']),
            ('structural_candidate_sources',lambda o:o['structural_eligible'] > 0),
            ('unscoped_no_candidate',lambda o:not o['query_scoped'] and not o['structural_eligible'])):
        selected = [(s,o) for s,o in qualified if predicate(o)]
        groups[name] = dict(sources=len(selected),
            inclusive_encode_cpu_ms=sum(s['encode_cpu_ms'] for s,o in selected),
            observer_cpu_ms=sum(o['observer_cpu_ms'] for s,o in selected),
            structural_passes=sum(o['structural_eligible'] for s,o in selected),
            structural_draws=sum(o['structural_draws'] for s,o in selected))
    rejections = collections.Counter()
    for _,o in qualified: rejections.update(o['rejections'])
    return dict(qualified_sources=len(qualified), unqualified_sources=len(sources)-len(qualified),
        groups=groups, rejections=dict(rejections),
        qualification='Inclusive source CPU includes observer and noncandidate work; candidate-source CPU is an upper bound, not predicted savings')


def analyze(meta, rows, wait_meta, wait_rows, query=None, source_only=False):
    if source_only and query is not None:
        raise ValueError('source-only analysis cannot claim a query target')
    joined = joiner.join(wait_meta, wait_rows, meta, rows)
    pending = [r for r in joined['receipts'] if r['target'] and
               (query is None or r['query'] == query)]
    if not pending and not source_only:
        raise ValueError('no matched pending receipt')
    target = max(pending, key=lambda r: r['receipt_ns']) if pending else None
    events = collections.defaultdict(dict)
    backend_events = collections.defaultdict(list)
    chunk_events = collections.defaultdict(list)
    for r in rows:
        if r['kind'] in CHUNK_BODY + COMMAND_EVENTS + DRAW_EVENTS + ('draw_run_phase',) + PARALLEL_EVENTS + ACCESS_EVENTS:
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
    if source_only:
        if not sources:
            raise ValueError('no complete ordinary sources')
        return dict(target=None, target_sources=[max(sources,key=lambda s:s['encode_cpu_ms'])],
            complete_sources=sources, incomplete_source_count=len(events)-len(sources),
            parallel_summary=parallel_summary(sources),
            selection='largest encode CPU among complete ordinary sources; not a blocking-receipt selection',
            qualification='Source-local timing only; no query wait ownership or scene equivalence claim')
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
        parallel_summary=parallel_summary(sources),
        complete_sources=sources, incomplete_source_count=len(events)-len(sources),
        qualification='Ordinary source stages only; encode CPU uses same-thread endpoints and includes prefetch/backend work, not only draw encoding; retirement in wait interval is not OS wake identity; elapsed stages across sources overlap')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('supply', type=Path)
    p.add_argument('waits', type=Path)
    p.add_argument('--output', required=True, type=Path)
    selection = p.add_mutually_exclusive_group()
    selection.add_argument('--query', type=int, help='Select a recorded query identity; absent means longest receipt')
    selection.add_argument('--source-only', action='store_true',
                           help='Explicitly select largest complete source without a blocking-receipt claim')
    a = p.parse_args()
    try:
        result = analyze(*supply.load(a.supply), *waits.load(a.waits),
                         query=a.query, source_only=a.source_only)
        with a.output.open('x') as stream:
            stream.write(json.dumps(result, indent=2)+'\n')
        print(json.dumps({k:v for k,v in result.items() if k not in ('complete_sources','target_sources','target')}, indent=2))
    except (ValueError, KeyError, OSError, StopIteration) as error:
        p.exit(1, f'unqualified progress: {error}\n')
