import csv
import os
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)/'trace.csv'
    env = dict(os.environ, DXMT9_CPU_SUPPLY_TRACE=str(path),
               DXMT9_CPU_SUPPLY_TRACE_FIRST_PRESENT='1',
               DXMT9_CPU_SUPPLY_TRACE_LAST_PRESENT='2')
    subprocess.run([sys.argv[1]], env=env, check=True, timeout=20)
    with path.open() as stream:
        header = stream.readline()
        assert 'closed=1 overflow=0 incomplete=0' in header, header
        rows = [{k: int(v) for k, v in r.items()} for r in csv.DictReader(stream)]
    kinds = [r['event'] for r in rows]
    assert 9 in kinds and 10 in kinds and 8 in kinds, kinds  # reserved/ready/empty
    begin = [r for r in rows if r['event']==6]
    assert len(begin)==1 and begin[0]['id']==2
    assert all(r['wall_ns']>0 and r['cpu_ns']>0 and r['tid']>0 for r in rows)
    a=next(r for r in rows if r['event']==1)
    b=next(r for r in rows if r['event']==2)
    wall, cpu=b['wall_ns']-a['wall_ns'], b['cpu_ns']-a['cpu_ns']
    # A mislabeled hardware-tick thread clock used to read about 1/41.67 of
    # real CPU under Rosetta. Do not accept positive/monotone alone.
    assert cpu > 20_000_000 and .5 < cpu/wall < 1.1, (wall,cpu)
    # Existing evidence cannot be overwritten by a second process.
    original = path.read_bytes()
    subprocess.run([sys.argv[1]], env=env, check=True, timeout=20)
    assert path.read_bytes()==original
    env['DXMT9_CPU_SUPPLY_TRACE']=''
    subprocess.run([sys.argv[1]], env=env, check=True, timeout=20)
