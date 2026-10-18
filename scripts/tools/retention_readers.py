#!/usr/bin/env python3
"""Digest-bound reader parity contracts for recoverable referenced-file archival."""
from __future__ import annotations

import argparse
import hashlib
import importlib
import json
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'scripts/tools'))
from scripts.tools.retention_archive import restore_verified, snapshot_tree, _safe_path

SCHEMA = 'dxmt9.retention.reader-qualification.v1'
CONTRACTS = {'cpu-supply-v2': 'analyze_cpu_supply', 'wait-trace-v1': 'summarize_wait_trace'}


def digest(path):
    value = hashlib.sha256()
    with _safe_path(Path(path)).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def implementation_identity(contract):
    if contract not in CONTRACTS:
        raise ValueError('unsupported reader contract')
    paths = [f'scripts/tools/{CONTRACTS[contract]}.py',
             'scripts/tools/retention_readers.py', 'scripts/tools/retention_resolver.py',
             'scripts/tools/retention_archive.py', 'scripts/check/audit_perf_docs_sources.py']
    return {path: digest(ROOT / path) for path in paths}


def canonical_digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':'),
                                    allow_nan=False).encode()).hexdigest()


def qualify(repo, receipt_path, contract, zstd):
    # Local import avoids making policy validation depend on command dispatch.
    from scripts.tools import retain_trace_artifacts as retention
    receipt_path = _safe_path(Path(receipt_path).absolute())
    if receipt_path.name != 'receipt.json' or receipt_path.parent.parent != retention.store_path(repo) / 'operations':
        raise ValueError('qualification requires a retention operation receipt')
    receipt = retention.read_json(receipt_path)
    plan_path = receipt_path.parent / 'plan.json'
    if receipt.get('status') not in ('complete', 'complete-with-blocked') or receipt.get('plan_sha256') != digest(plan_path):
        raise ValueError('incomplete or mismatched archive operation')
    plan = retention.read_json(plan_path)
    actions = [a for a in receipt['actions'] if a.get('status') == 'complete' and a.get('archive')]
    if len(actions) != 1:
        raise ValueError('qualification requires exactly one successful archive action')
    action = actions[0]
    matching = [e for e in plan['entries'] if e['path'] == action['path'] and e['action'] == action['action']]
    archive = action['archive']
    if len(matching) != 1 or matching[0]['snapshot'] != archive['contents']:
        raise ValueError('archive snapshot differs from frozen plan')
    source = retention.source_path(repo, action['path'])
    if source.suffix.lower() != '.csv' or not source.is_file():
        raise ValueError('reader qualification requires a materialized CSV source')
    expected = archive['contents']
    if snapshot_tree(source) != expected:
        raise ValueError('source changed since archive publication')
    archive_path = _safe_path(Path(archive['archive_path']))
    if archive_path != retention.store_path(repo) / 'archives' / (archive['sha256'] + '.tar.zst'):
        raise ValueError('archive path is outside the content-addressed store')
    if digest(archive_path) != archive['sha256']:
        raise ValueError('archive digest mismatch')
    identities = implementation_identity(contract)
    reader = importlib.import_module(CONTRACTS[contract])
    original = canonical_digest(reader.load(source))
    with tempfile.TemporaryDirectory(prefix='reader-parity-', dir=receipt_path.parent) as staging:
        restored = Path(staging) / source.name
        restore_verified(archive_path, expected, restored,
                         receipt['policy']['free_space_floor_bytes'], zstd=zstd)
        recovered = canonical_digest(reader.load(restored))
    if original != recovered or snapshot_tree(source) != expected:
        raise ValueError('original/restored reader parity or source stability failed')
    if implementation_identity(contract) != identities:
        raise ValueError('reader implementation changed during qualification')
    report = {'schema': SCHEMA, 'qualified_at': retention.now(), 'contract': contract,
              'source': action['path'], 'source_sha256': expected['tree_digest'],
              'source_bytes': expected['total_bytes'], 'archive_sha256': archive['sha256'],
              'archive_receipt': str(receipt_path.relative_to(repo)),
              'archive_receipt_sha256': digest(receipt_path), 'implementations': identities,
              'original_reader_sha256': original, 'restored_reader_sha256': recovered,
              'qualified': True, 'scope': 'storage/schema parity; no new scientific analysis claim'}
    output = receipt_path.parent / ('reader-qualification-' + contract + '.json')
    if output.exists():
        raise FileExistsError('qualification already exists; create a fresh archive-copy operation')
    retention.write_json(output, report)
    return output


def validate_qualification(repo, source, snapshot, review):
    """Return a frozen qualification identity or raise; no review flag bypasses it."""
    from scripts.tools import retain_trace_artifacts as retention
    contract = review.get('reader_contract')
    if contract not in CONTRACTS or review.get('reader_scope_complete') is not True:
        raise ValueError('referenced archival requires a qualified contract and explicit reader scope review')
    if source.suffix.lower() != '.csv' or not snapshot or snapshot['kind'] != 'file':
        raise ValueError('reader contract admits individual CSV files only')
    record = review.get('reader_qualification', {})
    raw = Path(record.get('path', ''))
    if '..' in raw.parts:
        raise ValueError('qualification path traversal')
    path = _safe_path(raw if raw.is_absolute() else repo / raw)
    if path.parent.parent != retention.store_path(repo) / 'operations' or not path.name.startswith('reader-qualification-'):
        raise ValueError('qualification must be an operation record')
    if digest(path) != record.get('sha256'):
        raise ValueError('reader qualification changed')
    value = retention.read_json(path)
    if (value.get('schema') != SCHEMA or value.get('qualified') is not True or
            value.get('contract') != contract or value.get('source') != source.relative_to(repo).as_posix() or
            value.get('source_sha256') != snapshot['tree_digest'] or
            value.get('source_bytes') != snapshot['total_bytes'] or
            value.get('implementations') != implementation_identity(contract) or
            value.get('original_reader_sha256') != value.get('restored_reader_sha256') or
            not value.get('original_reader_sha256')):
        raise ValueError('reader qualification/source/implementation binding mismatch')
    receipt_path = _safe_path(repo / value['archive_receipt'])
    if receipt_path.parent != path.parent or receipt_path.name != 'receipt.json' or digest(receipt_path) != value['archive_receipt_sha256']:
        raise ValueError('qualification archive receipt changed')
    receipt = retention.read_json(receipt_path)
    plan_path = receipt_path.parent / 'plan.json'
    if receipt.get('status') not in ('complete', 'complete-with-blocked') or receipt.get('plan_sha256') != digest(plan_path):
        raise ValueError('qualification operation is incomplete or plan changed')
    actions = [a for a in receipt['actions'] if a.get('status') == 'complete' and a.get('path') == value['source'] and a.get('archive')]
    if len(actions) != 1 or actions[0]['archive']['sha256'] != value['archive_sha256'] or actions[0]['archive']['contents']['tree_digest'] != snapshot['tree_digest']:
        raise ValueError('qualification archive binding mismatch')
    archive = actions[0]['archive']
    archive_path = _safe_path(Path(archive['archive_path']))
    if archive_path != retention.store_path(repo) / 'archives' / (archive['sha256'] + '.tar.zst') or digest(archive_path) != archive['sha256']:
        raise ValueError('qualified archive is unavailable or corrupt')
    return {'path': str(path.relative_to(repo)), 'sha256': record['sha256'],
            'contract': contract, 'archive_sha256': value['archive_sha256']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--receipt', type=Path, required=True)
    parser.add_argument('--contract', choices=sorted(CONTRACTS), required=True)
    parser.add_argument('--zstd', default='zstd')
    args = parser.parse_args()
    print(qualify(ROOT, args.receipt, args.contract, args.zstd))


if __name__ == '__main__':
    main()
