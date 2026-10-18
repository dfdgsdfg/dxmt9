"""Deterministic runner-side tracing contract tests."""
from __future__ import annotations

import json
import subprocess
import sqlite3
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.run_apps.tracing_harness import (  # noqa: E402
    TRACE_PROFILES,
    apply_trace_launch_configuration,
    atomic_write_json,
    artifact_graph,
    attempt_paths,
    canonical_digest,
    make_trace_request,
    parent_result_binding,
    seal_trace_manifest,
    sha256_file,
    source_identity,
    trace_launch_configuration,
    verify_engine_receipt,
)
from scripts.run_apps.run_experiment import (  # noqa: E402
    ExperimentApp,
    resolve_trace_runtime,
)


class TraceHarnessTests(unittest.TestCase):
    def test_profile_configuration_is_frozen_and_bounded(self):
        profile = TRACE_PROFILES["timeline"]
        config = profile.frozen_config()
        self.assertEqual(config["required_collectors"], ["engine-perfetto"])
        self.assertEqual(config["qualification_policy"], {
            "max_clock_error_ns": 1000,
            "require_zero_loss": True,
            "require_closed_window": True,
            "exact_native_consumer": True,
        })
        self.assertEqual(config["categories"], [
            "import", "replay", "queue", "encode", "wait", "submit", "completion"
        ])
        self.assertGreater(config["max_events"], 0)
        self.assertGreater(config["max_bytes"], 0)
        with self.assertRaises(Exception):
            profile.name = "changed"  # type: ignore[misc]

    def test_default_overview_requests_optional_gpu_without_pe_or_detail(self):
        paths = attempt_paths(Path('/tmp/overview-fixture'), 'run')
        request = make_trace_request(TRACE_PROFILES['overview'], run_id='run', attempt_id='attempt', paths=paths)
        self.assertIn('gpu', request['categories'])
        self.assertNotIn('gpu', request['required_categories'])
        self.assertFalse(request['gpu_timing']['required'])
        self.assertNotIn('pe', request['categories'])
        self.assertNotIn('detail', request['categories'])
        self.assertEqual(request['transport'], {
            'mode': 'deferred', 'batch_events': 64,
            'service_interval_ms': 10, 'transport_events': 16_384,
        })

    def test_async_transport_is_frozen_in_request_and_validated(self):
        paths = attempt_paths(Path('/tmp/async-fixture'), 'run')
        request = make_trace_request(
            TRACE_PROFILES['timeline'], run_id='run', attempt_id='attempt', paths=paths,
            drain_mode='async', batch_events=32, service_interval_ms=7,
            transport_events=2048,
        )
        frozen = {key: value for key, value in request.items() if key != 'config_sha256'}
        self.assertEqual(request['config_sha256'], canonical_digest(frozen))
        self.assertEqual(request['transport'], {
            'mode': 'async', 'batch_events': 32,
            'service_interval_ms': 7, 'transport_events': 2048,
        })
        with self.assertRaises(ValueError):
            make_trace_request(
                TRACE_PROFILES['timeline'], run_id='run', attempt_id='attempt', paths=paths,
                drain_mode='streaming',
            )
        with self.assertRaises(ValueError):
            make_trace_request(
                TRACE_PROFILES['timeline'], run_id='run', attempt_id='attempt', paths=paths,
                batch_events=4097,
            )
        with self.assertRaises(ValueError):
            make_trace_request(
                TRACE_PROFILES['timeline'], run_id='run', attempt_id='attempt', paths=paths,
                batch_events=64, transport_events=255,
            )

    def test_trace_only_collectors_do_not_require_legacy_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            paths = attempt_paths(Path(temporary), 'trace-only')
            paths['root'].mkdir(parents=True)
            environment = {'DXMT9_CPU_SUPPLY_TRACE': 'perfetto',
                           'DXMT9_WAIT_TRACE': 'perfetto'}
            manifest = {'run_id': 'trace-only', 'attempt_id': 'attempt',
                        'runtime': {'env_snapshot': environment}}
            sealed = seal_trace_manifest(paths, manifest)
            self.assertEqual(sealed['observations']['status'], 'published')
            self.assertEqual(sealed['observations']['normalization_errors'], [])
            with sqlite3.connect(paths['observations']) as db:
                runtime = json.loads(db.execute(
                    "SELECT value_json FROM provenance WHERE key='runtime'").fetchone()[0])
                self.assertEqual(runtime['env_snapshot'], environment)

    def test_malformed_supplement_keeps_failed_queryable_attempt(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            broken = root/'broken.csv'
            broken.write_text('not a trace')
            paths = attempt_paths(root,'run-failed')
            paths['root'].mkdir(parents=True)
            manifest = {'run_id':'run-failed','attempt_id':'attempt',
                        'validity':{'status':'valid'},
                        'workload_outcome':{'status':'pass'},
                        'supplemental_bundles':[{'role':'cpu','path':str(broken)}]}
            sealed = seal_trace_manifest(paths,manifest)
            self.assertEqual(sealed['observations']['status'],'partial')
            self.assertEqual(manifest['validity']['status'],'invalid')
            with sqlite3.connect(paths['observations']) as db:
                status = json.loads(db.execute("SELECT value_json FROM provenance WHERE key='workload_outcome'").fetchone()[0])
                self.assertEqual(status['status'],'pass')
                errors = json.loads(db.execute("SELECT value_json FROM provenance WHERE key='normalization_errors'").fetchone()[0])
                self.assertEqual(errors[0]['role'],'cpu')

    def test_attempt_paths_are_run_scoped(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = attempt_paths(root, "run-a")
            second = attempt_paths(root, "run-b")
            self.assertNotEqual(first["manifest"], second["manifest"])
            self.assertEqual(first["root"], root / "tracing" / "run-a")

    def test_sealed_manifest_and_parent_result_binding_are_immutable(self):
        with tempfile.TemporaryDirectory() as temporary:
            paths = attempt_paths(Path(temporary), "run-sealed")
            paths["root"].mkdir(parents=True)
            paths["request"].write_text('{"config":"frozen"}\n')
            manifest = {"schema": "dxmt9-trace-manifest-v1", "validity": {"status": "valid"}}
            sealed = seal_trace_manifest(paths, manifest)
            before = sha256_file(paths["manifest"])
            self.assertTrue(json.loads(paths["manifest"].read_text())["sealed"])
            self.assertEqual(sealed["artifact_graph_sha256"], manifest["artifact_graph_sha256"])
            self.assertEqual([item["role"] for item in artifact_graph(paths)], ["observations-sqlite", "request"])
            with self.assertRaises(FileExistsError):
                seal_trace_manifest(paths, {"validity": {"status": "invalid"}})
            self.assertEqual(sha256_file(paths["manifest"]), before)
            binding = parent_result_binding(
                run_id="run-sealed", attempt_id="attempt-sealed", workload_status="fail",
                trace_qualified=False, sealed=sealed,
            )
            self.assertEqual(binding["parent_result_envelope"]["manifest_sha256"], before)
            self.assertEqual(len(binding["parent_result_envelope_sha256"]), 64)

    def test_dry_run_runtime_resolution_reports_paths_without_staging(self):
        app = ExperimentApp(
            name="fixture", source="local", license="MIT", source_kind="local",
            license_scope="test", binary="app.exe", launcher="run.sh", reference="ref.png",
            features=[], status="ready", requires_wine=False,
        )
        args = type("Args", (), {
            "wine_id": None, "wine_root": None, "wine_bin": None, "wine_manifest": None,
            "pe_build_dir": "missing-pe", "runtime_pe_build_dir": None,
            "unix_build_dir": None, "wow64_pe_build_dir": None,
            "wow64_runtime_pe_build_dir": None, "binary": None, "skip_stage": False,
            "mingw_bin_dir": None, "wow64_mingw_bin_dir": None,
            "stage_mingw_runtime": False, "prefix": None,
        })()
        with mock.patch("scripts.run_apps.run_experiment.detect_heroic_wine_root", return_value=None):
            resolved = resolve_trace_runtime(app, args)
        self.assertIsNone(resolved["wine_root"])
        self.assertIsNone(resolved["wine_bin"])
        self.assertEqual(resolved["build_dirs"]["pe"], str(Path("missing-pe").resolve()))
        self.assertEqual(resolved["binary"], str(app.binary_path))
        self.assertFalse(resolved["skip_stage"])

    def test_gpu_and_detail_requests_are_explicit_and_bounded(self):
        with tempfile.TemporaryDirectory() as temporary:
            paths = attempt_paths(Path(temporary), "run-gpu")
            gpu = make_trace_request(
                TRACE_PROFILES["gpu-detail"], run_id="run-gpu", attempt_id="attempt-gpu",
                paths=paths, duration_ms=1_000,
            )
            self.assertEqual(gpu["categories"][-1], "gpu")
            self.assertEqual(gpu["gpu_timing"], {
                "level": "command-buffer", "capacity": 256,
                "required": True, "calibration": True,
            })
            self.assertEqual(gpu["window"]["duration_ms"], 1_000)
            self.assertEqual(gpu["qualification_policy"], {
                "max_clock_error_ns": 1000,
                "require_zero_loss": True,
                "require_closed_window": True,
                "exact_native_consumer": True,
            })
            timeline_metal = make_trace_request(
                TRACE_PROFILES["timeline"], run_id="run-gpu", attempt_id="attempt-metal",
                paths=paths, duration_ms=1_000, include_metal=True,
            )
            self.assertEqual(timeline_metal["required_collectors"], ["engine-perfetto"])
            self.assertEqual(timeline_metal["optional_collectors"], ["metal-timing"])
            self.assertFalse(timeline_metal["gpu_timing"]["required"])
            detailed = make_trace_request(
                TRACE_PROFILES["timeline"], run_id="run-gpu", attempt_id="attempt-detail",
                paths=paths, duration_ms=1_000, include_detail=True,
            )
            self.assertEqual(detailed["categories"][-1], "detail")
            self.assertIn("detail", detailed["required_categories"])
            with self.assertRaises(ValueError):
                make_trace_request(
                    TRACE_PROFILES["gpu-detail"], run_id="run-gpu", attempt_id="bad-capacity",
                    paths=paths, duration_ms=1_000, gpu_capacity=257,
                )

    def test_pe_trace_environment_is_explicitly_enabled_or_suppressed(self):
        with tempfile.TemporaryDirectory() as temporary:
            paths = attempt_paths(Path(temporary), "run-pe")
            profile = TRACE_PROFILES["timeline"]
            enabled_request = make_trace_request(
                profile, run_id="run-pe", attempt_id="attempt-pe", paths=paths,
                duration_ms=1_000, include_pe=True,
            )
            enabled_config = trace_launch_configuration(
                profile, enabled_request, request_path=paths["request"],
                receipt_path=paths["receipt"], requested_pe=True,
            )
            environment = {
                "DXMT9_TRACE_PE": "0",
                "DXMT9_TRACE_CONFIG": "inherited-request.json",
                "DXMT9_TRACE_RECEIPT": "inherited-receipt.json",
            }
            apply_trace_launch_configuration(environment, enabled_config)
            self.assertEqual(enabled_config["pe_events"]["requested"], True)
            self.assertEqual(environment["DXMT9_TRACE_PE"], "1")
            self.assertEqual(environment["DXMT9_TRACE_CONFIG"], str(paths["request"]))
            self.assertEqual(environment["DXMT9_TRACE_RECEIPT"], str(paths["receipt"]))
            disabled_request = make_trace_request(
                profile, run_id="run-pe", attempt_id="attempt-no-pe", paths=paths,
                duration_ms=1_000,
            )
            inherited_opt_in = {
                "DXMT9_TRACE_PE": "1",
                "DXMT9_TRACE_CONFIG": "inherited-request.json",
                "DXMT9_TRACE_RECEIPT": "inherited-receipt.json",
            }
            disabled_config = trace_launch_configuration(
                profile, disabled_request, request_path=paths["request"],
                receipt_path=paths["receipt"],
            )
            apply_trace_launch_configuration(inherited_opt_in, disabled_config)
            self.assertEqual(inherited_opt_in["DXMT9_TRACE_PE"], "0")
            self.assertEqual(inherited_opt_in["DXMT9_TRACE_CONFIG"], str(paths["request"]))
            self.assertEqual(inherited_opt_in["DXMT9_TRACE_RECEIPT"], str(paths["receipt"]))

            baseline = {"DXMT9_TRACE_PE": "1",
                        "DXMT9_TRACE_CONFIG": "inherited-request.json",
                        "DXMT9_TRACE_RECEIPT": "inherited-receipt.json"}
            baseline_config = trace_launch_configuration(TRACE_PROFILES["baseline"], None)
            apply_trace_launch_configuration(baseline, baseline_config)
            self.assertEqual(baseline["DXMT9_TRACE_PE"], "0")
            self.assertNotIn("DXMT9_TRACE_CONFIG", baseline)
            self.assertNotIn("DXMT9_TRACE_RECEIPT", baseline)

            rejected_window = trace_launch_configuration(profile, None, requested_pe=True)
            self.assertTrue(rejected_window["pe_events"]["requested"])
            self.assertFalse(rejected_window["pe_events"]["enabled"])
            self.assertEqual(rejected_window["suppressed_environment"], [
                "DXMT9_TRACE_CONFIG", "DXMT9_TRACE_RECEIPT",
            ])
            refused = {"DXMT9_TRACE_CONFIG": "inherited-config.json",
                       "DXMT9_TRACE_RECEIPT": "inherited-receipt.json"}
            apply_trace_launch_configuration(refused, rejected_window)
            self.assertNotIn("DXMT9_TRACE_CONFIG", refused)
            self.assertNotIn("DXMT9_TRACE_RECEIPT", refused)

            ordinary = {"DXMT9_TRACE_PE": "1",
                        "DXMT9_TRACE_CONFIG": "ordinary-config.json",
                        "DXMT9_TRACE_RECEIPT": "ordinary-receipt.json"}
            apply_trace_launch_configuration(ordinary, trace_launch_configuration(None, None))
            self.assertEqual(ordinary["DXMT9_TRACE_CONFIG"], "ordinary-config.json")
            self.assertEqual(ordinary["DXMT9_TRACE_RECEIPT"], "ordinary-receipt.json")

    def test_source_identity_binds_changed_contents_and_untracked_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            tracked = root / "tracked.txt"
            tracked.write_text("first\n")
            subprocess.run(["git", "add", "tracked.txt"], cwd=root, check=True)
            subprocess.run(["git", "-c", "user.name=Harness", "-c",
                            "user.email=harness@example.invalid", "commit", "-qm", "init"],
                           cwd=root, check=True)
            tracked.write_text("modified one\n")
            first = source_identity(root)
            tracked.write_text("modified two\n")
            second = source_identity(root)
            self.assertTrue(first["dirty"])
            self.assertNotEqual(first["tracked_diff_sha256"], second["tracked_diff_sha256"])

            untracked = root / "new.txt"
            untracked.write_text("untracked one\n")
            first = source_identity(root)
            untracked.write_text("untracked two\n")
            second = source_identity(root)
            self.assertNotEqual(first["untracked_content_sha256"],
                                second["untracked_content_sha256"])

    def test_atomic_publish_refuses_collision_unless_runner_replaces(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "manifest.json"
            atomic_write_json(path, {"state": "initial"})
            with self.assertRaises(FileExistsError):
                atomic_write_json(path, {"state": "stale-overwrite"})
            atomic_write_json(path, {"state": "owned-update"}, replace_existing=True)
            self.assertEqual(json.loads(path.read_text()), {"state": "owned-update"})
            if hasattr(Path, "symlink_to"):
                link = Path(temporary) / "link"
                target = Path(temporary) / "target"
                target.mkdir()
                link.symlink_to(target, target_is_directory=True)
                with self.assertRaises(OSError):
                    atomic_write_json(link / "manifest.json", {"state": "bad"})

    def test_source_identity_failure_does_not_claim_clean(self):
        with tempfile.TemporaryDirectory() as temporary:
            with mock.patch("scripts.run_apps.tracing_harness.subprocess.run",
                            side_effect=OSError("git unavailable")):
                result = source_identity(Path(temporary))
            self.assertFalse(result["available"])
            self.assertIsNone(result["dirty"])

    def test_source_identity_marks_unreadable_untracked_content_incomplete(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            (root / "tracked.txt").write_text("tracked\n")
            subprocess.run(["git", "add", "tracked.txt"], cwd=root, check=True)
            subprocess.run(["git", "-c", "user.name=Harness", "-c",
                            "user.email=harness@example.invalid", "commit", "-qm", "init"],
                           cwd=root, check=True)
            (root / "untracked-link").symlink_to(root / "tracked.txt")
            result = source_identity(root)
            self.assertTrue(result["available"])
            self.assertFalse(result["content_identity_complete"])

    def test_receipt_rejects_missing_loss_and_malformed_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            paths = attempt_paths(Path(temporary), "run-3")
            paths["root"].mkdir(parents=True)
            paths["pftrace"].write_bytes(b"trace")
            raw_path = Path(str(paths["pftrace"]) + ".raw.jsonl")
            raw_path.write_text('{"schema":"dxmt9.trace.raw.v1"}\n')
            request = make_trace_request(
                TRACE_PROFILES["timeline"], run_id="run-3", attempt_id="attempt-3", paths=paths
            )
            receipt = {
                "schema": "dxmt9.trace.receipt.v1", "run_id": "run-3",
                "attempt_id": "attempt-3", "profile_id": "timeline",
                "config_sha256": request["config_sha256"], "status": "complete",
                "window": {"closed": True},
                "categories_resolved": request["categories"],
                "applicability": {name: {"status": "observed", "count": 0}
                                  for name in request["required_categories"]},
                "counts": {"engine": True}, "loss": {"engine": None},
                "flush": {"status": "complete"},
                "artifact": {"path": str(paths["pftrace"]), "bytes": 5,
                             "sha256": sha256_file(paths["pftrace"])},
                "raw_artifact": {"path": str(raw_path), "bytes": raw_path.stat().st_size,
                                 "sha256": sha256_file(raw_path)},
            }
            paths["receipt"].write_text(json.dumps(receipt))
            verified = verify_engine_receipt(paths["receipt"], request, paths["pftrace"])
            self.assertFalse(verified["qualified"])
            self.assertIn("source_counts_malformed", verified["errors"])
            self.assertIn("event_loss_or_missing_loss_table", verified["errors"])

            paths["receipt"].write_text(json.dumps(["not", "an", "object"]))
            malformed = verify_engine_receipt(paths["receipt"], request, paths["pftrace"])
            self.assertFalse(malformed["qualified"])
            self.assertIn("receipt_root_not_object", malformed["errors"])

    def test_receipt_rejects_path_escape_and_symlinked_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            paths = attempt_paths(Path(temporary), "run-4")
            paths["root"].mkdir(parents=True)
            paths["pftrace"].write_bytes(b"trace")
            raw_path = Path(str(paths["pftrace"]) + ".raw.jsonl")
            raw_path.write_text('{"schema":"dxmt9.trace.raw.v1"}\n')
            request = make_trace_request(
                TRACE_PROFILES["timeline"], run_id="run-4", attempt_id="attempt-4", paths=paths
            )
            receipt = {
                "schema": "dxmt9.trace.receipt.v1", "run_id": "run-4",
                "attempt_id": "attempt-4", "profile_id": "timeline",
                "config_sha256": request["config_sha256"], "status": "complete",
                "window": {"closed": True},
                "categories_resolved": request["categories"],
                "applicability": {name: {"status": "observed", "count": 0}
                                  for name in request["required_categories"]},
                "counts": {"engine": "0"}, "loss": {"engine": "0"},
                "flush": {"status": "complete"},
                "artifact": {"path": str(paths["pftrace"]), "bytes": 5,
                             "sha256": sha256_file(paths["pftrace"])},
                "raw_artifact": {"path": str(raw_path), "bytes": raw_path.stat().st_size,
                                 "sha256": sha256_file(raw_path)},
            }
            receipt["artifact"]["path"] = str(paths["root"].parent / "escape.pftrace")
            paths["receipt"].write_text(json.dumps(receipt))
            escaped = verify_engine_receipt(paths["receipt"], request, paths["pftrace"])
            self.assertFalse(escaped["qualified"])
            self.assertIn("artifact_path_mismatch", escaped["errors"])

            link = paths["root"] / "linked.pftrace"
            link.symlink_to(paths["pftrace"])
            unsafe = verify_engine_receipt(paths["receipt"], request, link)
            self.assertFalse(unsafe["qualified"])
            self.assertIn("artifact_path_mismatch", unsafe["errors"])

    def test_receipt_requires_matching_config_and_exact_artifact_digest(self):
        with tempfile.TemporaryDirectory() as temporary:
            paths = attempt_paths(Path(temporary), "run-1")
            paths["root"].mkdir(parents=True)
            paths["pftrace"].write_bytes(b"valid trace bytes")
            raw_path = Path(str(paths["pftrace"]) + ".raw.jsonl")
            raw_path.write_text('{"schema":"dxmt9.trace.raw.v1"}\n')
            request = make_trace_request(
                TRACE_PROFILES["timeline"], run_id="run-1", attempt_id="attempt-1", paths=paths
            )
            receipt = {
                "schema": "dxmt9.trace.receipt.v1",
                "run_id": "run-1",
                "attempt_id": "attempt-1",
                "profile_id": "timeline",
                "config_sha256": request["config_sha256"],
                "status": "complete",
                "window": {"closed": True},
                "categories_resolved": request["categories"],
                "applicability": {name: {"status": "observed", "count": 0}
                                  for name in request["required_categories"]},
                "counts": {"engine": 12},
                "loss": {"engine": 0},
                "flush": {"status": "complete"},
                "artifact": {"path": str(paths["pftrace"]),
                             "bytes": paths["pftrace"].stat().st_size,
                             "sha256": sha256_file(paths["pftrace"])},
                "raw_artifact": {"path": str(raw_path), "bytes": raw_path.stat().st_size,
                                 "sha256": sha256_file(raw_path)},
            }
            paths["receipt"].write_text(json.dumps(receipt))
            verified = verify_engine_receipt(paths["receipt"], request, paths["pftrace"])
            self.assertTrue(verified["qualified"], verified["errors"])

            receipt["config_sha256"] = "0" * 64
            paths["receipt"].write_text(json.dumps(receipt))
            rejected = verify_engine_receipt(paths["receipt"], request, paths["pftrace"])
            self.assertFalse(rejected["qualified"])
            self.assertIn("receipt_mismatch:config_sha256", rejected["errors"])

    def test_async_receipt_binds_transport_and_raw_spool(self):
        with tempfile.TemporaryDirectory() as temporary:
            paths = attempt_paths(Path(temporary), "async-run")
            paths["root"].mkdir(parents=True)
            paths["pftrace"].write_bytes(b"trace")
            raw_path = Path(str(paths["pftrace"]) + ".raw.jsonl")
            raw_path.write_text('{"schema":"dxmt9.trace.raw.v1"}\n')
            spool_path = Path(str(raw_path) + ".spool")
            spool_path.write_bytes(b"owned raw spool")
            request = make_trace_request(
                TRACE_PROFILES["timeline"], run_id="async-run", attempt_id="async-attempt",
                paths=paths, drain_mode="async", transport_events=2048,
            )
            proof_path = Path(str(paths["receipt"]) + ".completion.json")
            receipt = {
                "schema": "dxmt9.trace.receipt.v1", "run_id": "async-run",
                "attempt_id": "async-attempt", "profile_id": "timeline",
                "config_sha256": request["config_sha256"], "status": "complete",
                "generation": "7", "proof_required": True,
                "completion_proof": {"path": str(proof_path), "required": True,
                                     "publication": "after-decision-outside-budget"},
                "transport": dict(request["transport"]), "window": {"closed": True},
                "categories_resolved": request["categories"],
                "applicability": {name: {"status": "observed", "count": 0}
                                  for name in request["required_categories"]},
                "counts": {"engine": 12}, "loss": {"engine": 0},
                "flush": {"status": "complete"},
                "artifact": {"path": str(paths["pftrace"]), "bytes": 5,
                             "sha256": sha256_file(paths["pftrace"])},
                "raw_artifact": {"path": str(raw_path), "bytes": raw_path.stat().st_size,
                                 "sha256": sha256_file(raw_path)},
                "raw_spool_artifact": {
                    "path": str(spool_path), "bytes": spool_path.stat().st_size,
                    "sha256": sha256_file(spool_path),
                },
            }
            paths["receipt"].write_text(json.dumps(receipt))
            proof_path.write_text(json.dumps({
                "schema": "dxmt9.trace.completion.v1", "run_id": "async-run",
                "attempt_id": "async-attempt", "config_sha256": request["config_sha256"],
                "generation": "7", "status": "complete", "completed_ns": "10",
                "deadline_ns": "10", "candidate_receipt_sha256": sha256_file(paths["receipt"]),
            }))
            self.assertTrue(verify_engine_receipt(
                paths["receipt"], request, paths["pftrace"]
            )["qualified"])
            proof = json.loads(proof_path.read_text())
            proof["completed_ns"] = "11"
            proof_path.write_text(json.dumps(proof))
            late = verify_engine_receipt(paths["receipt"], request, paths["pftrace"])
            self.assertIn("completion_proof_deadline_exceeded", late["errors"])
            proof["completed_ns"] = "10"
            proof_path.write_text(json.dumps(proof))
            receipt["transport"]["transport_events"] = 1024
            paths["receipt"].write_text(json.dumps(receipt))
            rejected = verify_engine_receipt(paths["receipt"], request, paths["pftrace"])
            self.assertIn("receipt_mismatch:transport", rejected["errors"])
            self.assertIn("completion_proof_candidate_digest_mismatch", rejected["errors"])

    def test_receipt_rejects_loss_even_when_workload_trace_exists(self):
        with tempfile.TemporaryDirectory() as temporary:
            paths = attempt_paths(Path(temporary), "run-2")
            paths["root"].mkdir(parents=True)
            paths["pftrace"].write_bytes(b"trace")
            raw_path = Path(str(paths["pftrace"]) + ".raw.jsonl")
            raw_path.write_text('{"schema":"dxmt9.trace.raw.v1"}\n')
            request = make_trace_request(
                TRACE_PROFILES["timeline"], run_id="run-2", attempt_id="attempt-2", paths=paths
            )
            receipt = {
                "schema": "dxmt9.trace.receipt.v1", "run_id": "run-2",
                "attempt_id": "attempt-2", "profile_id": "timeline",
                "config_sha256": request["config_sha256"], "status": "complete",
                "window": {"closed": True},
                "categories_resolved": request["categories"],
                "applicability": {name: {"status": "observed", "count": 0}
                                  for name in request["required_categories"]},
                "counts": {"engine": 1}, "loss": {"engine": 1},
                "flush": {"status": "complete"},
                "artifact": {"path": str(paths["pftrace"]), "bytes": 5,
                             "sha256": sha256_file(paths["pftrace"])},
                "raw_artifact": {"path": str(raw_path), "bytes": raw_path.stat().st_size,
                                 "sha256": sha256_file(raw_path)},
            }
            paths["receipt"].write_text(json.dumps(receipt))
            verified = verify_engine_receipt(paths["receipt"], request, paths["pftrace"])
            self.assertFalse(verified["qualified"])
            self.assertIn("event_loss_or_missing_loss_table", verified["errors"])


if __name__ == "__main__":
    unittest.main()
