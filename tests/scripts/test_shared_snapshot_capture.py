"""Query/frame/append snapshots retain exact values and semantics in native TP/SQL."""
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


def verify(parent):
    parent = parent.resolve()
    subprocess.run([sys.argv[1], str(parent)], check=True, timeout=30)
    capture, = parent.glob('dxmt9-tracing-runtime-*')
    bundle = adapt(capture / 'trace.pftrace.raw.jsonl', 'auto', run_id='run',
                   process_incarnation='unknown')
    bundle_path = capture / 'normalized.json'
    bundle_path.write_text(json.dumps(bundle))
    snapshots = [e for e in bundle['events']
                 if e['attrs'].get('observation_timestamp_kind') == 'snapshot-observed']
    groups = {}
    for event in snapshots:
        a = event['attrs']
        key = a['observation_source'], a['observation_record']
        fields = groups.setdefault(key, {})
        assert a['observation_field'] not in fields
        fields[a['observation_field']] = a
        assert a['acquisition_version'] == '1'
        if event['kind'] == 'counter':
            assert a['counter_value'] == a['observation_value']
            assert a['unit'] == a['observation_unit']
    assert len(groups) == 5, groups.keys()
    backend = groups['query-backend', '1']
    assert backend['calls']['observation_value'] == '2'
    assert backend['samples']['observation_value'] == '1'
    assert backend['invalid_clocks']['observation_value'] == '1'
    assert backend['queue_wait_elapsed_ns']['observation_value'] == '37'
    assert all(a['timing_stride'] == '64' for a in backend.values())
    assert all(a['timing_stride'] == '0' for a in groups['query-backend', '2'].values())
    receipt = groups['query-receipt', '1']
    assert receipt['capture_elapsed_ns']['observation_value'] == '53'
    assert receipt['ok_capture_elapsed_ns']['observation_value'] == '53'
    frame = groups['perf-frame', '1']
    assert frame['draw_calls']['observation_value'] == '7'
    assert frame['command_buffers']['observation_value'] == '0'
    assert frame['regressed_fields']['observation_value'] == '1'
    assert frame['render_encoder_gpu_last_pso']['observation_value'] == str(2**64-1)
    assert frame['render_encoder_gpu_last_pso']['observation_aggregation'] == 'latest'
    append = groups['compat-append', '1']
    assert append['batch_calls']['observation_value'] == '1'
    assert append['append_calls']['observation_value'] == '1'
    receipt = qualify_trace(capture / 'trace.pftrace', expected=derive_expected(bundle))
    assert receipt['verdict'] == 'qualified'
    (capture / 'consumer.json').write_text(json.dumps(receipt, indent=2))
    database = capture / 'observations.sqlite'
    store = publish_store(database, run_id='run', attempt_id='attempt',
                          provenance={'test_results': {'shared-snapshots': {'status': 'pass'}}},
                          bundles=[('engine', bundle_path)], artifacts=[])
    (capture / 'store-receipt.json').write_text(json.dumps(store, indent=2))
    with sqlite3.connect(database) as db:
        assert db.execute('PRAGMA integrity_check').fetchall() == [('ok',)]
        assert db.execute('PRAGMA foreign_key_check').fetchall() == []
        values = db.execute("SELECT value_json FROM args WHERE key='observation_value'").fetchall()
        assert len(values) == len(snapshots)
        assert str(2**64-1) in [json.loads(row[0]) for row in values]
        assert db.execute('SELECT status FROM test_result').fetchall() == [('pass',)]
    print(f'Shared snapshots qualified: {len(snapshots)} fields, {len(groups)} snapshots; {capture}')


if len(sys.argv) > 2:
    parent = Path(sys.argv[2])
    parent.mkdir(parents=True, exist_ok=False)
    verify(parent)
else:
    with tempfile.TemporaryDirectory() as directory:
        verify(Path(directory))
