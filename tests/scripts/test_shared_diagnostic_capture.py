"""R-HARN-TRACE-15 native consumer and immutable structured store round trip."""
import json
import os
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
from scripts.run_apps.tracing_harness import verify_engine_receipt


def verify(parent, mode='deferred', collectors='dual'):
    env = dict(os.environ, DXMT9_TEST_DRAIN_MODE=mode, DXMT9_TEST_COLLECTOR_MODE=collectors)
    subprocess.run([sys.argv[1], str(parent.resolve())], check=True, timeout=30, env=env)
    capture, = parent.glob('dxmt9-tracing-runtime-*')
    capture = capture.resolve()
    qualification = verify_engine_receipt(capture / 'receipt.json',
        json.loads((capture / 'request.json').read_text()), capture / 'trace.pftrace')
    assert qualification['qualified'], qualification
    bundle = adapt(capture / 'trace.pftrace.raw.jsonl', 'auto', run_id='run', process_incarnation='unknown')
    receipt = qualify_trace(capture / 'trace.pftrace', expected=derive_expected(bundle))
    (capture / 'consumer.json').write_text(json.dumps(receipt, indent=2))
    assert receipt['verdict'] == 'qualified', receipt
    normalized = capture / 'normalized.json'
    normalized.write_text(json.dumps(bundle))
    database = capture / 'observations.sqlite'
    store = publish_store(database, run_id='run', attempt_id='attempt',
                          provenance={'test_results': {'shared-diagnostics': {'status': 'pass'}}},
                          bundles=[('engine', normalized)], artifacts=[])
    (capture / 'store-receipt.json').write_text(json.dumps(store, indent=2))
    with sqlite3.connect(database) as db:
        assert db.execute('PRAGMA integrity_check').fetchall() == [('ok',)]
        assert db.execute("SELECT count(*) FROM observation_report WHERE source='perf-report' AND encoding=2").fetchone()[0] > 2300
        assert db.execute("SELECT count(*) FROM observation_report WHERE source='process-memory' AND schema='sample_status'").fetchone()[0] == 1
        assert db.execute("SELECT count(*) FROM observation_field WHERE key='omitted_samples' AND value_exact='3'").fetchone()[0] >= 1
        assert db.execute('PRAGMA foreign_key_check').fetchall() == []
        sources = {r[0] for r in db.execute('SELECT DISTINCT source FROM observation_report')}
        assert {'query-report','perf-report','process-memory','cpu-detail','wait-detail',
                'pe-report','queue-report','signpost','renderer'} <= sources, sources
        assert db.execute("SELECT value_exact FROM observation_report WHERE schema='exact-counter-fixture'").fetchone()[0] == str(2**64-1)
        assert db.execute("SELECT payload FROM observation_report WHERE schema='unicode-fragment-fixture'").fetchone()[0] == 'x'*190+'한글'
        assert db.execute("SELECT value_json FROM structural_value WHERE path='/passes/0/id'").fetchone()[0] == str(2**64-1)
        assert db.execute("SELECT count(*) FROM observation_report WHERE status!='complete'").fetchone()[0] == 0
        assert db.execute('SELECT status FROM test_result').fetchall() == [('pass',)]
        assert db.execute("SELECT COUNT(*) FROM observation_report WHERE schema='pe-pc-sample-v1' AND encoding=4 AND status='complete'").fetchone()[0] == 1
        assert db.execute("SELECT COUNT(*) FROM observation_field WHERE key='qpc_frequency'").fetchone()[0] == 1
        assert db.execute("SELECT COUNT(*) FROM observation_report WHERE schema='cpu-supply-row-v2' AND encoding=4").fetchone()[0] > 0
        assert db.execute("SELECT COUNT(*) FROM observation_report WHERE schema='wait-row-v2' AND encoding=4").fetchone()[0] > 0
        print('Shared diagnostic reports:', db.execute('SELECT count(*) FROM observation_report').fetchone()[0], capture)
    engine_receipt = json.loads((capture / 'receipt.json').read_text())
    assert engine_receipt['transport']['mode'] == mode
    if mode == 'async':
        from scripts.tools.tracing_store import digest
        spool = engine_receipt['raw_spool_artifact']
        assert digest(Path(spool['path'])) == spool['sha256']
        assert Path(spool['path']).stat().st_size == int(spool['bytes'])
    if collectors in ('perfetto', 'unwritable'):
        assert not (capture / 'cpu-supply.csv').exists()
        assert not (capture / 'wait.csv').exists()


if len(sys.argv) > 2:
    parent = Path(sys.argv[2]); parent.mkdir(parents=True, exist_ok=False)
    verify(parent, os.environ.get('DXMT9_TEST_DRAIN_MODE', 'deferred'),
           os.environ.get('DXMT9_TEST_COLLECTOR_MODE', 'dual'))
else:
    with tempfile.TemporaryDirectory() as directory:
        verify(Path(directory), os.environ.get('DXMT9_TEST_DRAIN_MODE', 'deferred'),
               os.environ.get('DXMT9_TEST_COLLECTOR_MODE', 'dual'))
