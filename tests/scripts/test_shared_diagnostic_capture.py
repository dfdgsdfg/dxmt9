"""R-HARN-TRACE-15 native consumer and immutable structured store round trip."""
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
    subprocess.run([sys.argv[1], str(parent.resolve())], check=True, timeout=30)
    capture, = parent.glob('dxmt9-tracing-runtime-*')
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
        print('Shared diagnostic reports:', db.execute('SELECT count(*) FROM observation_report').fetchone()[0], capture)


if len(sys.argv) > 2:
    parent = Path(sys.argv[2]); parent.mkdir(parents=True, exist_ok=False); verify(parent)
else:
    with tempfile.TemporaryDirectory() as directory:
        verify(Path(directory))
