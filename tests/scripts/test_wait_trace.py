import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile

spec = importlib.util.spec_from_file_location("wait_trace", sys.argv[2])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory() as directory:
    for mode in ("normal", "terminal", "superseded", "cross", "incomplete", "overflow", "unclosed"):
        path = Path(directory) / f"{mode}.csv"
        env = dict(os.environ, DXMT9_WAIT_TRACE=str(path),
                   DXMT9_WAIT_TRACE_FIRST_PRESENT="1", DXMT9_WAIT_TRACE_LAST_PRESENT="2")
        subprocess.run([sys.argv[1], mode], env=env, check=True, timeout=20)
        if mode in ("overflow", "unclosed", "incomplete"):
            try:
                module.load(path)
            except ValueError:
                pass
            else:
                raise AssertionError("unqualified trace accepted")
            continue
        meta, rows = module.load(path)
        receipt = [r for r in rows if r["waits"] and r["site"] == 0]
        assert len(receipt) == 1 and receipt[0]["query"] == 71
        assert receipt[0]["owner"] == 1 and receipt[0]["raw_before"] == 1
        assert receipt[0]["state_after"] == {"normal": 1, "cross": 1, "superseded": 2, "terminal": 3}[mode]
        summary = module.summarize(meta, rows)
        assert summary["excluded_cross_window"] == (1 if mode == "cross" else 0)
        original = path.read_bytes()
        malformed = Path(directory) / "malformed.csv"
        for bad in (original.replace(b"closed=1", b"closed=0"),
                    original.replace(b"unknown=0", b"unknown=1"),
                    original.replace(b"incomplete=0", b"incomplete=1"),
                    b"\n".join(original.splitlines()[:-1])):
            malformed.write_bytes(bad)
            try:
                module.load(malformed)
            except ValueError:
                pass
            else:
                raise AssertionError("invalid evidence accepted")
        # Existing evidence must survive a second process unchanged.
        subprocess.run([sys.argv[1], mode], env=env, check=True, timeout=20)
        assert path.read_bytes() == original
    env = dict(os.environ, DXMT9_WAIT_TRACE="0")
    subprocess.run([sys.argv[1]], env=env, check=True, timeout=20)
