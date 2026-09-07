#!/usr/bin/env python3
"""Live same-user peer access + CPU units, without task ports or suspension."""

import json
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile


helper = Path(sys.argv[1]).resolve()
spec = importlib.util.spec_from_file_location('observer', Path(__file__).resolve().parents[2] /
                                            'scripts/tools/observe_darwin_thread_cpu.py')
observer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(observer)
for args in (['--self-check', '--pid', '42', '--tid', '7'],
             ['--pid', '42', '--tid', '7', '--self-check'],
             ['--pid', '-1', '--tid', '7'], ['--pid', '42', '--tid', ' -1'],
             ['--pid', '+42', '--tid', '7'], ['--pid', '42', '--tid', '+7']):
    invalid = subprocess.run([str(helper), *args], capture_output=True, text=True, timeout=3)
    if invalid.returncode != 2 or json.loads(invalid.stdout)['error']['kind'] != 'usage':
        raise RuntimeError(f'Invalid CLI accepted: {args} {invalid.stdout}')
program = r'''
import json, os, sys, threading, time
print(json.dumps(dict(pid=os.getpid(), tid=threading.get_native_id())), flush=True)
assert input() == 'busy'
before = time.thread_time_ns()
value = 1
while time.thread_time_ns() - before < 200_000_000:
    for i in range(100):
        value = (value * 17 + i) & 0xffffffff
after = time.thread_time_ns()
print(json.dumps(dict(cpu_ns=after-before, value=value)), flush=True)
assert input() == 'exit'
'''


def query(pid, tid, expected_code=0):
    result = subprocess.run([str(helper), '--pid', str(pid), '--tid', str(tid)],
                            capture_output=True, text=True, timeout=3)
    if (result.returncode == 0) != (expected_code == 0):
        raise RuntimeError(f'Unexpected helper exit {result.returncode}: {result.stdout} {result.stderr}')
    row = json.loads(result.stdout)
    if row['status'] != ('ok' if expected_code == 0 else 'error'):
        raise RuntimeError(row)
    return row


with subprocess.Popen([sys.executable, '-u', '-c', program], stdin=subprocess.PIPE,
                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as child:
    try:
        target = json.loads(child.stdout.readline())
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / 'dxmt9.log'
            marker = f"unix_commit_chunk_entry device=0x123 native_tid={hex(target['tid'])}\n"
            log.write_text('command chunk negotiation selected canonical transport=0x1\n' + marker)
            collector = observer.Observer(helper, log, target['pid'])
            collected = collector.sample()
            if collected['status'] != 'sample' or collected['selector']['native_tid'] != target['tid']:
                raise RuntimeError(collected)
            log.write_text('command chunk negotiation selected canonical transport=0x2\n' + marker)
            try:
                observer.Observer(helper, log, target['pid']).sample()
            except ValueError:
                pass
            else:
                raise RuntimeError('unsupported transport accepted')
            log.write_text('command chunk negotiation selected canonical transport=0x1\n' + marker)
            os.utime(log, ns=(1_000_000_000, 1_000_000_000))
            try:
                observer.Observer(helper, log, target['pid']).sample()
            except ValueError:
                pass
            else:
                raise RuntimeError('stale log accepted')
        before = query(target['pid'], target['tid'])
        child.stdin.write('busy\n')
        child.stdin.flush()
        measured = json.loads(child.stdout.readline())
        after = query(target['pid'], target['tid'])
        keys = ('pid', 'native_tid', 'start_sec', 'start_usec')
        if any(before[key] != after[key] for key in keys):
            raise RuntimeError('peer identity changed')
        cpu_ns = after['user_ns'] + after['system_ns'] - before['user_ns'] - before['system_ns']
        if abs(cpu_ns - measured['cpu_ns']) > 20_000_000:
            raise RuntimeError(f'peer CPU units mismatch: {cpu_ns} vs {measured}')
        missing = query(target['pid'], 2**64-1, expected_code=1)
        child.stdin.write('exit\n')
        child.stdin.flush()
        child.wait(timeout=3)
        if child.returncode != 0:
            raise RuntimeError(child.stderr.read())
        exited = query(target['pid'], target['tid'], expected_code=1)
        print(json.dumps(dict(status='pass', target=target, libproc_delta_ns=cpu_ns,
            peer_thread_cpu_ns=measured['cpu_ns'], missing_thread=missing,
            exited_process=exited)))
    finally:
        if child.poll() is None:
            child.terminate()
            child.wait(timeout=3)
