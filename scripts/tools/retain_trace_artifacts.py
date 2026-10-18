#!/usr/bin/env python3
"""Evidence-aware offline retention. Inventory/plan never remove source bytes.

Eviction intentionally supports explicit bulk files only. Directory bundles can
be archive-copied; metadata and path-bound citations remain materialized.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts.tools.retention_archive import (archive_verified, restore_verified,
                                            snapshot_tree, _safe_path, _rename_noreplace)

GIB = 1024 ** 3
SCHEMA = 'dxmt9.retention.v1'
DEFAULT_FLOOR = 20 * GIB
ARTIFACT_ROOTS = ('experiments/output', 'traces')
TEXT_SUFFIXES = {'.md', '.json', '.toml', '.yaml', '.yml', '.sql', '.txt', '.xml',
                 '.jsonl', '.csv', '.log', '.py', '.sh', '.cpp', '.c', '.h',
                 '.hpp', '.mm', '.ini'}
OPAQUE_SUFFIXES = {'.sqlite', '.sqlite3', '.db', '.duckdb', '.parquet'}
BULK_SUFFIXES = {'.csv', '.jsonl', '.log', '.sqlite', '.sqlite3', '.db',
                 '.duckdb', '.parquet', '.bin', '.pftrace', '.ktrace', '.spool'}
CONTROL_WORDS = re.compile(r'(manifest|receipt|proof|request|summary|result|completion|retention)', re.I)
MAX_REFERENCE_BYTES = 16 * 1024 * 1024
MAX_REFERENCE_FILES = 200_000


def now():
    return datetime.now(timezone.utc).isoformat()


def sha256(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def read_json(path):
    _safe_path(path)
    if path.stat().st_size > MAX_REFERENCE_BYTES:
        raise ValueError(f'control file too large: {path}')
    return json.loads(path.read_text())


def sync_dir(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def write_json(path, value):
    _safe_path(path)
    fd, name = tempfile.mkstemp(prefix='.record-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
        sync_dir(path.parent)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def store_path(repo):
    return _safe_path(repo / 'experiments/output/_retention')


def source_path(repo, value):
    raw = Path(value)
    if '..' in raw.parts:
        raise ValueError('parent traversal is not allowed')
    path = _safe_path(raw if raw.is_absolute() else repo / raw)
    relative = path.relative_to(repo).as_posix()
    if not any(relative.startswith(root + '/') for root in ARTIFACT_ROOTS):
        raise ValueError('candidate must be below experiments/output/ or traces/')
    if path == store_path(repo) or store_path(repo) in path.parents:
        raise ValueError('retention records and archives are never candidates')
    return path


@contextmanager
def lease(repo):
    store = store_path(repo)
    store.mkdir(parents=True, exist_ok=True)
    lock = _safe_path(store / 'mutation.lock')
    with lock.open('a+') as stream:
        try:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError('another retention mutation holds the lease') from error
        try:
            yield store
        finally:
            fcntl.flock(stream, fcntl.LOCK_UN)


def open_handles(path):
    executable = shutil.which('lsof') or '/usr/sbin/lsof'
    args = [executable, '-nP', '-t']
    args += ['+D', str(path)] if path.is_dir() else ['--', str(path)]
    result = subprocess.run(args, capture_output=True, text=True, timeout=60)
    if result.stderr.strip() or result.returncode not in (0, 1):
        raise ValueError('ownership check failed: ' + result.stderr.strip())
    return sorted(set(result.stdout.split()))


def timestamp(value):
    result = datetime.fromisoformat(value.replace('Z', '+00:00'))
    if result.tzinfo is None:
        raise ValueError('lifecycle timestamps must include a timezone')
    return result


def reference_scan(repo, path, snapshot):
    """Conservative reverse references, including untracked source documents.

    Every external metadata reference protects a candidate, regardless of whether
    the referring run is itself cited. This over-approximates transitive reach.
    Opaque/oversize metadata is unknown, never evidence of no references.
    """
    relative = path.relative_to(repo).as_posix()
    components = Path(relative).parts
    # Include ancestor run names, so directory citations protect descendants.
    start = 2 if components[:2] == ('experiments', 'output') else 1
    tokens = {relative, str(path), path.name}
    tokens.update(components[start:-1])
    tokens.update('/'.join(components[:i]) for i in range(start + 1, len(components)))
    if snapshot:
        tokens.add(snapshot['tree_digest'])
        tokens.update(row['sha256'] for row in snapshot['entries'] if 'sha256' in row)
    tokens.discard('')
    # Generic tiny basenames would match prose everywhere; full paths still apply.
    tokens = {token for token in tokens if len(token) >= 4}
    pattern = re.compile('|'.join(re.escape(token) for token in sorted(tokens)))
    references, unknown, scanned = [], [], 0
    roots = ['docs', 'specs', 'tests', 'agents', 'scripts', 'experiments/analysis', *ARTIFACT_ROOTS]
    roots += [p.name for p in sorted(repo.iterdir()) if p.is_file() and p.suffix.lower() in TEXT_SUFFIXES]
    for root_name in roots:
        base = repo / root_name
        if not base.exists():
            continue
        def failed(error):
            unknown.append(str(error))
        walked = os.walk(base, followlinks=False, onerror=failed) if base.is_dir() else [(base.parent, [], [base.name])]
        for parent, dirs, files in walked:
            parent = Path(parent)
            dirs[:] = sorted(d for d in dirs if d not in {'.git', '__pycache__'})
            for name in list(dirs):
                item = parent / name
                if item == store_path(repo):
                    dirs.remove(name)
                elif item.is_symlink():
                    unknown.append(f'symlinked reference directory: {item.relative_to(repo)}')
                    dirs.remove(name)
            for name in sorted(files):
                item = parent / name
                if item == path or path in item.parents:
                    continue
                if item.suffix.lower() not in TEXT_SUFFIXES | OPAQUE_SUFFIXES:
                    continue
                scanned += 1
                if scanned > MAX_REFERENCE_FILES:
                    return {'references': references, 'unknown': unknown + ['reference file limit'],
                            'files_scanned': scanned}
                label = item.relative_to(repo).as_posix()
                try:
                    _safe_path(item)
                    info = item.stat()
                    if not stat.S_ISREG(info.st_mode):
                        raise ValueError('nonregular metadata')
                    if item.suffix.lower() in OPAQUE_SUFFIXES:
                        raise ValueError('opaque dependency metadata needs an adapter')
                    if info.st_size > MAX_REFERENCE_BYTES:
                        raise ValueError('metadata exceeds scan bound')
                    content = item.read_text(encoding='utf-8')
                    if item.suffix.lower() == '.json':
                        parsed = json.loads(content)
                        # Decode JSON escapes before looking for path/digest references.
                        content += '\n' + json.dumps(parsed, ensure_ascii=False)
                    if pattern.search(content):
                        references.append(label)
                    after = item.stat()
                    if (info.st_ino, info.st_size, info.st_mtime_ns) != (after.st_ino, after.st_size, after.st_mtime_ns):
                        raise ValueError('metadata changed while scanning')
                except (OSError, ValueError, UnicodeError) as error:
                    unknown.append(f'{label}: {error}')
                if references or unknown:
                    return {'references': references, 'unknown': unknown,
                            'files_scanned': scanned, 'complete': False,
                            'note': 'stopped after a protecting reference or unresolved input'}
    # Retention records are not source citations, but retained dependency claims
    # survive pruning and must protect their original inputs and outputs.
    operations = store_path(repo) / 'operations'
    if operations.exists():
        for contents in sorted(operations.glob('*/contents.json')):
            try:
                data = read_json(contents)
                for source in data['sources']:
                    for dependency in source['review_evidence']:
                        dependency_path = source_path(repo, dependency['path'])
                        if (dependency_path == path or dependency_path in path.parents or
                                path in dependency_path.parents or
                                (snapshot and dependency['sha256'] == snapshot['tree_digest'])):
                            references.append(contents.relative_to(repo).as_posix())
            except (OSError, ValueError, KeyError, TypeError) as error:
                unknown.append(f'{contents.relative_to(repo)}: {error}')
    return {'references': sorted(set(references)), 'unknown': sorted(set(unknown)),
            'files_scanned': scanned, 'complete': not unknown}


def pin_matches(repo, path, snapshot):
    pins_path = store_path(repo) / 'pins.json'
    if not pins_path.exists():
        return []
    data = read_json(pins_path)
    if not isinstance(data, dict) or not isinstance(data.get('pins'), list):
        raise ValueError('invalid pins.json: expected {"pins": [...]}')
    matches = []
    for pin in data['pins']:
        if not isinstance(pin, dict) or not pin.get('reason') or not (pin.get('path') or pin.get('sha256')):
            raise ValueError('each pin requires a path/digest and reason')
        if pin.get('path'):
            pinned = source_path(repo, pin['path'])
            if pinned == path or pinned in path.parents or path in pinned.parents:
                matches.append(pin)
        if pin.get('sha256') and snapshot and pin['sha256'] in {
                snapshot['tree_digest'], *(e.get('sha256') for e in snapshot['entries'])}:
            matches.append(pin)
    return matches


def lifecycle_check(repo, review, action, at):
    blockers = []
    if not review:
        return ['missing explicit lifecycle/use review'], []
    if review.get('owner_closed') is not True or review.get('recovery_resolved') is not True:
        blockers.append('owner closure and recovery resolution are required')
    if action != 'archive-copy' and review.get('reference_scope_complete') is not True:
        blockers.append('review must confirm all dependency locations are in the scan scope')
    if not review.get('reviewer') or not review.get('reason'):
        blockers.append('reviewer and rationale are required')
    evidence = []
    for record in review.get('evidence', []):
        try:
            item = source_path(repo, record['path'])
            actual = sha256(item)
            if actual != record['sha256']:
                raise ValueError('evidence digest changed')
            evidence.append({'path': item.relative_to(repo).as_posix(), 'sha256': actual, 'compact_copy': True})
        except (KeyError, OSError, ValueError) as error:
            blockers.append(f'invalid lifecycle evidence: {error}')
    if not evidence:
        blockers.append('digest-bound lifecycle evidence is required')
    try:
        completed = timestamp(review['completed_at'])
        used = timestamp(review['last_used_at'])
        if completed > at or used > at:
            raise ValueError('future lifecycle/use time')
        age = (at - max(completed, used)).total_seconds() / 86400
        threshold = {'archive-copy': 0, 'archive': 14, 'delete': 90, 'prune': 7}[action]
        if review.get('outcome') not in ('passed', 'failed', 'rejected'):
            blockers.append('unknown outcome')
        if review.get('outcome') in ('failed', 'rejected') and action != 'archive-copy':
            threshold = max(30, threshold)
            if review.get('issue_resolved') is not True:
                blockers.append('failed/rejected issue is unresolved')
        if action == 'prune' and review.get('class') == 'scratch':
            threshold = 0 if review.get('outcome') == 'passed' else threshold
        if age < threshold:
            blockers.append(f'review age {age:.2f} days below {threshold} days')
    except (KeyError, ValueError, TypeError) as error:
        blockers.append(f'missing/invalid completion or last-use timestamp: {error}')
    if action == 'prune':
        # No shell commands are executed from a review. Regeneration is verified
        # separately and attached as digest-bound evidence plus retained inputs.
        if review.get('class') not in ('scratch', 'derived'):
            blockers.append('prune requires scratch or derived class')
        if not review.get('verified_outputs') or not review.get('retained_inputs'):
            blockers.append('prune requires verified outputs and retained inputs')
        if review.get('class') == 'derived' and not review.get('regeneration'):
            blockers.append('derived pruning requires a verified regeneration record')
        for key in ('verified_outputs', 'retained_inputs'):
            for record in review.get(key, []):
                try:
                    item = source_path(repo, record['path'])
                    if sha256(item) != record['sha256']:
                        raise ValueError('dependency digest mismatch')
                    evidence.append({'path': item.relative_to(repo).as_posix(), 'sha256': record['sha256'], 'compact_copy': False})
                except (KeyError, OSError, ValueError) as error:
                    blockers.append(f'invalid {key}: {error}')
    return blockers, evidence


def assess(repo, value, action, review, *, at=None, scan=True):
    path = source_path(repo, value)
    at = at or datetime.now(timezone.utc)
    result = {'path': path.relative_to(repo).as_posix(), 'action': action,
              'review': review, 'blockers': [], 'snapshot': None}
    reasons, evidence = lifecycle_check(repo, review, action, at)
    result['blockers'].extend(reasons)
    result['evidence'] = evidence
    info = path.lstat()
    result['logical_bytes'] = info.st_size if path.is_file() else None
    result['allocated_bytes'] = info.st_blocks * 512 if path.is_file() else None
    if action != 'archive-copy':
        if not path.is_file():
            result['blockers'].append('eviction supports individual bulk files only; preserve bundle metadata')
        if path.suffix.lower() not in BULK_SUFFIXES or CONTROL_WORDS.search(path.name):
            result['blockers'].append('metadata/unknown file class is never evicted')
        if stat.S_ISREG(info.st_mode) and info.st_nlink != 1:
            result['blockers'].append('hardlinked source requires separate allocation review')
    siblings = [Path(str(path) + suffix) for suffix in ('-wal', '-shm', '-journal')]
    if any(p.exists() or p.is_symlink() for p in siblings) or path.name.endswith(('-wal', '-shm', '-journal')):
        result['blockers'].append('unresolved SQLite WAL/SHM/journal')
    try:
        handles = open_handles(path)
        if handles:
            result['blockers'].append('open source handles: ' + ','.join(handles))
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        result['blockers'].append(f'ownership unknown: {error}')
    # Avoid hashing gigabytes already blocked by missing lifecycle records.
    if not result['blockers']:
        result['snapshot'] = snapshot_tree(path)
        result['logical_bytes'] = result['snapshot']['total_bytes']
    pins = pin_matches(repo, path, result['snapshot'])
    result['pins'] = pins
    if pins and action != 'archive-copy':
        result['blockers'].append('explicitly pinned')
    refs = reference_scan(repo, path, result['snapshot']) if scan else {'references': [], 'unknown': ['scan disabled'], 'files_scanned': 0}
    result['reference_scan'] = refs
    if action != 'archive-copy':
        if refs['references']:
            result['blockers'].append('referenced evidence requires its materialized path')
        if refs['unknown']:
            result['blockers'].append('reference graph has unknown inputs')
        if any(e['path'] == result['path'] for e in evidence):
            result['blockers'].append('candidate is required review/dependency evidence')
    result['eligible'] = not result['blockers']
    return result


def budget(floor, note):
    if floor < 0:
        raise ValueError('free-space floor must be nonnegative')
    if floor != DEFAULT_FLOOR and not note:
        raise ValueError('nondefault free-space floor requires --budget-note')
    return {'free_space_floor_bytes': floor, 'override_reason': note,
            'derived_analysis_budget_bytes': 10 * GIB}


def make_plan(repo, candidates, action, reviews, floor=DEFAULT_FLOOR, note=''):
    policy = budget(floor, note)
    if action not in ('archive-copy', 'archive', 'delete', 'prune'):
        raise ValueError('unsupported action')
    normalized = [source_path(repo, value).relative_to(repo).as_posix() for value in candidates]
    if len(normalized) != len(set(normalized)):
        raise ValueError('duplicate candidate')
    entries = [assess(repo, value, action, reviews.get(value)) for value in normalized]
    for entry in entries:
        for other in normalized:
            if other != entry['path'] and (other in [e['path'] for e in entry['evidence']] or
                    entry['path'].startswith(other + '/') or other.startswith(entry['path'] + '/')):
                entry['blockers'].append('overlapping candidate or retained dependency selected')
                entry['eligible'] = False
                for dependency in entries:
                    if dependency['path'] == other:
                        dependency['blockers'].append('selected by another candidate as a retained dependency or overlapping source')
                        dependency['eligible'] = False
    return {'schema': SCHEMA, 'created_at': now(), 'repo': str(repo), 'policy': policy,
            'entries': entries, 'mode': 'explicit-selection',
            'estimated_reclaimed_bytes': sum(e['logical_bytes'] or 0 for e in entries
                if e['eligible'] and e['action'] in ('delete', 'prune')),
            'archive_savings': 'unknown until compression; logical bytes are not allocated savings'}


def save_plan(repo, plan):
    with lease(repo) as store:
        operation = store / 'operations' / (datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + uuid.uuid4().hex[:12])
        _safe_path(operation).mkdir(parents=True)
        write_json(operation / 'plan.json', plan)
        return operation / 'plan.json'


def remove_file(path, snapshot, operation, result, receipt):
    """Quarantine one checked file; never recursively remove an evidence tree."""
    if snapshot_tree(path) != snapshot or open_handles(path):
        raise ValueError('source changed or reopened before removal')
    quarantine = path.with_name('.' + path.name + '.retention-' + operation.name)
    _safe_path(quarantine)
    if quarantine.exists():
        raise ValueError('quarantine already exists; inspect interrupted operation')
    result['quarantine'] = str(quarantine)
    result['phase'] = 'quarantine-intent'
    write_json(operation / 'receipt.json', receipt)
    _rename_noreplace(path, quarantine)
    result['materialized_path'] = str(quarantine)
    try:
        sync_dir(path.parent)
        actual = snapshot_tree(quarantine)
        # Snapshot root/member names necessarily changed during quarantine.
        actual['root_name'] = snapshot['root_name']
        actual['entries'][0]['path'] = snapshot['entries'][0]['path']
        if actual != snapshot or open_handles(quarantine):
            raise ValueError('quarantined source changed or has open handles')
        if path.exists() or path.is_symlink():
            raise ValueError('source path was recreated; quarantine retained')
        result['phase'] = 'unlink-intent'
        write_json(operation / 'receipt.json', receipt)
        quarantine.unlink()
        result['availability'] = {'archive': 'archived', 'delete': 'deleted',
                                  'prune': 'pruned-derived'}[result['action']]
        result['logical_bytes_removed'] = snapshot['total_bytes']
        result['materialized_path'] = None
        result['phase'] = 'unlinked-awaiting-directory-sync'
        write_json(operation / 'receipt.json', receipt)
        sync_dir(path.parent)
    except BaseException:
        if quarantine.exists() and not path.exists() and not path.is_symlink():
            _rename_noreplace(quarantine, path)
            result['materialized_path'] = str(path)
            sync_dir(path.parent)
        raise


def apply_plan(repo, plan_path, zstd='zstd'):
    plan_path = _safe_path(plan_path)
    operations = store_path(repo) / 'operations'
    if plan_path.name != 'plan.json' or plan_path.parent.parent != operations:
        raise ValueError('apply requires a saved operations/<id>/plan.json')
    with lease(repo) as store:
        plan = read_json(plan_path)
        if plan.get('schema') != SCHEMA or plan.get('repo') != str(repo):
            raise ValueError('plan schema/repository mismatch')
        paths = [source_path(repo, e['path']).relative_to(repo).as_posix() for e in plan['entries']]
        if len(paths) != len(set(paths)):
            raise ValueError('duplicate plan candidate')
        for entry in plan['entries']:
            if entry['action'] not in ('archive-copy', 'archive', 'delete', 'prune'):
                raise ValueError('invalid planned action')
            for other in paths:
                if other != entry['path'] and (entry['path'].startswith(other + '/') or
                        other.startswith(entry['path'] + '/') or
                        any(e['path'] == other for e in entry['evidence'])):
                    raise ValueError('plan selects overlapping sources or retained dependencies')
        operation = plan_path.parent
        if (operation / 'receipt.json').exists():
            raise ValueError('operation already attempted; inspect receipt and create a fresh plan')
        policy = plan['policy']
        budget(policy['free_space_floor_bytes'], policy['override_reason'])
        receipt = {'schema': SCHEMA, 'plan_sha256': sha256(plan_path), 'started_at': now(),
                   'policy': policy, 'status': 'running', 'actions': [],
                   'free_bytes_before': shutil.disk_usage(store).free}
        write_json(operation / 'receipt.json', receipt)
        contents = {'schema': SCHEMA, 'sources': []}
        try:
            for entry in plan['entries']:
                result = {'path': entry['path'], 'action': entry['action'],
                          'availability': 'materialized', 'status': 'blocked'}
                receipt['actions'].append(result)
                if not entry['eligible']:
                    result['blockers'] = entry['blockers']
                    write_json(operation / 'receipt.json', receipt)
                    continue
                fresh = assess(repo, entry['path'], entry['action'], entry['review'])
                if (not fresh['eligible'] or fresh['snapshot'] != entry['snapshot'] or
                        fresh['evidence'] != entry['evidence']):
                    raise ValueError('stale plan: identity, lifecycle, ownership, pins or references changed')
                path = source_path(repo, entry['path'])
                contents['sources'].append({'path': entry['path'], 'snapshot': entry['snapshot'],
                                            'review_evidence': entry['evidence']})
                write_json(operation / 'contents.json', contents)
                result['status'], result['phase'] = 'running', 'source-verified'
                write_json(operation / 'receipt.json', receipt)
                # Freeze compact lifecycle evidence independently of original runs.
                evidence_bytes = sum(source_path(repo, record['path']).stat().st_size + 8192
                                     for record in entry['evidence'] if record['compact_copy'])
                journal_bytes = len(json.dumps(entry).encode()) * 4 + 1024 * 1024
                if shutil.disk_usage(store).free < policy['free_space_floor_bytes'] + evidence_bytes + journal_bytes:
                    raise OSError('insufficient free space for evidence and operation records')
                evidence_dir = operation / 'evidence'
                evidence_dir.mkdir(exist_ok=True)
                for record in entry['evidence']:
                    if not record['compact_copy']:
                        continue
                    source = source_path(repo, record['path'])
                    if source.stat().st_size > MAX_REFERENCE_BYTES:
                        raise ValueError('lifecycle evidence must be compact')
                    target = evidence_dir / record['sha256']
                    if not target.exists():
                        with target.open('xb') as output, source.open('rb') as stream:
                            shutil.copyfileobj(stream, output)
                            output.flush()
                            os.fsync(output.fileno())
                    if sha256(target) != record['sha256']:
                        raise ValueError('retained lifecycle evidence mismatch')
                sync_dir(evidence_dir)
                if entry['action'] in ('archive-copy', 'archive'):
                    archives = _safe_path(store / 'archives')
                    archives.mkdir(exist_ok=True)
                    result['codec'] = subprocess.check_output([zstd, '--version'], text=True).strip()
                    result['codec_options'] = ['-3', '-T1']
                    result['archive'] = archive_verified(path, archives, operation,
                        entry['snapshot'], policy['free_space_floor_bytes'], zstd=zstd)
                    result['restore_verified'] = True
                    write_json(operation / 'receipt.json', receipt)
                    if entry['action'] == 'archive' and result['archive']['size'] >= entry['snapshot']['total_bytes']:
                        result['status'], result['phase'] = 'skipped', 'compression-not-beneficial'
                        write_json(operation / 'receipt.json', receipt)
                        continue
                if entry['action'] != 'archive-copy':
                    # Repeat the entire safety gate after potentially long compression.
                    final = assess(repo, entry['path'], entry['action'], entry['review'])
                    if not final['eligible'] or final['snapshot'] != entry['snapshot']:
                        raise ValueError('source/reference/ownership changed before eviction')
                    remove_file(path, entry['snapshot'], operation, result, receipt)
                    result['availability'] = {'archive': 'archived', 'delete': 'deleted',
                                              'prune': 'pruned-derived'}[entry['action']]
                    result['logical_bytes_removed'] = entry['snapshot']['total_bytes']
                result['status'], result['phase'] = 'complete', 'complete'
                write_json(operation / 'receipt.json', receipt)
            receipt['status'] = 'complete-with-blocked' if any(
                a['status'] == 'blocked' for a in receipt['actions']) else 'complete'
        except BaseException as error:
            receipt['status'], receipt['error'] = 'interrupted', str(error)
            write_json(operation / 'receipt.json', receipt)
            raise
        finally:
            receipt['finished_at'] = now()
            receipt['free_bytes_after'] = shutil.disk_usage(store).free
            receipt['filesystem_free_delta_bytes'] = receipt['free_bytes_after'] - receipt['free_bytes_before']
            write_json(operation / 'receipt.json', receipt)
        return receipt


def restore(repo, receipt_path, destination, zstd):
    if receipt_path.name != 'receipt.json' or receipt_path.parent.parent != store_path(repo) / 'operations':
        raise ValueError('restore requires an operation receipt')
    receipt = read_json(receipt_path)
    actions = [a for a in receipt['actions'] if a.get('archive')]
    if len(actions) != 1:
        raise ValueError('restore requires a receipt with exactly one archived source')
    archive = actions[0]['archive']
    archive_path = _safe_path(Path(archive['archive_path']))
    if sha256(archive_path) != archive['sha256']:
        raise ValueError('archive digest mismatch')
    with lease(repo):
        result = restore_verified(archive_path, archive['contents'], destination,
                                  receipt['policy']['free_space_floor_bytes'], zstd=zstd)
        # Restore records are separate; never rewrite sealed operation receipts.
        record = receipt_path.parent / ('restore-' + uuid.uuid4().hex + '.json')
        write_json(record, {'schema': SCHEMA, 'restored_at': now(), **result})
        return result


def inventory(repo):
    rows = []
    for root_name in ARTIFACT_ROOTS:
        base = repo / root_name
        if not base.exists():
            continue
        for path in sorted(base.iterdir()):
            if path == store_path(repo):
                continue
            logical, allocated, files, errors = 0, 0, 0, []
            paths = path.rglob('*') if path.is_dir() and not path.is_symlink() else [path]
            for item in paths:
                try:
                    _safe_path(item)
                    info = item.lstat()
                    if stat.S_ISREG(info.st_mode):
                        logical += info.st_size
                        allocated += info.st_blocks * 512
                        files += 1
                except (OSError, ValueError) as error:
                    errors.append(str(error))
            rows.append({'path': path.relative_to(repo).as_posix(), 'logical_bytes': logical,
                         'allocated_bytes': allocated, 'files': files,
                         'availability': 'materialized', 'eligibility': 'not-reviewed', 'errors': errors})
    return {'schema': SCHEMA, 'created_at': now(), 'free_bytes': shutil.disk_usage(repo).free,
            'entries': sorted(rows, key=lambda row: row['allocated_bytes'], reverse=True)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=ROOT)
    parser.add_argument('--zstd', default=shutil.which('zstd') or 'zstd')
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('inventory')
    plan = sub.add_parser('plan')
    plan.add_argument('--candidate', action='append', required=True)
    plan.add_argument('--action', choices=['archive-copy', 'archive', 'delete', 'prune'], default='archive-copy')
    plan.add_argument('--review', type=Path, help='JSON mapping repository-relative candidates to explicit lifecycle reviews')
    plan.add_argument('--free-space-floor-gib', type=float, default=20)
    plan.add_argument('--budget-note', default='')
    apply = sub.add_parser('apply')
    apply.add_argument('--plan', type=Path, required=True)
    recovery = sub.add_parser('restore')
    recovery.add_argument('--receipt', type=Path, required=True)
    recovery.add_argument('--destination', type=Path, required=True)
    args = parser.parse_args()
    repo = _safe_path(args.repo.absolute())
    try:
        if args.command == 'inventory':
            value = inventory(repo)
        elif args.command == 'plan':
            reviews = read_json(args.review) if args.review else {}
            value = make_plan(repo, args.candidate, args.action, reviews,
                              int(args.free_space_floor_gib * GIB), args.budget_note)
            location = save_plan(repo, value)
            value = {'plan': str(location), 'eligible': sum(e['eligible'] for e in value['entries']),
                     'blocked': sum(not e['eligible'] for e in value['entries'])}
        elif args.command == 'apply':
            value = apply_plan(repo, args.plan.absolute(), args.zstd)
        else:
            value = restore(repo, args.receipt.absolute(), args.destination.absolute(), args.zstd)
        print(json.dumps(value, indent=2, sort_keys=True))
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        parser.exit(1, f'retention: {error}\n')


if __name__ == '__main__':
    main()
