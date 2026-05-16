import tempfile
import unittest
from pathlib import Path
import sys


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))
sys.path.insert(0, str(REPO_ROOT / "tests" / "module_boundary"))

from scripts.check import check_debug_result_schema as schema  # noqa: E402
import run_module_boundary  # noqa: E402


class ModuleBoundaryDebugResultTests(unittest.TestCase):
    def test_scaffold_result_can_emit_debug_schema(self):
        result = {
            "schema": run_module_boundary.RESULT_SCHEMA,
            "case": "result-schema",
            "lane": "provider-side",
            "arch": "native",
            "cost_class": "compile-time-test-only",
            "command": ["run_module_boundary.py", "emit-scaffold-result"],
            "environment": {},
            "artifacts": [
                {
                    "role": "provider-probe",
                    "path": "tests/native/backend/dxmt9-unix-chunk-injection-probe",
                    "sha256": "abc",
                    "bytes": 128,
                }
            ],
            "exit_code": 0,
            "failure_category": "unsupported-runtime",
            "checks": [
                {
                    "name": "runtime_execution",
                    "status": "skip",
                    "summary": "scaffold result only",
                }
            ],
            "log_excerpt": ["scaffold"],
        }

        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "debug_result.json"
            run_module_boundary.write_debug_result(result, output)
            data = schema.load_json(output)

        self.assertEqual(schema.validate_result(data), [])
        self.assertEqual(data["module"], "module-boundary")
        self.assertEqual(data["failure_category"], "headless-unsupported")
        self.assertEqual(data["diagnostics"]["module_boundary"]["case"], "result-schema")
        self.assertTrue(any(artifact["path"] == "artifacts/module_boundary_stage_manifest.json" for artifact in data["artifacts"]))
        self.assertTrue(any(sidecar["kind"] == "provider-locator" for sidecar in data["diagnostics"]["module_boundary"]["sidecars"]))

    def test_bridge_mismatch_maps_to_wine_abi_category(self):
        result = {
            "schema": run_module_boundary.RESULT_SCHEMA,
            "case": "app-local-loader-smoke",
            "lane": "app-local",
            "arch": "x64",
            "cost_class": "opt-in-cold-diagnostic",
            "command": ["run_module_boundary.py", "run"],
            "executed_command": ["wine64", "./dxmt9-module-boundary-probe_x64.exe"],
            "environment": {"WINEDLLOVERRIDES": "d3d9,winemetal=n,b"},
            "artifacts": [],
            "exit_code": 1,
            "failure_category": "bridge-abi-mismatch",
            "checks": [],
            "log_excerpt": [],
            "wine": "/usr/local/bin/wine64",
        }

        data = run_module_boundary.build_debug_result(result)
        self.assertEqual(schema.validate_result(data), [])
        self.assertEqual(data["failure_category"], "wine-abi-handshake")
        self.assertEqual(data["diagnostics"]["wine"]["abi_status"], "mismatch")

    def test_live_log_sidecars_are_attached_to_debug_result(self):
        result = {
            "schema": run_module_boundary.RESULT_SCHEMA,
            "case": "app-local-loader-smoke",
            "lane": "app-local",
            "arch": "x64",
            "cost_class": "opt-in-cold-diagnostic",
            "command": ["run_module_boundary.py", "run"],
            "environment": {
                "DXMT9_MODULE_BOUNDARY_LOAD_MODE": "app-local",
                "DXMT9_WINEMETAL_SO": "/tmp/winemetal.so",
            },
            "artifacts": [],
            "exit_code": 1,
            "failure_category": "bridge-abi-mismatch",
            "checks": [],
            "stdout": "winemetal.dll/winemetal.so ABI hash mismatch",
            "stderr": "",
            "log_excerpt": [],
        }

        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "debug_result.json"
            run_module_boundary.write_debug_result(result, output)
            data = schema.load_json(output)
            self.assertTrue((Path(tmp) / "logs" / "provider_locator.json").is_file())
            self.assertTrue((Path(tmp) / "logs" / "abi_handshake.json").is_file())

        self.assertEqual(schema.validate_result(data), [])
        self.assertEqual(data["failure_category"], "wine-abi-handshake")
        self.assertTrue(any(artifact["path"] == "logs/abi_handshake.json" for artifact in data["artifacts"]))


if __name__ == "__main__":
    unittest.main()
