#!/usr/bin/env python3
"""Check archived OFF/ON or default/retired-selector replay with real reuse."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics


def check(off, on, default=False):
    receipts = [json.loads((path / 'result.json').read_text()) for path in (off, on)]
    a, b = receipts
    for key in ('provider', 'input_components', 'forced_environment', 'source_boundary_mode'):
        if a[key] != b[key]:
            raise ValueError(f'mismatched {key}')
    environments = [{k: v for k, v in r['renderer_environment'].items()
                     if k != 'DXMT9_COMMAND_BINDING_PLAN_REUSE'} for r in receipts]
    if environments[0] != environments[1]:
        raise ValueError('uncontrolled renderer environment')
    if a['result']['loaded_images'] != b['result']['loaded_images']:
        raise ValueError('loaded image drift')
    images, reports = None, []
    policies = (None, '0') if default else ('0', '1')
    for enabled, path, receipt in zip(policies, (off, on), receipts):
        if not (receipt['accepted'] and receipt['readback_artifacts_valid'] and
                receipt['loaded_images_stable']):
            raise ValueError('unqualified receipt')
        if receipt['renderer_environment'].get('DXMT9_COMMAND_BINDING_PLAN_REUSE') != enabled:
            raise ValueError('wrong binding-plan policy')
        times = []
        for iteration in receipt['result']['iterations']:
            if not iteration['reset_ok']:
                raise ValueError('reset failed')
            pixels = {}
            for key, artifact in iteration['readback_artifacts'].items():
                data = (path / 'readback' / artifact['path']).read_bytes()
                if (len(data) != artifact['bytes'] or
                        hashlib.sha256(data).hexdigest() != artifact['sha256'] or
                        len(set(data)) < 2):
                    raise ValueError('invalid or degenerate readback')
                pixels[key] = data
            if not pixels:
                raise ValueError('no readbacks')
            if images is None:
                images = pixels
            if pixels != images:
                raise ValueError('GPU exact mismatch')
            if not iteration['warmup']:
                times.append(iteration['cpu_scope_wall_ns']['encode_chunk'])
        stderr = (path / 'provider.stderr').read_text()
        def counter(name):
            values = re.findall(r'\b' + re.escape(name) + r'=(\d+)', stderr)
            if not values:
                raise ValueError(f'missing {name}')
            return int(values[-1])
        builds = counter('encode_draw_command_binding_plan_builds')
        reuses = counter('encode_draw_command_binding_plan_reuses')
        if ((default or enabled == '1') and (builds == 0 or reuses == 0)) or (
                not default and enabled == '0' and (builds or reuses)):
            raise ValueError('vacuous or wrong candidate coverage')
        if counter('gpu_command_buffer_errors'):
            raise ValueError('GPU errors')
        reports.append(dict(policy=enabled, builds=builds, reuses=reuses,
                            measured_iterations=len(times),
                            encode_elapsed_median_ms=statistics.median(times)/1e6))
    return dict(exact=True, reports=reports,
                readbacks={k: hashlib.sha256(v).hexdigest() for k, v in images.items()},
                scope='Fixed-input elapsed encode scopes; not native CPU or FPS proof.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('off', type=Path)
    parser.add_argument('on', type=Path)
    parser.add_argument('--default', action='store_true',
                        help='Require reuse with selector unset and retired selector=0')
    args = parser.parse_args()
    print(json.dumps(check(args.off, args.on, args.default), indent=2))


if __name__ == '__main__':
    main()
