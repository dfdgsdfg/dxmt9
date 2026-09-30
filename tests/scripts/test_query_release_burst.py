"""Real serialized query/pixel oracle plus mandatory release-outcome evidence."""
import collections
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts/tools'))
import analyze_cpu_supply as supply
import analyze_session_release as release

with tempfile.TemporaryDirectory(prefix='dxmt9-release-burst-') as directory:
    trace = Path(directory) / 'supply.csv'
    env = dict(os.environ, DXMT9_CPU_SUPPLY_TRACE=str(trace),
               DXMT9_CPU_SUPPLY_TRACE_FIRST_PRESENT='1',
               DXMT9_CPU_SUPPLY_TRACE_LAST_PRESENT='2')
    subprocess.run([*sys.argv[1:], '--release-burst'], env=env, check=True, timeout=25)
    report = release.analyze(*supply.load(trace), require_close=True)
    rows = report['releases']
    assert len(rows) == 34, (len(rows), report)
    assert report['clipped'] == report['stopped'] == report['terminal'] == 0
    if env['DXMT9_QUERY_SCOPE_RANGES'] == '1':
        assert all(r['writer'] == 1 for r in rows)
        assert rows[0]['close_outcome'] == 'closed', report
        assert report['consecutive_same_prefix']['count'] == 33
        repeated = rows[1:]
        assert all(r['close_outcome'] in ('no_active_pass', 'reused') for r in repeated), report
        assert report['reused']['count'] > 0, 'candidate never reused the closed prefix'
    else:
        assert all(r['writer'] == 0 for r in rows)
        assert report['consecutive_same_prefix']['count'] == 0
    print('release-burst outcomes:', dict(collections.Counter(r['close_outcome'] for r in rows)))
    print('release-burst same-prefix:', report['consecutive_same_prefix']['count'])
