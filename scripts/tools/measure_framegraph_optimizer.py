#!/usr/bin/env python3
"""Same-input optimizer graph equality and native CPU budgets, never FPS proof."""
import argparse
import csv
import hashlib
import io
import json
import statistics
import subprocess
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', type=Path, nargs='+')
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    probe = args.probe.resolve()
    image = digest(probe)
    runtime = Path(subprocess.check_output([str(probe), '--runtime-image'],
                                          text=True, timeout=30).strip()).resolve()
    runtime_hash = digest(runtime)
    reports = []
    for index, path in enumerate(args.inputs):
        source_hash = digest(path)
        result = subprocess.run([str(probe), str(path.resolve())],
                                capture_output=True, text=True, timeout=300)
        prefix = args.output / str(index)
        prefix.with_suffix('.csv').write_text(result.stdout)
        prefix.with_suffix('.stderr').write_text(result.stderr)
        if result.returncode or not result.stderr.startswith('PASS exact optimizer output equality;'):
            raise RuntimeError(f'unqualified probe {path}: {result.stderr}')
        rows = list(csv.DictReader(io.StringIO(result.stdout)))
        if len(rows) != 128:
            raise ValueError('missing/duplicate trial rows')
        seen, groups = set(), {}
        for row in rows:
            key = (int(row['round']), int(row['diagnostics']), row['mode'], row['stage'])
            if key in seen or not 0 <= key[0] < 16 or key[1] not in (0, 1):
                raise ValueError('invalid trial identity')
            if ((key[2] == 'whole' and key[3] != 'optimizer') or
                (key[2] == 'split' and key[3] not in ('lifetime', 'coalesce', 'loadstore')) or
                key[2] not in ('whole', 'split')):
                raise ValueError('invalid trial phase')
            seen.add(key)
            group = groups.setdefault('/'.join(map(str, key[1:])), [])
            group.append(row)
        summary = {}
        for key, group in groups.items():
            if len(group) != 16:
                raise ValueError('incomplete group')
            summary[key] = {
                metric.replace('_ns', '_ms'): statistics.median(int(r[metric]) for r in group) / 1e6
                for metric in ('cpu_ns', 'wall_ns')
            }
            summary[key]['merged'] = sorted({int(r['merged']) for r in group})
            summary[key]['candidates'] = sorted({int(r['candidates']) for r in group})
            summary[key]['rejections'] = {
                reason: sorted({int(r[reason]) for r in group})
                for reason in ('blocked_cycle', 'second_non_draw',
                               'non_render_intervener', 'missing_invariant')
            }
        if digest(probe) != image or digest(path) != source_hash or digest(runtime) != runtime_hash:
            raise ValueError('binary/input changed during measurement')
        reports.append(dict(input=str(path), sha256=source_hash,
                            equality=result.stderr.strip(), median_ms=summary))
    output = dict(probe=str(probe), probe_sha256=image,
                  runtime=str(runtime), runtime_sha256=runtime_hash, warmups_per_policy=2,
                  measured_pairs=16, order='alternating OFF/ON and ON/OFF',
                  copy_and_parse_timed=False, reports=reports,
                  limitations=['Graph output equality, not GPU pixels or FPS.',
                               'Coalescing includes its internal lifetime refresh.',
                               'Microsecond clock overhead matters for tiny phases.'])
    (args.output / 'result.json').write_text(json.dumps(output, indent=2)+'\n')
    for report in reports:
        print(report['equality'])
        for key, values in report['median_ms'].items():
            print(key, values)


if __name__ == '__main__':
    main()
