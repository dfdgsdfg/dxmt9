"""R-HARN-TRACE-13: one acquisition reaches legacy, native TP and companion SQL."""
import csv
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts.tools.tracing_normalize import adapt
from scripts.tools.tracing_consumer import derive_expected, qualify_trace
from scripts.tools.tracing_store import publish_store

with tempfile.TemporaryDirectory() as directory:
    parent = Path(directory)
    subprocess.run([sys.argv[1], str(parent)], check=True, timeout=30)
    capture, = parent.glob('dxmt9-tracing-runtime-*')
    raw = capture / 'trace.pftrace.raw.jsonl'
    bundle = adapt(raw, "auto", run_id="run", process_incarnation="unknown")
    bundle_path = capture / 'normalized.json'
    bundle_path.write_text(json.dumps(bundle))
    projections = [e for e in bundle['events'] if e['attrs'].get('observation_source')]
    assert len(projections) == 4, projections
    rows = {}
    for source, name in [('cpu-supply', 'cpu-supply.csv'), ('wait', 'wait.csv')]:
        lines = (capture / name).read_text().splitlines()
        assert 'acquisition_version=1 trace_attempted=2 trace_accepted=2' in lines[0]
        rows[source] = list(csv.DictReader(line for line in lines[1:] if not line.startswith('#')))
    for event in projections:
        attrs = event['attrs']
        source = attrs['observation_source']
        row = rows[source][int(attrs['observation_record']) - 1]
        value = int(attrs['counter_value'])
        if source == 'cpu-supply':
            assert value == int(row['cpu_ns'])
            assert attrs['observation_timestamp_kind'] == 'cpu-query-start'
        else:
            expected = int(row['acquired_ns']) - int(row['begin_ns']) if (
                attrs['track_name'].startswith('wait_mutex_acquire_')) else (
                    int(row['end_ns']) - int(row['acquired_ns']))
            assert value == expected
            assert attrs['observation_timestamp_kind'] == 'wait-result-observed'
    native = qualify_trace(capture / 'trace.pftrace', expected=derive_expected(bundle))
    assert native['verdict'] == 'qualified'
    assert len(native['queries']['observation_counter_rows']) == 16
    database = capture / 'observations.sqlite'
    publish_store(database, run_id='run', attempt_id='attempt',
                  provenance={'test_results': {'shared-observation': {'status': 'pass'}}},
                  bundles=[('engine', bundle_path), ('cpu-supply', capture / 'cpu-supply.csv'),
                           ('wait', capture / 'wait.csv')], artifacts=[])
    with sqlite3.connect(database) as db:
        assert db.execute('PRAGMA integrity_check').fetchall() == [('ok',)]
        assert db.execute('PRAGMA foreign_key_check').fetchall() == []
        envelopes = [json.loads(r[0]) for r in db.execute('SELECT envelope_json FROM bundle')]
        shared = [e['acquisition'] for e in envelopes if 'acquisition' in e]
        assert len(shared) == 2 and all(a['trace_accepted'] == '2' for a in shared)
        assert db.execute('SELECT status FROM test_result').fetchall() == [('pass',)]
print('Shared acquisition values, native counter annotations and SQL provenance match')
