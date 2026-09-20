#!/usr/bin/env python3
"""Difference complete guest-PC snapshots bracketed by encoded frame logs.

Stopped guest contexts are not exclusive CPU time; bridge PCs can be saved
contexts during Unix work/waits. Never multiply these shares by thread CPU.
"""
import argparse
import json
import re
from pathlib import Path

from symbolicate_xctrace_pe import parse_sampler_log, validate_sampler_coverage


class TextSource:
    def __init__(self, text):
        self.text = text

    def read_text(self, **kwargs):
        return self.text


def summarize(text, first, last):
    if 'target_thread_mismatch' in text:
        raise ValueError('sampler target mismatch')
    groups, lines, frame, anchor = [], [], None, None
    def close():
        if not lines:
            return
        report = parse_sampler_log(TextSource('\n'.join(lines)))
        if report is None:
            return
        errors = validate_sampler_coverage(report)
        if errors or not report.coverage or report.coverage.get('complete') != 1:
            raise ValueError(f'incomplete sampler group: {errors}')
        if any(report.header.get(k, -1) != 0 for k in
               ('suspend_failures', 'ctx_failures', 'resume_failures')):
            raise ValueError('failed sampler operations')
        omitted = report.samples-sum(v for _, v in report.modules)
        if omitted < 0:
            raise ValueError('module sample population exceeds total')
        # The runtime exports only its top 20 modules. Preserve the residual;
        # it is neither zero nor attributable to any named library.
        report.modules.append(('unreported-module-tail', omitted))
        groups.append((anchor, report))
    for line in text.splitlines():
        match = re.search(r'\[dxmt9-perf-frame frame=(\d+)', line)
        if match:
            frame = int(match[1])
        if '[dxmt9-pe-sampler]' not in line:
            continue
        if re.search(r'\bpresents=\d+.*\bsamples=', line):
            close()
            lines, anchor = [], frame
        lines.append(line)
    close()
    selected = [(f, r) for f, r in groups if f is not None and first <= f <= last]
    if len(selected) < 2:
        raise ValueError('insufficient interior sampler reports')
    (a, before), (b, after) = selected[0], selected[-1]
    old, new = dict(before.modules), dict(after.modules)
    if old.keys() != new.keys():
        raise ValueError('top-module membership changed across endpoints')
    delta = {k: new.get(k, 0)-old.get(k, 0) for k in old.keys() | new.keys()}
    count = after.samples-before.samples
    if count <= 0 or any(v < 0 for v in delta.values()) or sum(delta.values()) != count:
        raise ValueError('nonconserved interval population')
    return dict(first_frame_anchor=a, last_frame_anchor=b,
        first_pe_present=before.header['presents'], last_pe_present=after.header['presents'],
        samples=count, modules=[dict(module=k, samples=v, percent=100*v/count)
                               for k, v in sorted(delta.items(), key=lambda p: -p[1]) if v],
        qualification='Approximate log-frame anchors; stopped guest-PC population, not exclusive CPU')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--first', required=True, type=int)
    parser.add_argument('--last', required=True, type=int)
    args = parser.parse_args()
    try:
        print(json.dumps(summarize(args.log.read_text(), args.first, args.last), indent=2))
    except (ValueError, KeyError, OSError) as error:
        parser.exit(1, f'unqualified sampler window: {error}\n')
