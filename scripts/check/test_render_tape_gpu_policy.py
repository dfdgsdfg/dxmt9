#!/usr/bin/env python3
"""Native-free strict GPU policy qualification contract tests."""

from __future__ import annotations

import copy
import errno
import hashlib
import importlib.util
import json
import pathlib
import tempfile
from unittest import mock


def _load(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


ROOT = pathlib.Path(__file__).resolve().parents[2]
GPU = _load("render_tape_gpu_policy_tested", ROOT / "scripts/tools/render_tape_gpu_policy.py")
RUNNER = _load("run_dxmt9_render_tape_tested", ROOT / "scripts/tools/run_dxmt9_render_tape.py")
CPU = _load("render_tape_cpu_policy_tested", ROOT / "scripts/tools/render_tape_cpu_policy.py")
SOURCE = _load("render_tape_source_policy_tested", ROOT / "scripts/tools/render_tape_source_policy.py")


def _source_policy_case(root):
    manifest, policy, receipt, readbacks = _case(root)
    manifest["components"]["identity"] = {
        "path": "identity.bin", "bytes": 1, "sha256": "a" * 64}
    def publication(index, present):
        return {"source_index": index, "source_generation": 1,
                "storage_first_page": index, "storage_page_count": 1,
                "storage_generation": 1, "raw_ordinal": 0,
                "source_ordinal": index + 1, "seq_id": index + 1,
                "build_generation": 0, "used_bytes": 16, "page_count": 1,
                "padding_pages_before": 0, "strict_admission": False,
                "control_index": index, "has_present": present}
    rows = [{"event_ordinal": 7, "source_ordinal": 100 + index,
             "captured_seq_id": 200 + index, "command_chunk_index": 0,
             "first_record": index, "record_count": 1,
             "local_raw_ordinal": 4, "range_index": index, "settled": True,
             "settlement": kind,
             "publications": ([] if index == 1 else [publication(index // 2, index == 2)])}
            for index, kind in enumerate(("published", "empty", "present-published"))]
    plan = SOURCE.source_plan_digest(manifest["components"], rows)
    policy.update(schema=CPU.SOURCE_SCHEMA, source_boundary_mode="capture-source",
                  source_plan_sha256=plan)
    policy = CPU.validate_policy(policy)
    source = {"requested": True, "validated": True, "complete": True,
              "plan_sha256": plan, "source_count": 3, "completed_source_count": 3,
              "publication_count": 2, "empty_source_count": 1,
              "present_source_count": 1, "storage_overflow": False, "sources": rows}
    receipt["source_boundary_mode"] = "capture-source"
    receipt["requested"].update(cpu_policy=policy, cpu_policy_sha256=CPU.policy_digest(policy),
                                source_boundary_mode="capture-source", source_plan_sha256=plan)
    receipt["result"]["source_boundary_mode"] = "capture-source"
    for iteration in receipt["result"]["iterations"]:
        iteration["provider_result"]["source_replay"] = copy.deepcopy(source)
    return manifest, policy, receipt, readbacks


def source_policy_cases(root):
    manifest, policy, good, readbacks = _source_policy_case(root)
    def accepts(receipt):
        return GPU.gpu_receipt_accepts(receipt, policy, manifest=manifest, bundle=root,
                                       readback_root=readbacks, runner=RUNNER)
    assert accepts(good)[0], accepts(good)
    source = good["result"]["iterations"][0]["provider_result"]["source_replay"]
    trace = [{"event": row["event_ordinal"], "submission": pub["seq_id"],
              "source": pub["source_index"], "submitted": True, "completed": True,
              **{key: pub[key] for key in ("source_generation", "storage_first_page",
                                          "storage_page_count", "storage_generation")}}
             for row in source["sources"] for pub in row["publications"]]
    cpu_row = {"source_replay": copy.deepcopy(source), "trace": trace}
    assert SOURCE.cpu_source_trace_matches(cpu_row)
    bad_cpu = copy.deepcopy(cpu_row)
    for row in bad_cpu["source_replay"]["sources"]:
        for publication in row["publications"]:
            publication["seq_id"] += 100
            publication["source_ordinal"] += 100
    assert SOURCE.source_replay_accepts(bad_cpu["source_replay"], policy, manifest["components"])
    assert not SOURCE.cpu_source_trace_matches(bad_cpu), "disjoint local completion authority"
    for variable in ("DXMT9_DUMP_COLOR_ATTACHMENT_DIR", "DXMT9_DUMP_COLOR_ATTACHMENT_PATH",
                     "DXMT9_DUMP_COLOR_ATTACHMENT_ROI_SUMMARY_PATH",
                     "DXMT9_DUMP_DRAW_TEXTURE_DIR", "DXMT_DUMP_SHADER_DIR",
                     "DXMT9_PROBE_VERTEX_POSITION_DIR", "DXMT9_PROBE_NO_CONTRACT_SOURCE_SHA256"):
        for path in ("/diagnostic", "0"):
            bad_observer = copy.deepcopy(good)
            bad_observer["renderer_environment"][variable] = path
            assert not accepts(bad_observer)[0], ("hidden observer", variable, path)
    assert CPU.validate_policy(CPU.default_policy("compatibility"))["schema"] == CPU.SCHEMA
    for bad_digest in (None, "0" * 64, "A" * 64, "f" * 63, True):
        bad = dict(policy, source_plan_sha256=bad_digest)
        try:
            CPU.validate_policy(bad)
        except CPU.PolicyError:
            pass
        else:
            raise AssertionError("invalid v2 plan digest admitted")
    for field, replacement in (("plan_sha256", "b" * 64), ("storage_overflow", True),
                               ("sources", []), ("source_count", True),
                               ("publication_count", 1), ("complete", False)):
        bad = copy.deepcopy(good)
        bad["result"]["iterations"][0]["provider_result"]["source_replay"][field] = replacement
        assert not accepts(bad)[0], field
    for field, replacement in (("first_record", 1), ("captured_seq_id", 201),
                               ("local_raw_ordinal", 0), ("range_index", 2),
                               ("settlement", "empty"), ("settled", False)):
        bad = copy.deepcopy(good)
        bad["result"]["iterations"][0]["provider_result"]["source_replay"]["sources"][0][field] = replacement
        assert not accepts(bad)[0], field
    for field, replacement in (("seq_id", 0), ("source_ordinal", 99),
                               ("raw_ordinal", 200), ("source_generation", 0),
                               ("storage_page_count", 2), ("has_present", True),
                               ("strict_admission", 0)):
        bad = copy.deepcopy(good)
        publication = bad["result"]["iterations"][0]["provider_result"]["source_replay"]["sources"][0]["publications"][0]
        publication[field] = replacement
        assert not accepts(bad)[0], field
    bad = copy.deepcopy(good)
    source = bad["result"]["iterations"][0]["provider_result"]["source_replay"]
    source["sources"][2]["publications"][0] = copy.deepcopy(source["sources"][0]["publications"][0])
    source["sources"][2]["publications"][0]["has_present"] = True
    assert not accepts(bad)[0], "reused local source"
    bad = copy.deepcopy(good)
    bad["result"]["source_boundary_mode"] = "chunk"
    assert not accepts(bad)[0]
    # A rehydrated report may be internally self-consistent but must still
    # authenticate the caller's name -> policy contract, not choose its own.
    qualification = {"accepted": True, "policies": {}}
    image = root / "image.metallib"
    image_digest = hashlib.sha256(image.read_bytes()).hexdigest()
    for name in ("reference", "candidate"):
        stored = copy.deepcopy(good)
        stored["provider"]["sha256"] = image_digest
        stored["validator"]["sha256"] = image_digest
        path = root / "gpu" / name / "result.json"
        path.parent.mkdir(parents=True)
        data = json.dumps(stored).encode()
        path.write_bytes(data)
        qualification["policies"][name] = {"sha256": hashlib.sha256(data).hexdigest()}
    expected = {name: policy for name in ("reference", "candidate")}
    with mock.patch.object(RUNNER, "load_bundle", return_value=(manifest, None, None, None, None, None)), \
         mock.patch.object(GPU, "gpu_receipt_accepts", return_value=(True, "accepted")):
        args = dict(staging=root, input_bundle=root, provider=image, validator=image, runner=RUNNER)
        assert GPU.final_reauthenticate(qualification, **args, expected_policies=expected)
        assert not GPU.final_reauthenticate(qualification, **args)
        wrong = dict(expected, reference=CPU.default_policy("compatibility"))
        assert not GPU.final_reauthenticate(qualification, **args, expected_policies=wrong)


def _case(root: pathlib.Path, *, route: str = "compatibility"):
    root.mkdir(parents=True, exist_ok=True)
    output = b"output-original"
    source = b"source-original"
    image = b"loaded-image"
    (root / "output.rgba").write_bytes(output)
    (root / "source.rgba").write_bytes(source)
    image_path = root / "image.metallib"
    image_path.write_bytes(image)
    def digest(data: bytes) -> str:
        return hashlib.sha256(data).hexdigest()
    components = {
        "events": {"path": "events.bin", "bytes": 1, "sha256": "e" * 64},
        "blobs": [],
        "output_oracle": {"path": "output.rgba", "bytes": len(output), "sha256": digest(output)},
        "source_oracle": {"path": "source.rgba", "bytes": len(source), "sha256": digest(source)},
    }
    manifest = {"scope": {"synthetic": True}, "components": components}
    policy = CPU.default_policy(route)
    readbacks = root / "readback"
    readbacks.mkdir()
    for kind, data in (("output", output), ("source", source)):
        (readbacks / f"iteration-000000-{kind}.rgba").write_bytes(data)
        (readbacks / f"iteration-000001-{kind}.rgba").write_bytes(data)

    def interval():
        return {"validity": {
            "oracle_mode": "strict", "expected_digest_matched": True,
            "output_readback": True,
            "source_readback": True, "expected_digest_captured": True,
            "output_oracle_matched": True, "output_non_degenerate": True,
            "output_sha256": digest(output), "source_sha256": digest(source),
        }, "present_ordinal": 1, "completion_ordinal": 1}
    def provider():
        return {"status": "complete", "profile": "frame-tape",
                "validity": {"structurally_valid": True, "digests_valid": True,
                    "output_readback": True, "expected_digest_captured": True,
                    "output_oracle_matched": True, "output_non_degenerate": True,
                    "oracle_mode": "strict", "expected_digest_matched": True,
                    "source_oracle_exact_matched": True, "source_oracle_matched": True,
                    "output_sha256": digest(output), "output_bytes": len(output),
                    "source_readback": True, "source_sha256": digest(source),
                    "source_bytes": len(source), "source_oracle_matched": True},
                "coverage": {"present_records": 1, "present_source_mappings": 1,
                    "present_outputs": 1},
                "conservation": {"input_blobs": 0, "referenced_blobs": 0,
                    "objects_created": 1, "objects_released": 1,
                    "present_ordinal": 1, "completion_ordinal": 1},
                "intervals": [interval()]}
    iterations = []
    for index in range(2):
        iterations.append({"index": index, "warmup": False, "reset_ok": True,
            "provider_result": provider(),
            "path_counters": {"gpu_errors": 0, "encode_chunks": 1,
                "direct_lease_draws": 0 if route == "compatibility" else 1,
                "direct_leases": 0 if route == "compatibility" else 1,
                "compatibility_draws": 1 if route == "compatibility" else 0},
            "observed_replay_path": "compatibility" if route == "compatibility" else "direct-slot",
            "setup_ns": 1, "replay_ns": 1, "reset_ns": 1,
            "readback_artifacts": {
                "output_rgba": {"path": f"iteration-{index:06d}-output.rgba",
                    "bytes": len(output), "sha256": digest(output)},
                "source_rgba": {"path": f"iteration-{index:06d}-source.rgba",
                    "bytes": len(source), "sha256": digest(source)}}})
    result = {"schema": "dxmt9.render_tape.experiment.v1", "status": "complete",
        "backend": "metal", "reset": "cold-device", "replay_route": route,
        "completion": "real-gpu",
        "prepare_count": 1, "pixel_deterministic": True, "iterations": iterations,
        "loaded_images": [{"path": str(image_path), "bytes": len(image), "sha256": digest(image)}],
        "readback_artifacts_enabled": True}
    receipt = {"schema": GPU.ARTIFACT_SCHEMA, "accepted": True,
        "provider_exit_code": 0, "compare_wrapper_exit_code": 0,
        "compare_wrapper_timeout": False, "diagnostic": False,
        "metal_validation_diagnostic": False, "loaded_images_stable": True,
        "readback_artifacts_valid": True, "provider": {"path": "/provider", "sha256": "p" * 64},
        "validator": {"path": "/validator", "sha256": "v" * 64},
        "input_components": components,
        "input_scope": manifest["scope"],
        "input_inspection": {"schema": "dxmt9.render_tape.v2", "valid": True,
                             "events_sha256": components["events"]["sha256"],
                             "grammar": {"descriptor_kinds": {}, "record_types": {},
                                         "control_kinds": {}}},
        "renderer_environment": {},
        "forced_environment": GPU.policy_environment(policy),
        "requested": {"warmup": 0, "repeat": 2, "reset": "cold-device",
            "replay_route": route, "partition_mode": "identity", "require_path": True,
            "cpu_policy": policy, "cpu_policy_sha256": CPU.policy_digest(policy)},
        "result": result}
    return manifest, policy, receipt, readbacks


def gpu_input_staging_cases(root: pathlib.Path) -> None:
    """The GPU path uses the same independent owned snapshot as CPU lanes."""
    source = root / "original"
    source.mkdir(parents=True)
    events = source / "events.bin"
    events.write_bytes(b"original immutable events")
    args = RUNNER.parser().parse_args([
        "experiment", str(source), "--output-dir", str(root / "output"),
    ])
    stop_after_stage = RuntimeError("stop before validation or GPU effects")

    def inspect_staged(staged):
        target = staged / "events.bin"
        assert target.read_bytes() == events.read_bytes()
        assert target.stat().st_ino != events.stat().st_ino
        target.write_bytes(b"independent staged mutation")
        assert events.read_bytes() == b"original immutable events"
        raise stop_after_stage

    with mock.patch.object(RUNNER, "copy_input_owned",
                           wraps=RUNNER.copy_input_owned) as copied, \
         mock.patch.object(RUNNER, "load_bundle", side_effect=inspect_staged) as loaded, \
         mock.patch.object(RUNNER, "run_validator") as validated:
        try:
            RUNNER.fixed_input_experiment(args)
        except RuntimeError as error:
            assert error is stop_after_stage
        else:
            raise AssertionError("GPU staging probe reached execution")
        copied.assert_called_once()
        assert copied.call_args.args[0] == source.resolve()
        loaded.assert_called_once_with(copied.call_args.args[1])
        validated.assert_not_called()
    assert not args.output_dir.exists()

    failure = OSError(errno.ENOSPC, "injected input clone failure")
    with mock.patch.object(RUNNER, "copy_input_owned", side_effect=failure) as copied, \
         mock.patch.object(RUNNER, "load_bundle") as loaded, \
         mock.patch.object(RUNNER, "run_validator") as validated:
        try:
            RUNNER.fixed_input_experiment(args)
        except OSError as error:
            assert error is failure
        else:
            raise AssertionError("GPU staging swallowed clone failure")
        copied.assert_called_once()
        loaded.assert_not_called()
        validated.assert_not_called()
    assert not args.output_dir.exists()
    assert events.read_bytes() == b"original immutable events"


def main() -> int:
    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary)
        gpu_input_staging_cases(root / "gpu-staging")
        source_policy_cases(root / "source-v2")
        manifest, policy, good, readbacks = _case(root)
        accepted, reason = GPU.gpu_receipt_accepts(
            good, policy, manifest=manifest, bundle=root,
            readback_root=readbacks, runner=RUNNER)
        assert accepted, reason
        default_input_audit = {
            "requested": False, "diagnostic": True, "validated": False,
            "attempted": False, "uploaded": False, "readback_matched": False,
            "kind": 0, "generation": 0, "object_id": 0, "after_captured_seq": 0, "bytes": 0,
            "input_sha256": "0" * 64, "before_sha256": "0" * 64, "after_sha256": "0" * 64}
        assert GPU.texture_input_audit_is_default({})
        assert GPU.texture_input_audit_is_default({"texture_input_audit": default_input_audit})
        for field, value in (("requested", True), ("validated", True), ("attempted", True),
                             ("uploaded", True), ("readback_matched", True), ("bytes", 16),
                             ("generation", 1), ("object_id", 2), ("after_captured_seq", 3),
                             ("input_sha256", "a" * 64), ("before_sha256", "b" * 64),
                             ("after_sha256", "c" * 64)):
            changed = {**default_input_audit, field: value}
            assert not GPU.texture_input_audit_is_default({"texture_input_audit": changed})
            forged = copy.deepcopy(good)
            forged["result"]["iterations"][0]["provider_result"]["texture_input_audit"] = changed
            assert not GPU.gpu_receipt_accepts(forged, policy, manifest=manifest,
                bundle=root, readback_root=readbacks, runner=RUNNER)[0], field
        for malformed in (None, [], {"requested": False}):
            assert not GPU.texture_input_audit_is_default({"texture_input_audit": malformed})
        default_args = RUNNER.parser().parse_args(
            ["experiment", "bundle", "--output-dir", "out"]
        )
        assert default_args.audit_source_boundaries is False
        assert default_args.audit_texture_input is None
        assert default_args.audit_batch_cuts == []
        assert default_args.source_boundary_mode == "chunk"
        source_args = RUNNER.parser().parse_args([
            "experiment", "bundle", "--output-dir", "out",
            "--source-boundary-mode", "capture-source",
        ])
        assert source_args.source_boundary_mode == "capture-source"
        assert source_args.audit_source_boundaries is False
        requested_args = RUNNER.parser().parse_args([
            "experiment", "bundle", "--output-dir", "out",
            "--audit-source-boundaries", "--audit-batch-cut", "7:11",
            "--audit-batch-cut", "9:0",
        ])
        assert requested_args.audit_batch_cuts == ["7:11", "9:0"]
        malformed_args = RUNNER.parser().parse_args([
            "experiment", "bundle", "--output-dir", "out",
            "--audit-source-boundaries", "--audit-batch-cut=-1:2",
        ])
        assert malformed_args.audit_batch_cuts == ["-1:2"]
        dependency_args = RUNNER.parser().parse_args([
            "experiment", "bundle", "--output-dir", "out",
            "--audit-batch-cut", "7:11",
        ])
        try:
            RUNNER.fixed_input_experiment(dependency_args)
        except SystemExit as error:
            assert "require --audit-source-boundaries" in str(error)
        else:
            raise AssertionError("batch-cut dependency was not rejected")

        source_diagnostic_receipt = copy.deepcopy(good)
        source_diagnostic_receipt["diagnostic"] = False
        source_diagnostic_receipt["result"]["audit_source_boundaries"] = True
        source_diagnostic_receipt["result"]["iterations"][0][
            "provider_result"]["source_boundary_audit"] = {
                "requested": True, "diagnostic": True, "validated": True,
                "complete": True, "source_count": 1,
                "completed_source_count": 1,
            }
        assert not GPU.gpu_receipt_accepts(
            source_diagnostic_receipt, policy, manifest=manifest,
            bundle=root, readback_root=readbacks, runner=RUNNER)[0]

        # Source-boundary replay is an explicitly perturbative diagnostic.  A
        # normal experiment receipt remains admissible when the metadata is
        # absent/default-off, while either the native root marker or the
        # provider evidence makes it ineligible for qualification.
        experiment = copy.deepcopy(good["result"])
        experiment.update({
            "schema": "dxmt9.render_tape.experiment.v1",
            "status": "complete",
            "backend": "metal",
            "reset": "cold-device",
            "replay_route": "compatibility",
            "prepare_count": 1,
            "pixel_deterministic": True,
        })
        assert RUNNER.experiment_accepts(
            experiment, warmup=0, repeat=2, reset="cold-device",
            route="compatibility", require_path=True, non_degenerate=False,
            input_inspection=good["input_inspection"])
        experiment["source_boundary_mode"] = "capture-source"
        assert not RUNNER.experiment_accepts(
            experiment, warmup=0, repeat=2, reset="cold-device",
            route="compatibility", require_path=True, non_degenerate=False)
        experiment.pop("source_boundary_mode")
        experiment["iterations"][0]["provider_result"]["source_replay"] = {
            "requested": True, "validated": True, "complete": True,
        }
        assert not RUNNER.experiment_accepts(
            experiment, warmup=0, repeat=2, reset="cold-device",
            route="compatibility", require_path=True, non_degenerate=False)
        experiment["iterations"][0]["provider_result"]["source_replay"]["requested"] = False
        assert not RUNNER.experiment_accepts(
            experiment, warmup=0, repeat=2, reset="cold-device",
            route="compatibility", require_path=True, non_degenerate=False)
        experiment["iterations"][0]["provider_result"].pop("source_replay")
        experiment["audit_source_boundaries"] = True
        assert not RUNNER.experiment_accepts(
            experiment, warmup=0, repeat=2, reset="cold-device",
            route="compatibility", require_path=True, non_degenerate=False)
        experiment.pop("audit_source_boundaries")
        for iteration in experiment["iterations"]:
            iteration["provider_result"]["source_boundary_audit"] = {
                "requested": True, "diagnostic": True, "validated": True,
                "complete": True, "source_count": 1,
                "completed_source_count": 1,
            }
        assert not RUNNER.experiment_accepts(
            experiment, warmup=0, repeat=2, reset="cold-device",
            route="compatibility", require_path=True, non_degenerate=False)

        negative_index = 0
        def rejects(mutator, expected_reason):
            nonlocal negative_index
            negative_index += 1
            case_root = root / f"negative-{negative_index}"
            case_manifest, case_policy, case_good, case_readbacks = _case(case_root)
            assert GPU.gpu_receipt_accepts(
                case_good, case_policy, manifest=case_manifest, bundle=case_root,
                readback_root=case_readbacks, runner=RUNNER)[0]
            broken = copy.deepcopy(case_good)
            mutator(broken, case_readbacks)
            ok, reason = GPU.gpu_receipt_accepts(
                broken, case_policy, manifest=case_manifest, bundle=case_root,
                readback_root=case_readbacks, runner=RUNNER)
            assert not ok and reason == expected_reason, reason

        rejects(lambda value, _: value["result"]["iterations"][0]["provider_result"]["validity"].pop("expected_digest_matched"), "gpu-top-level-strict-source-oracle-missing")
        rejects(lambda value, _: value["result"]["iterations"][0]["provider_result"]["validity"].update(oracle_mode="pixel-envelope"), "gpu-top-level-strict-source-oracle-missing")
        rejects(lambda value, _: [item["provider_result"]["intervals"][0]["validity"].update(source_sha256="0" * 64)
                                  for item in value["result"]["iterations"]], "source-original-oracle-mismatch")
        rejects(lambda value, readback: (readback / "iteration-000000-output.rgba").write_bytes(b"tampered"), "gpu-readback-artifacts-invalid")
        rejects(lambda value, _: value.update(metal_validation_diagnostic=True), "gpu-artifact-rejected")
        rejects(lambda value, _: value.update(audit_source_boundaries=True),
                "gpu-artifact-rejected")
        rejects(lambda value, _: value.update(audit_batch_cuts=["7:11"]),
                "gpu-artifact-rejected")
        for malformed_cuts in (None, False, True, "7:11", {}, ["7:11"]):
            rejects(lambda value, _, cuts=malformed_cuts: value.update(
                audit_batch_cuts=cuts), "gpu-artifact-rejected")
        rejects(lambda value, _: value["requested"].update(audit_source_boundaries=True),
                "gpu-source-boundary-diagnostic")
        for malformed_cuts in (None, False, True, "7:11", {}, ["7:11"]):
            rejects(lambda value, _, cuts=malformed_cuts: value["requested"].update(
                audit_batch_cuts=cuts), "gpu-source-boundary-batch-cuts")
        for malformed_cuts in (None, False, True, "7:11", {}, ["7:11"]):
            rejects(lambda value, _, cuts=malformed_cuts: value["result"].update(
                audit_batch_cuts=cuts), "gpu-experiment-rejected")
        rejects(lambda value, _: value["result"]["iterations"][0][
            "provider_result"].update(source_boundary_audit={
                "requested": False, "diagnostic": True, "validated": False,
                "complete": False, "source_count": 0,
                "completed_source_count": 0, "batch_cut_count": 1,
                "completed_batch_cut_count": 0}), "gpu-experiment-rejected")
        rejects(lambda value, _: value["result"].update(source_boundary_audit={
                "requested": False, "diagnostic": True, "validated": False,
                "complete": False, "source_count": 0,
                "completed_source_count": 0, "batch_cut_count": 1,
                "completed_batch_cut_count": 0}), "gpu-experiment-rejected")
        for malformed_audit in (None, True, [], "false", {}, {"requested": 0}):
            rejects(lambda value, _, audit=malformed_audit: value["result"]["iterations"][0][
                        "provider_result"].update(source_boundary_audit=audit),
                    "gpu-experiment-rejected")
        rejects(lambda value, _: value.pop("renderer_environment"),
                "gpu-renderer-environment-missing-or-invalid")
        rejects(lambda value, _: value.update(renderer_environment={"DXMT_TEST": 1}),
                "gpu-renderer-environment-missing-or-invalid")
        rejects(lambda value, _: value["requested"].update(replay_route="production-slot"), "gpu-request-policy-mismatch")
        rejects(lambda value, _: value["result"]["iterations"][0]["path_counters"].update(gpu_errors=1), "gpu-experiment-rejected")
        rejects(lambda value, _: value["forced_environment"].update(DXMT9_DRAW_CHUNK_COMMAND_LIMIT="9"), "gpu-policy-environment-mismatch")
        rejects(lambda value, _: value["input_components"]["source_oracle"].update(sha256="0" * 64), "gpu-input-identity-mismatch")
        no_scope = copy.deepcopy(manifest)
        no_scope["scope"] = {"production_capture": False}
        ok, _ = GPU.gpu_receipt_accepts(good, policy, manifest=no_scope,
                                        bundle=root, readback_root=readbacks, runner=RUNNER)
        assert not ok
        assert GPU.original_oracles(manifest, root)[1] == "synthetic-test-input"
        production = copy.deepcopy(manifest)
        production["scope"] = {"production_capture": True}
        assert GPU.original_oracles(production, root)[1] == "production-capture"
        contradictory = copy.deepcopy(production)
        contradictory["scope"]["synthetic"] = True
        assert GPU.original_oracles(contradictory, root)[0] is None
        mislabeled = copy.deepcopy(good)
        mislabeled["input_scope"] = production["scope"]
        assert GPU.gpu_receipt_accepts(
            mislabeled, policy, manifest=manifest, bundle=root,
            readback_root=readbacks, runner=RUNNER) == (False, "gpu-input-scope-mismatch")
        # A/A is a real pair of strict acceptance calls, not an equality-only shortcut.
        manifest_a, policy_a, receipt_a, readbacks_a = _case(root / "aa")
        manifest_b, policy_b, receipt_b, readbacks_b = _case(root / "aa-b")
        assert GPU.gpu_receipt_accepts(receipt_a, policy_a, manifest=manifest_a,
                                       bundle=root / "aa", readback_root=readbacks_a,
                                       runner=RUNNER)[0]
        assert GPU.gpu_receipt_accepts(receipt_b, policy_b, manifest=manifest_b,
                                       bundle=root / "aa-b", readback_root=readbacks_b,
                                       runner=RUNNER)[0]
        policy_ab = {**policy, "draw_command_limit": 4}
        receipt_ab = copy.deepcopy(good)
        receipt_ab["forced_environment"].update(GPU.policy_environment(policy_ab))
        receipt_ab["requested"].update(
            cpu_policy=policy_ab, cpu_policy_sha256=CPU.policy_digest(policy_ab))
        assert GPU._gpu_pair_fingerprint(good) == GPU._gpu_pair_fingerprint(receipt_ab)
        receipt_ab["renderer_environment"] = {"DXMT_UNEXPECTED_TEST_KNOB": "1"}
        assert GPU._gpu_pair_fingerprint(good) != GPU._gpu_pair_fingerprint(receipt_ab)
        for invalid_names in ({}, {"reference": {}}, {"unexpected": {}, "candidate": {}}):
            assert not GPU.final_reauthenticate(
                {"accepted": True, "policies": invalid_names}, staging=root,
                input_bundle=root, provider=root / "absent-provider",
                validator=root / "absent-validator", runner=RUNNER)

        # Exercise the real CPU compare dispatch gate: a failed GPU pair must
        # never invoke a measure child.
        cpu_test_helpers = _load(
            "render_tape_cpu_policy_helpers",
            ROOT / "scripts/check/test_render_tape_cpu_policy.py")
        gate_root = root / "cpu-gate"
        gate_root.mkdir()
        (gate_root / "events.bin").write_bytes(b"owned")
        policy_file = gate_root / "policy.json"
        policy_file.write_bytes(CPU.canonical_policy(policy))
        with tempfile.TemporaryDirectory() as artifacts:
            image_path = pathlib.Path(artifacts) / "image.metallib"
            image_path.write_bytes(b"image")
            verified = cpu_test_helpers._native(CPU, policy, repeat=2)
            artifact = cpu_test_helpers._artifact(verified, accepted=True,
                                                   image_path=image_path)
            artifact["forced_environment"].update(GPU.policy_environment(policy))
            calls = []
            def child(command, timeout):
                calls.append(command)
                if command[command.index("--run-mode") + 1] == "measure":
                    raise AssertionError("GPU failure dispatched a measure child")
                output = pathlib.Path(command[command.index("--output-dir") + 1])
                output.mkdir(parents=True)
                (output / "result.json").write_text(json.dumps(artifact))
                return copy.deepcopy(artifact)
            fake_gpu = type("FakeGpu", (), {
                "SCHEMA": GPU.SCHEMA,
                "qualify_policy_pair": staticmethod(lambda **_: {
                    "schema": GPU.SCHEMA, "accepted": False,
                    "qualification_scope": "synthetic-test-input",
                    "failures": [{"name": "gpu-pair-fingerprint-mismatch"}],
                    "policies": {}}),
                "final_reauthenticate": staticmethod(lambda *_args, **_: False),
            })
            args = type("Args", (), {
                "bundle": gate_root, "reference_policy": policy_file,
                "candidate_policy": policy_file, "repeat": 2, "warmup": 1,
                "output_dir": gate_root / "out", "provider": pathlib.Path("/provider"),
                "gpu_provider": pathlib.Path("/gpu-provider"),
                "validator": pathlib.Path("/validator"), "timeout_seconds": 1,
                "gpu_qualify": True, "gpu_reset": "cold-device"})()
            with mock.patch.object(CPU, "_child", side_effect=child), \
                    mock.patch.object(CPU, "_final_identity_check", return_value=True), \
                    mock.patch.object(CPU, "_gpu_module", return_value=fake_gpu):
                assert CPU.cpu_policy_compare(args) == 1
            assert len(calls) == 2
            report = json.loads((args.output_dir / "result.json").read_text())
            assert report["measurement"] == {}

        # GPU/CPU loaded-image fingerprints are a real binding, not a live-file
        # existence check; changing one module identity rejects the pair.
        cpu_binding = copy.deepcopy(artifact)
        cpu_binding["result"]["cpu_policy"] = policy
        gpu_binding = copy.deepcopy(artifact)
        gpu_binding["requested"] = {"cpu_policy": policy,
            "cpu_policy_sha256": CPU.policy_digest(policy)}
        module_image = {"path": "/shared/module.dylib", "bytes": 1,
                        "sha256": "m" * 64}
        cpu_binding["result"]["loaded_images"].append(copy.deepcopy(module_image))
        gpu_binding["result"]["loaded_images"].append(copy.deepcopy(module_image))
        assert GPU.cpu_gpu_binding_matches(cpu_binding, gpu_binding, policy)[0]
        gpu_binding["result"]["loaded_images"][1]["sha256"] = "f" * 64
        assert not GPU.cpu_gpu_binding_matches(cpu_binding, gpu_binding, policy)[0]

        # Missing original oracles reject before even the first GPU child is
        # launched, preserving the failure context without a fake experiment.
        missing_root = root / "missing-oracles"
        missing_root.mkdir()
        events = missing_root / "events.bin"
        events.write_bytes(b"events")
        manifest_missing = {
            "schema": RUNNER.SCHEMA, "profile": "frame-tape",
            "scope": {"production_capture": True},
            "components": {"events": {"path": "events.bin", "bytes": 6,
                                        "sha256": hashlib.sha256(b"events").hexdigest()},
                            "blobs": []},
        }
        (missing_root / "manifest.json").write_text(json.dumps(manifest_missing))
        fake_provider = root / "provider-binary"
        fake_validator = root / "validator-binary"
        fake_provider.write_bytes(b"provider")
        fake_validator.write_bytes(b"validator")
        child_calls = []
        report = GPU.qualify_policy_pair(
            input_bundle=missing_root, staging=missing_root / "out",
            provider=fake_provider, validator=fake_validator,
            reference_policy=policy, candidate_policy=policy, timeout_seconds=1,
            child_runner=lambda command, timeout: child_calls.append(command))
        assert report["accepted"] is False and child_calls == []
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
