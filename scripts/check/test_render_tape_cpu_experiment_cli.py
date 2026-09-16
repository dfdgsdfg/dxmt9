#!/usr/bin/env python3
"""Real buffer Tape -> production Queue/Pool, with no GPU completion fiction."""

import copy
import errno
import hashlib
import io
import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile
from unittest import mock


def bounded_phase_receipt_tests(module, evidence, options):
    assert module.cpu_experiment_accepts(evidence, **options)
    for field, invalid in (
        ("phase_accounting", "overlapping"), ("clock", "thread_cpu"),
        ("reset_policy", "warm"), ("cache_policy", "warm"),
        ("shared_native_resource_device", 1),
        ("native_cpu_clock", "wall"), ("import_ns", -1),
        ("import_ns", True), ("shared_device_setup_ns", "0"),
    ):
        broken = copy.deepcopy(evidence)
        broken[field] = invalid
        assert not module.cpu_experiment_accepts(broken, **options), field
        del broken[field]
        assert not module.cpu_experiment_accepts(broken, **options), field
    for field in ("elapsed_ns", "phase_ns", "native_cpu_ns"):
        broken = copy.deepcopy(evidence)
        del broken["iterations"][0][field]
        assert not module.cpu_experiment_accepts(broken, **options), field
    for invalid in (True, -1, "0", None):
        broken = copy.deepcopy(evidence)
        broken["native_cpu_clock"] = "process"
        broken["iterations"][0]["native_cpu_ns"] = invalid
        assert not module.cpu_experiment_accepts(broken, **options), "CPU clock"
    unavailable = copy.deepcopy(evidence)
    unavailable["native_cpu_clock"] = "unavailable"
    for row in unavailable["iterations"]:
        row["native_cpu_ns"] = None
    assert module.cpu_experiment_accepts(unavailable, **options)
    unavailable["iterations"][0]["native_cpu_ns"] = 0
    assert not module.cpu_experiment_accepts(unavailable, **options)
    for phase in ("setup", "replay", "observe", "completion", "cleanup"):
        for invalid in (True, -1, "0", None):
            broken = copy.deepcopy(evidence)
            broken["iterations"][0]["phase_ns"][phase] = invalid
            assert not module.cpu_experiment_accepts(broken, **options), phase
        broken = copy.deepcopy(evidence)
        del broken["iterations"][0]["phase_ns"][phase]
        assert not module.cpu_experiment_accepts(broken, **options), phase
    broken = copy.deepcopy(evidence)
    row = broken["iterations"][0]
    row["phase_ns"]["observe"] = row["elapsed_ns"] + 1
    assert not module.cpu_experiment_accepts(broken, **options), "double counting"
    broken = copy.deepcopy(evidence)
    broken["iterations"][0]["phase_ns"]["gpu"] = 0
    assert not module.cpu_experiment_accepts(broken, **options), "invented phase"


def _general_frame_fixture():
    inspection = {
        "schema": "dxmt9.render_tape.v2",
        "valid": True,
        "events": 1,
        "objects": 1,
        "mutations": 0,
        "chunks": 1,
        "records": 1,
        "draws": 1,
        "record_types": {"generate_mipmaps": 0},
        "event_types": [5],
    }
    result = {
        "schema": "dxmt9.render_tape.cpu_experiment.v1",
        "grammar": "general-frame",
        "semantic_projection": "final-slot-general-v2",
        "attachment_contents": "backend-command-owned-v1", "gpu_readback": False,
        "reset_policy": "fresh-device",
        "cache_policy": "fresh-runtime", "shared_native_resource_device": True,
        "backend": "cpu-metal-resources",
        "resource_execution": "cpu-visible-metal-no-gpu-submit",
        "status": "complete", "replay_route": "production-slot",
        "completion": "after-command", "publication": "per-command",
        "fault": "none", "repeat": 1, "completion_plan_sha256": None,
        "immutable_input_owned": True, "deterministic": True,
        "native_cpu_clock": "process",
        "effective_cpu_policy": {
            "reset": "fresh-device", "cpu_ready_arena": False,
            "draw_command_limit": 0, "draw_payload_limit": 0,
            "early_prefix": False, "gpu_submit": False},
        "input_counts": {"events": 1, "objects": 1, "mutations": 0,
                          "chunks": 1, "records": 1, "draws": 1},
        "iterations": [{
            "native_cpu_ns": 3,
            "index": 0, "complete": True, "reset_ok": True,
            "fault_triggered": False, "failure": "",
            "input_counts": {"events": 1, "objects": 1, "mutations": 0,
                              "chunks": 1, "records": 1, "draws": 1},
            "phase_ns": {"setup": 1, "replay": 1, "observe": 1,
                          "completion": 1, "cleanup": 1},
            "logical_resource_oracle_complete": True,
            "physical_texture_oracle": False,
            "buffer_backing_oracle_complete": True,
            "buffer_backing_observed": True,
            "buffer_snapshot_observations": 1,
            "buffer_live_binding_observations": 1,
            "buffer_binding_observations": 2,
            "buffer_backing_oracle": "real-metal-cpu-visible",
            "command_count": 1, "draw_count": 1, "direct_draws": 1,
            "compatibility_draws": 0, "submitted": 1, "completed": 1,
            "reclaimed": 1, "resources_created": 1, "resources_released": 1,
            "pending_peak": 1, "deferred_releases": 0,
            "destroys_with_pending": 0,
            "retained_snapshots_after_destroy": 0,
            "mipmap_command_count": 0,
            "elapsed_ns": 4, "semantic_digest": "1",
            "trace": [{"iteration": 1, "submission": 1, "source": 1,
                       "source_generation": 1, "storage_first_page": 0,
                       "storage_page_count": 1, "storage_generation": 1,
                       "first_event": 1, "last_event": 1, "event": 1,
                       "event_submission": 1, "released_at_event": 1,
                       "submitted": True, "completed": True}],
            "draw_observations": [{"ordinal": 1, "publication_event": 1,
                                    "cumulative_semantic_digest": "1"}],
        }],
    }
    return inspection, result


def cpu_input_staging_tests(module):
    """Exercise the bounded CPU staging copy without a provider or GPU."""
    with tempfile.TemporaryDirectory(prefix="dxmt9-cpu-stage-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "source;$(must-not-run)"
        source.mkdir()
        source_bytes = b"events\x00payload\xff\n"
        source_file = source / "events.bin"
        source_file.write_bytes(source_bytes)

        destination = root / "destination"
        module.copy_input_owned(source, destination)
        destination_file = destination / source_file.name
        assert destination_file.read_bytes() == source_bytes
        assert destination_file.stat().st_ino != source_file.stat().st_ino
        destination_file.write_bytes(b"destination-only")
        assert source_file.read_bytes() == source_bytes
        source_file.write_bytes(b"source-only")
        assert destination_file.read_bytes() == b"destination-only"
        source_file.write_bytes(source_bytes)

        dangling = root / "dangling-target"
        dangling.symlink_to(root / "missing")
        try:
            module.copy_input_owned(source, dangling)
        except FileExistsError:
            pass
        else:
            raise AssertionError("dangling staging destination was accepted")
        assert dangling.is_symlink()

        linked_source = root / "linked-source"
        linked_source.mkdir()
        (linked_source / "events.bin").symlink_to(source_file)
        linked_copy = root / "linked-copy"
        module.copy_input_owned(linked_source, linked_copy)
        assert not (linked_copy / "events.bin").is_symlink()
        assert (linked_copy / "events.bin").read_bytes() == source_bytes
        (linked_copy / "events.bin").write_bytes(b"isolated-link-copy")
        assert source_file.read_bytes() == source_bytes

        existing = root / "existing"
        existing.mkdir()
        existing_file = existing / "keep.bin"
        existing_file.write_bytes(b"keep me")
        with mock.patch.object(module.subprocess, "run") as invoked:
            try:
                module.copy_input_owned(source, existing)
            except FileExistsError:
                pass
            else:
                raise AssertionError("existing CPU staging target was accepted")
            invoked.assert_not_called()
        assert existing_file.read_bytes() == b"keep me"

        fallback = root / "fallback"
        with mock.patch.object(module.platform, "system", return_value="Linux"), \
                mock.patch.object(module.shutil, "copytree",
                                  wraps=module.shutil.copytree) as copied:
            module.copy_input_owned(source, fallback)
            copied.assert_called_once_with(source, fallback)
        assert (fallback / source_file.name).read_bytes() == source_bytes
        assert (fallback / source_file.name).stat().st_ino != source_file.stat().st_ino

        failed = root / "failed"
        failure = subprocess.CalledProcessError(
            17, ["/bin/cp", "-c", "-R", "-L", str(source), str(failed)])
        with mock.patch.object(module.platform, "system", return_value="Darwin"), \
                mock.patch.object(module.subprocess, "run", side_effect=failure) as invoked:
            try:
                module.copy_input_owned(source, failed)
            except subprocess.CalledProcessError as error:
                assert error is failure
            else:
                raise AssertionError("CPU staging swallowed clone failure")
            invoked.assert_called_once_with(
                ["/bin/cp", "-c", "-R", "-L", str(source), str(failed)], check=True)
        assert not failed.exists()


def rejected_staging_retention_tests(module):
    """Keep CPU raw evidence when transaction publication runs out of space."""
    with tempfile.TemporaryDirectory(prefix="dxmt9-rejected-stage-") as temporary:
        root = pathlib.Path(temporary)

        output = root / "cpu-result"
        original_error = OSError(errno.ENOSPC, "injected staging fsync failure")
        failure_metadata = {}
        with mock.patch.object(module, "sync_directory", side_effect=original_error):
            try:
                with module.bundle_transaction(
                        output, preserve_rejected=True,
                        failure_metadata=failure_metadata) as staging:
                    child = staging / "reference"
                    child.mkdir()
                    (child / "provider.stdout").write_bytes(b"raw child evidence")
                    module.write_cpu_receipt(
                        staging / "result.json", b"{}", failure_metadata)
            except OSError as error:
                assert error is original_error
            else:
                raise AssertionError("injected ENOSPC was swallowed")
        assert not output.exists()
        rejected = sorted(root.glob(".cpu-result.rejected-*"))
        assert len(rejected) == 1, rejected
        assert (rejected[0] / "reference" / "provider.stdout").read_bytes() == (
            b"raw child evidence")
        marker = json.loads((rejected[0] / module.REJECTED_MARKER_NAME).read_text())
        assert marker["accepted"] is False
        assert marker["exception_type"] == "OSError"
        assert marker["exception_errno_name"] == "ENOSPC"
        assert marker["intended_target"] == str(output.absolute())
        diagnostic = marker["failure_metadata"]["receipt_write_diagnostic"]
        assert diagnostic["attempted_receipt_payload_bytes"] == 2
        assert type(diagnostic["free_bytes_before_receipt_write"]) is int
        assert (rejected[0] / "result.json").read_bytes() == b"{}"

        # If quarantine allocation itself fails, the populated private
        # staging directory remains available under its original name.
        allocation_output = root / "allocation-failure"
        allocation_error = OSError(errno.ENOSPC, "injected rejected-path ENOSPC")
        real_mkdtemp = module.tempfile.mkdtemp
        allocation_calls = 0

        def fail_rejected_allocation(*args, **kwargs):
            nonlocal allocation_calls
            allocation_calls += 1
            if allocation_calls == 2:
                raise allocation_error
            return real_mkdtemp(*args, **kwargs)

        allocation_stderr = io.StringIO()
        with mock.patch.object(module, "sync_directory", side_effect=original_error), \
                mock.patch.object(module.tempfile, "mkdtemp",
                                  side_effect=fail_rejected_allocation), \
                mock.patch.object(module.sys, "stderr", allocation_stderr):
            try:
                with module.bundle_transaction(
                        allocation_output, preserve_rejected=True) as staging:
                    (staging / "child.bin").write_bytes(b"allocation raw")
            except OSError as error:
                assert error is original_error
            else:
                raise AssertionError("allocation failure masked original ENOSPC")
        assert not allocation_output.exists()
        allocation_staging = sorted(root.glob(".allocation-failure.staging-*"))
        assert len(allocation_staging) == 1
        assert (allocation_staging[0] / "child.bin").read_bytes() == b"allocation raw"
        assert str(allocation_staging[0]) in allocation_stderr.getvalue()

        # Likewise, a quarantine rename failure leaves the same raw tree
        # untouched instead of falling back to destructive cleanup.
        rename_output = root / "rename-failure"
        rename_error = OSError(errno.EIO, "injected quarantine rename failure")
        rename_stderr = io.StringIO()
        with mock.patch.object(module, "sync_directory", side_effect=original_error), \
                mock.patch.object(module.pathlib.Path, "rename",
                                  side_effect=rename_error), \
                mock.patch.object(module.sys, "stderr", rename_stderr):
            try:
                with module.bundle_transaction(
                        rename_output, preserve_rejected=True) as staging:
                    (staging / "child.bin").write_bytes(b"rename raw")
            except OSError as error:
                assert error is original_error
            else:
                raise AssertionError("rename failure masked original ENOSPC")
        assert not rename_output.exists()
        rename_staging = sorted(root.glob(".rename-failure.staging-*"))
        assert len(rename_staging) == 1
        assert (rename_staging[0] / "child.bin").read_bytes() == b"rename raw"
        assert str(rename_staging[0]) in rename_stderr.getvalue()

        # A marker write failure is secondary: the raw tree survives and the
        # original publication error remains the one callers receive.
        marker_failure_output = root / "marker-failure"
        marker_error = OSError(errno.ENOSPC, "injected marker ENOSPC")
        with mock.patch.object(module, "sync_directory", side_effect=original_error), \
                mock.patch.object(module, "write_flushed", side_effect=marker_error):
            try:
                with module.bundle_transaction(
                        marker_failure_output, preserve_rejected=True) as staging:
                    (staging / "candidate.rgba").write_bytes(b"raw candidate")
            except OSError as error:
                assert error is original_error
            else:
                raise AssertionError("original ENOSPC was masked by marker failure")
        assert not marker_failure_output.exists()
        marker_failure_rejected = sorted(root.glob(".marker-failure.rejected-*"))
        assert len(marker_failure_rejected) == 1
        assert (marker_failure_rejected[0] / "candidate.rgba").read_bytes() == b"raw candidate"
        assert not (marker_failure_rejected[0] / module.REJECTED_MARKER_NAME).exists()

        # KeyboardInterrupt follows the same evidence-preserving path.
        interrupted_output = root / "interrupted"
        interrupt = KeyboardInterrupt()
        with mock.patch.object(module, "sync_directory", side_effect=interrupt):
            try:
                with module.bundle_transaction(
                        interrupted_output, preserve_rejected=True) as staging:
                    (staging / "child.bin").write_bytes(b"interrupted raw evidence")
            except KeyboardInterrupt as error:
                assert error is interrupt
            else:
                raise AssertionError("KeyboardInterrupt was swallowed")
        interrupted_rejected = sorted(root.glob(".interrupted.rejected-*"))
        assert len(interrupted_rejected) == 1
        assert (interrupted_rejected[0] / "child.bin").read_bytes() == (
            b"interrupted raw evidence")

        # Once renamed, the output is committed even if parent-directory
        # synchronization is interrupted. Do not report nonexistent staging.
        published_output = root / "published-interrupted"
        published_stderr = io.StringIO()
        with mock.patch.object(module, "sync_directory", side_effect=[None, interrupt]), \
                mock.patch.object(module.sys, "stderr", published_stderr):
            try:
                with module.bundle_transaction(
                        published_output, preserve_rejected=True) as staging:
                    (staging / "result.json").write_bytes(b"{}")
            except KeyboardInterrupt as error:
                assert error is interrupt
            else:
                raise AssertionError("post-publication interrupt was swallowed")
        assert (published_output / "result.json").read_bytes() == b"{}"
        assert not list(root.glob(".published-interrupted.*"))
        assert not published_stderr.getvalue()

        # A failed input copy that produced no evidence retains the historical
        # cleanup behavior; there is no rejected artifact to inspect.
        empty_output = root / "empty-copy"
        with mock.patch.object(module, "sync_directory",
                               side_effect=OSError(errno.ENOSPC, "before evidence")):
            try:
                with module.bundle_transaction(empty_output, preserve_rejected=True):
                    raise OSError(errno.ENOSPC, "input copy failed before evidence")
            except OSError:
                pass
            else:
                raise AssertionError("pre-evidence input-copy failure was swallowed")
        assert not empty_output.exists()
        assert not list(root.glob(".empty-copy.rejected-*"))

        # Existing output paths remain untouched and are never repurposed as
        # either a success bundle or a rejected staging directory.
        existing = root / "existing"
        existing.mkdir()
        keep = existing / "keep.txt"
        keep.write_bytes(b"keep")
        try:
            with module.bundle_transaction(existing, preserve_rejected=True):
                raise AssertionError("existing output was accepted")
        except SystemExit:
            pass
        assert keep.read_bytes() == b"keep"

        # The default remains cleanup-on-failure for GPU/publisher callers.
        default_output = root / "default-cleanup"
        with mock.patch.object(module, "sync_directory",
                               side_effect=original_error):
            try:
                with module.bundle_transaction(default_output) as staging:
                    (staging / "gpu.raw").write_bytes(b"gpu evidence")
            except OSError as error:
                assert error is original_error
            else:
                raise AssertionError("default transaction swallowed ENOSPC")
        assert not default_output.exists()
        assert not list(root.glob(".default-cleanup.rejected-*"))


def native_free_policy_mode_contract_tests(module):
    inspection, verify = _general_frame_fixture()
    policy = module.render_tape_cpu_policy.default_policy("production-slot")
    verify.update(run_mode="verify", cpu_policy=policy, max_pending=1,
                  phase_accounting="scoped-observations", runtime_perf_counters=True)
    row = verify["iterations"][0]
    row.update(full_oracle_calls=1, source_summary_commands=1, source_summary_draws=1,
               slot_provisioned=0, slot_provision_reused=0, slot_provision_failed=0,
               capacity_lease_denied=0, draw_limit_publishes=0)
    options = dict(repeat=1, route="production-slot", require_path=True,
                   completion="after-command", resource_backend="cpu-metal-resources",
                   publication="per-command", fault="none", completion_plan=None,
                   input_inspection=inspection, max_pending=1, cpu_policy=policy)
    assert module._cpu_general_frame_accepts(verify, **options)
    measure = copy.deepcopy(verify)
    measure.update(run_mode="measure", phase_accounting="iteration-only")
    measured = measure["iterations"][0]
    measured.update(full_oracle_calls=0, semantic_digest="0", draw_observations=[],
                    logical_resource_oracle_complete=False,
                    buffer_backing_oracle_complete=False, buffer_backing_observed=False,
                    buffer_snapshot_observations=0, buffer_live_binding_observations=0,
                    buffer_binding_observations=0, buffer_backing_oracle="not-applicable",
                    phase_ns={key: 0 for key in row["phase_ns"]})
    assert module._cpu_general_frame_accepts(measure, **options, measurement=True)
    diagnostic = copy.deepcopy(measure)
    diagnostic.update(run_mode="diagnose", phase_accounting="exclusive-diagnostic-wall-process")
    diagnosed = diagnostic["iterations"][0]
    diagnosed["native_cpu_ns"] = 100
    diagnosed["elapsed_ns"] = 100
    names = ("bookkeeping", "fixture_setup", "admission", "harness_setup",
             "object_setup", "mutation", "replay", "publication", "completion",
             "cleanup", "fixture_teardown")
    diagnosed["diagnostic_accounting"] = {
        "valid": True, "wall_ns": 11, "cpu_ns": 11,
        "phases": {name: {"wall_ns": 1, "cpu_ns": 1, "entries": 1} for name in names}}
    assert module._cpu_general_frame_accepts(
        diagnostic, **options, measurement=True, diagnostic=True)
    assert not module._cpu_general_frame_accepts(diagnostic, **options, measurement=True)
    assert not module._cpu_general_frame_accepts(diagnostic, **options)
    relabelled = copy.deepcopy(diagnostic)
    relabelled.update(run_mode="measure", phase_accounting="iteration-only")
    assert not module._cpu_general_frame_accepts(relabelled, **options, measurement=True)
    for field, value in (("valid", False), ("wall_ns", 12), ("cpu_ns", True),
                         ("phases", {})):
        broken = copy.deepcopy(diagnostic)
        broken["iterations"][0]["diagnostic_accounting"][field] = value
        assert not module._cpu_general_frame_accepts(
            broken, **options, measurement=True, diagnostic=True), field
    broken = copy.deepcopy(diagnostic)
    broken["iterations"][0]["native_cpu_ns"] = 10
    assert not module._cpu_general_frame_accepts(
        broken, **options, measurement=True, diagnostic=True)
    assert not module._cpu_general_frame_accepts(measure, **options)
    assert not module._cpu_general_frame_accepts(verify, **options, measurement=True)
    for field, value in (("full_oracle_calls", 1), ("semantic_digest", "1"),
                         ("logical_resource_oracle_complete", True),
                         ("buffer_binding_observations", 1), ("source_summary_draws", 0),
                         ("complete", False), ("reset_ok", False),
                         ("native_cpu_ns", -1), ("reclaimed", 0)):
        broken = copy.deepcopy(measure)
        broken["iterations"][0][field] = value
        assert not module._cpu_general_frame_accepts(
            broken, **options, measurement=True), field
    for field in ("source_summary_draws", "slot_provisioned", "capacity_lease_denied",
                  "full_oracle_calls", "buffer_binding_observations"):
        broken = copy.deepcopy(measure)
        del broken["iterations"][0][field]
        assert not module._cpu_general_frame_accepts(
            broken, **options, measurement=True), field
    broken = copy.deepcopy(measure)
    broken["iterations"][0]["phase_ns"]["observe"] = 1
    assert not module._cpu_general_frame_accepts(broken, **options, measurement=True)
    broken = copy.deepcopy(measure)
    broken["iterations"][0]["trace"][0]["source_generation"] = 0
    assert not module._cpu_general_frame_accepts(broken, **options, measurement=True)


def native_free_general_frame_contract_tests(module):
    native_free_policy_mode_contract_tests(module)
    inspection, result = _general_frame_fixture()
    options = dict(repeat=1, route="production-slot", completion="after-command",
                   require_path=True, resource_backend="cpu-metal-resources",
                   publication="per-command", input_inspection=inspection,
                   grammar="general-frame")
    assert module.cpu_experiment_accepts(result, **options)

    for field, value in (("reset_policy", "warm"), ("cache_policy", "warm"),
                         ("shared_native_resource_device", False),
                         ("native_cpu_clock", "thread")):
        broken = copy.deepcopy(result)
        broken[field] = value
        assert not module.cpu_experiment_accepts(broken, **options), field
        broken.pop(field)
        assert not module.cpu_experiment_accepts(broken, **options), field

    # A live binding can be observed without a captured versioned backing;
    # mixed observations remain valid when their total is conserved.
    live_only = copy.deepcopy(result)
    live_only_iteration = live_only["iterations"][0]
    live_only_iteration["buffer_snapshot_observations"] = 0
    live_only_iteration["buffer_live_binding_observations"] = 1
    live_only_iteration["buffer_binding_observations"] = 1
    assert module.cpu_experiment_accepts(live_only, **options)
    mixed = copy.deepcopy(result)
    mixed_iteration = mixed["iterations"][0]
    mixed_iteration["buffer_snapshot_observations"] = 2
    mixed_iteration["buffer_live_binding_observations"] = 3
    mixed_iteration["buffer_binding_observations"] = 5
    assert module.cpu_experiment_accepts(mixed, **options)

    for label, mutate in (
        ("binding-count-sum", lambda value: value.__setitem__(
            "buffer_binding_observations", 1)),
        ("binding-count-overflow", lambda value: value.__setitem__(
            "buffer_binding_observations", value["draw_count"] * 17 + 1)),
        ("missing-snapshot-count", lambda value: value.pop(
            "buffer_snapshot_observations")),
        ("missing-live-count", lambda value: value.pop(
            "buffer_live_binding_observations")),
        ("missing-binding-count", lambda value: value.pop(
            "buffer_binding_observations")),
        ("boolean-snapshot-count", lambda value: value.__setitem__(
            "buffer_snapshot_observations", True)),
        ("boolean-live-count", lambda value: value.__setitem__(
            "buffer_live_binding_observations", True)),
        ("boolean-binding-count", lambda value: value.__setitem__(
            "buffer_binding_observations", True)),
    ):
        broken = copy.deepcopy(result)
        mutate(broken["iterations"][0])
        assert not module.cpu_experiment_accepts(broken, **options), label
    old_projection = copy.deepcopy(result)
    old_projection["semantic_projection"] = "final-slot-general-v1"
    assert not module.cpu_experiment_accepts(old_projection, **options)

    for field, value in (("pending_peak", 2), ("deferred_releases", 1),
                         ("destroys_with_pending", 1),
                         ("retained_snapshots_after_destroy", 1)):
        broken = copy.deepcopy(result)
        broken["iterations"][0][field] = value
        assert not module.cpu_experiment_accepts(broken, **options), field
    for field, value, top_level in (
        ("schema", "wrong", True), ("grammar", "bounded", True),
        ("semantic_projection", "wrong", True),
        ("input_counts", {**result["iterations"][0]["input_counts"], "draws": 2},
         False),
        ("phase_ns", {"setup": 1}, False),
        ("physical_texture_oracle", True, False),
        ("buffer_backing_oracle_complete", False, False),
        ("buffer_snapshot_observations", -1, False),
        ("input_counts", {**result["input_counts"], "draws": True}, True),
        ("trace_iteration", True, False),
        ("index", False, False),
        ("effective_cpu_policy", {**result["effective_cpu_policy"], "draw_command_limit": False}, True),
    ):
        broken = copy.deepcopy(result)
        if field == "trace_iteration":
            broken["iterations"][0]["trace"][0]["iteration"] = value
        else:
            (broken if top_level else broken["iterations"][0])[field] = value
        assert not module.cpu_experiment_accepts(broken, **options), field
    for label, inspection_mutation, receipt_value in (
        ("missing-mipmap-record-type", lambda value: value.pop("record_types"), 0),
        ("negative-mipmap-record-type",
         lambda value: value["record_types"].__setitem__("generate_mipmaps", -1), 0),
        ("boolean-mipmap-record-type",
         lambda value: value["record_types"].__setitem__("generate_mipmaps", True), 0),
        ("mipmap-count-mismatch", lambda value: None, 1),
        ("boolean-mipmap-receipt", lambda value: None, True),
        ("negative-mipmap-receipt", lambda value: None, -1),
    ):
        broken_inspection = copy.deepcopy(inspection)
        inspection_mutation(broken_inspection)
        broken = copy.deepcopy(result)
        broken["iterations"][0]["mipmap_command_count"] = receipt_value
        assert not module.cpu_experiment_accepts(
            broken, **dict(options, input_inspection=broken_inspection)), label

    sparse_inspection = copy.deepcopy(inspection)
    sparse_inspection["record_types"].pop("generate_mipmaps")
    assert module.cpu_experiment_accepts(
        result, **dict(options, input_inspection=sparse_inspection))
    nested_inspection = copy.deepcopy(inspection)
    nested_inspection["grammar"] = {
        "record_types": nested_inspection.pop("record_types")}
    assert module.cpu_experiment_accepts(
        result, **dict(options, input_inspection=nested_inspection))

    positive_inspection = copy.deepcopy(inspection)
    positive_inspection["record_types"]["generate_mipmaps"] = 2
    positive = copy.deepcopy(result)
    positive["iterations"][0]["mipmap_command_count"] = 2
    assert module.cpu_experiment_accepts(
        positive, **dict(options, input_inspection=positive_inspection))
    for field, value in (("completion", "after-destroy"),
                         ("publication", "draw-island"),
                         ("resource_backend", "cpu-no-gpu"),
                         ("fault", "managed-allocation")):
        broken_options = dict(options)
        broken_options[field] = value
        assert not module.cpu_experiment_accepts(result, **broken_options), field

    # before-next-work keeps the source pending across ObjectDestroy and
    # releases it at the first subsequent non-destroy event (ObjectDefine).
    delayed_inspection = copy.deepcopy(inspection)
    delayed_inspection.update(events=3, objects=2, event_types=[5, 3, 2])
    delayed = copy.deepcopy(result)
    delayed["completion"] = "before-next-work"
    delayed["input_counts"] = {**result["input_counts"], "events": 3, "objects": 2}
    delayed_iteration = delayed["iterations"][0]
    delayed_iteration["input_counts"] = delayed["input_counts"]
    delayed_iteration["resources_created"] = 2
    delayed_iteration["resources_released"] = 2
    delayed_iteration["deferred_releases"] = 1
    delayed_iteration["destroys_with_pending"] = 1
    delayed_iteration["retained_snapshots_after_destroy"] = 1
    delayed_iteration["trace"][0]["released_at_event"] = 3
    delayed_options = dict(options, completion="before-next-work",
                            input_inspection=delayed_inspection)
    assert module.cpu_experiment_accepts(delayed, **delayed_options)
    for release in (1, 2):
        broken = copy.deepcopy(delayed)
        broken["iterations"][0]["trace"][0]["released_at_event"] = release
        assert not module.cpu_experiment_accepts(broken, **delayed_options), release
    broken = copy.deepcopy(delayed)
    broken["iterations"][0]["deferred_releases"] = 0
    assert not module.cpu_experiment_accepts(broken, **delayed_options)
    broken = copy.deepcopy(delayed)
    broken["iterations"][0]["destroys_with_pending"] = 0
    assert not module.cpu_experiment_accepts(broken, **delayed_options)
    broken = copy.deepcopy(delayed)
    broken["iterations"][0]["retained_snapshots_after_destroy"] = 0
    assert module.cpu_experiment_accepts(broken, **delayed_options)
    broken = copy.deepcopy(delayed)
    del broken["iterations"][0]["pending_peak"]
    assert not module.cpu_experiment_accepts(broken, **delayed_options)
    broken = copy.deepcopy(delayed)
    broken["iterations"][0]["pending_peak"] = 2
    assert not module.cpu_experiment_accepts(broken, **delayed_options)
    assert not module.cpu_experiment_accepts(
        delayed, **dict(delayed_options, grammar="bounded"))
    assert not module.cpu_experiment_accepts(
        delayed, **dict(delayed_options, completion_plan={}))

    # Max-two is authorized only by the independent inspector window witness;
    # queue sequence IDs need not start at one or be contiguous.
    window_inspection = copy.deepcopy(inspection)
    window_inspection.update(
        events=4, objects=2, chunks=2, records=2, draws=2,
        event_types=[5, 3, 5, 7], command_event_indices=[0, 2])
    def window_row(event):
        return {
            "event": event, "record_count": 1, "allowed_record_count": 1,
            "partitioned": True, "lease_eligible": True,
            "coordinator_count": 0, "ordered_control_count": 0,
            "compatibility_record_count": 0, "eligible": True,
            "lease_spans": [{"first_record_index": 0, "record_count": 1,
                             "owns_lease": True, "final_lease_span": True}],
        }
    window_inspection["cpu_window_plan"] = {
        "schema": "dxmt9.render_tape.cpu_window_plan.v1", "page_size": 4096,
        "bootstrap_state_only": True, "sources": [window_row(1), window_row(3)],
    }
    window_result = copy.deepcopy(result)
    window_result.update(
        max_pending=2, completion="before-next-work",
        input_counts={"events": 4, "objects": 2, "mutations": 0,
                      "chunks": 2, "records": 2, "draws": 2},
        window_runtime={"page_size": 4096, "offload": False,
                        "cpu_ready_arena": False, "workers_disabled": True},
        hold_safe_buffer_mutation=False)
    window_iteration = window_result["iterations"][0]
    window_iteration.update(
        input_counts=window_result["input_counts"], command_count=2, draw_count=2,
        direct_draws=2, submitted=2, completed=2, reclaimed=2,
        resources_created=2, resources_released=2, pending_peak=2,
        deferred_releases=2, destroys_with_pending=1,
        retained_snapshots_after_destroy=0,
        destroy_windows=[{"event": 2, "pending_submissions": [10],
                          "owners_checked": 1, "retention_valid": True}],
        draw_observations=[
            {"ordinal": 1, "publication_event": 1,
             "cumulative_semantic_digest": "1"},
            {"ordinal": 2, "publication_event": 3,
             "cumulative_semantic_digest": "1"},
        ],
        trace=[
            dict(window_iteration["trace"][0], submission=10, first_event=1,
                 last_event=1, event=1, event_submission=1, released_at_event=4),
            dict(window_iteration["trace"][0], submission=20, source=2,
                 source_generation=2, storage_first_page=1, first_event=3,
                 last_event=3, event=3, event_submission=2, released_at_event=4),
        ])
    window_options = dict(
        repeat=1, route="production-slot", completion="before-next-work",
        require_path=True, resource_backend="cpu-metal-resources",
        publication="per-command", input_inspection=window_inspection,
        grammar="general-frame", max_pending=2)
    assert module.cpu_experiment_accepts(window_result, **window_options)
    forged_window = copy.deepcopy(window_result)
    forged_window["iterations"][0]["destroy_windows"][0]["pending_submissions"] = [1]
    assert not module.cpu_experiment_accepts(forged_window, **window_options)
    shallow_window = copy.deepcopy(window_result)
    shallow_window["iterations"][0]["pending_peak"] = 1
    assert not module.cpu_experiment_accepts(shallow_window, **window_options)
    for field, value in (("window_runtime", {"page_size": 4096, "offload": True,
                                               "cpu_ready_arena": False,
                                               "workers_disabled": True}),
                         ("hold_safe_buffer_mutation", True)):
        broken = copy.deepcopy(window_result)
        broken[field] = value
        assert not module.cpu_experiment_accepts(broken, **window_options), field
    for field, mutate in (
        ("missing-plan", lambda value: value.pop("cpu_window_plan")),
        ("stale-plan-row", lambda value: value["cpu_window_plan"]["sources"].append(
            window_row(5))),
        ("forged-eligible", lambda value: value["cpu_window_plan"]["sources"][0].__setitem__(
            "eligible", False)),
    ):
        broken_inspection = copy.deepcopy(window_inspection)
        mutate(broken_inspection)
        assert not module.cpu_experiment_accepts(
            window_result, **dict(window_options, input_inspection=broken_inspection)), field

    # An ineligible pending source is never paired with an eligible incoming
    # source.  The next two eligible sources may overlap after that drain.
    interleaved_rows = [window_row(event) for event in (1, 2, 3, 4)]
    interleaved_rows[1]["eligible"] = False
    interleaved_schedule = module._cpu_window_schedule(
        [5, 5, 5, 5, 7], interleaved_rows)
    assert interleaved_schedule is not None
    releases, windows, peak, deferred, destroys = interleaved_schedule
    assert releases == {1: 2, 2: 3, 3: 5, 4: 5}
    assert windows == [] and peak == 2 and deferred == 4 and destroys == 0
    forged_rows = copy.deepcopy(interleaved_rows)
    forged_rows[1]["eligible"] = True
    forged_schedule = module._cpu_window_schedule([5, 5, 5, 5, 7], forged_rows)
    assert forged_schedule is not None
    assert forged_schedule[0] != releases

    # The public compare helper must place the selected grammar on the child
    # command; this uses no provider or native executable.
    with tempfile.TemporaryDirectory(prefix="dxmt9-cpu-cli-forward-") as root:
        output = pathlib.Path(root) / "child"
        with mock.patch.object(
            module.subprocess, "run",
            return_value=subprocess.CompletedProcess([], 1, "", ""),
        ) as invoked:
            module._cpu_compare_child(
                bundle=pathlib.Path(root) / "bundle", output=output,
                provider=pathlib.Path(root) / "provider",
                validator=pathlib.Path(root) / "validator",
                completion="before-next-work", repeat=1, publication="per-command",
                resource_backend="cpu-metal-resources", max_pending=32,
                require_backing_oracle=False, timeout_seconds=1,
                route="production-slot", grammar="general-frame")
            command = invoked.call_args.args[0]
            assert command[command.index("--grammar") + 1] == "general-frame"
            assert command[command.index("--completion") + 1] == "before-next-work"



def run(*args):
    result = subprocess.run(args, text=True, capture_output=True, timeout=30)
    if result.returncode:
        raise AssertionError((args, result.returncode, result.stdout, result.stderr))
    return result


def cpu_validation_environment_contract(module, root, bundle, provider, validator, evidence):
    """Test provenance/acceptance, not Metal's behavior under validation.

    The provider double returns an already checked real native receipt. It
    deliberately avoids activating GPU validation merely to test the wrapper.
    """
    original_run = subprocess.run
    for label, api, shader, diagnostic in (
            ("api", "1", "0", True),
            ("shader-unknown", "0", "unknown", True),
            ("disabled", "0", "0", False),
            ("empty", "", "", False)):
        output = root / ("validation-environment-" + label)
        args = module.argparse.Namespace(
            bundle=bundle, provider=provider, validator=validator,
            output_dir=output, repeat=4, grammar="bounded", timeout_seconds=30,
            max_pending=32, resource_backend="cpu-no-gpu", completion_plan=None,
            completion="after-destroy", publication="per-command", fault="none",
            replay_route="production-slot", require_path=True, require_backing_oracle=False)
        def provider_double(command, **kwargs):
            if pathlib.Path(command[0]).resolve() != provider.resolve():
                return original_run(command, **kwargs)
            assert kwargs["env"]["MTL_DEBUG_LAYER"] == api
            assert kwargs["env"]["MTL_SHADER_VALIDATION"] == shader
            return subprocess.CompletedProcess(command, 0, json.dumps(evidence), "")
        with mock.patch.dict(os.environ, {
                "MTL_DEBUG_LAYER": api, "MTL_SHADER_VALIDATION": shader,
                "DXMT9_TEST_CAPTURE_ENV": "observable",
                "CPU_TAPE_UNRELATED_SECRET": "must-not-be-recorded"}), \
                mock.patch.object(module.subprocess, "run", side_effect=provider_double):
            assert module.cpu_fixed_input_experiment(args) == int(diagnostic)
        receipt = json.loads((output / "result.json").read_text())
        assert receipt["execution_valid"] is True
        assert receipt["accepted"] is not diagnostic
        assert receipt["diagnostic"] is diagnostic
        assert receipt["metal_validation_diagnostic"] is diagnostic
        assert receipt["renderer_environment"]["MTL_DEBUG_LAYER"] == api
        assert receipt["renderer_environment"]["MTL_SHADER_VALIDATION"] == shader
        assert receipt["renderer_environment"]["DXMT9_TEST_CAPTURE_ENV"] == "observable"
        assert "must-not-be-recorded" not in (output / "result.json").read_text()
        assert (output / "provider.stdout").is_file()


def native_general_mipmap_receipt_tests(module, provider, validator, runner, root):
    source, bundle = root / "mipmap-source", root / "mipmap-bundle"
    run(str(provider), "--write-general-cpu-mipmap-fixture", str(source))
    pack = [sys.executable, str(runner), "pack", "--events", str(source / "events.bin"),
            "--output-dir", str(bundle), "--validator", str(validator)]
    for blob in sorted(source.glob("blob-*.bin")):
        pack.extend(("--blob", str(blob)))
    run(*pack)
    _, events, refs, _, _, _ = module.load_bundle(bundle)
    inspection = module.run_validator(validator, "inspect", events, refs)
    assert module._cpu_general_mipmap_count(inspection) == 1, inspection
    diagnostic_output = root / "mipmap-diagnostic"
    run(sys.executable, str(runner), "cpu-experiment", str(bundle),
        "--provider", str(provider), "--validator", str(validator),
        "--grammar", "general-frame", "--resource-backend", "cpu-metal-resources",
        "--completion", "after-command", "--max-pending", "1", "--repeat", "2",
        "--run-mode", "diagnose", "--policy-lane", "--output-dir", str(diagnostic_output))
    diagnostic_receipt = json.loads((diagnostic_output / "result.json").read_text())
    assert diagnostic_receipt["execution_valid"] is True
    assert diagnostic_receipt["diagnostic"] is True
    assert diagnostic_receipt["accepted"] is False
    assert diagnostic_receipt["semantic_qualified"] is False
    for row in diagnostic_receipt["result"]["iterations"]:
        assert module._cpu_diagnostic_accounting_accepts(row)
        assert not any(row["phase_ns"].values()) and row["full_oracle_calls"] == 0
    expected_digest = None
    for completion in ("after-command", "before-next-work"):
        output = root / ("mipmap-" + completion)
        run(sys.executable, str(runner), "cpu-compare", str(bundle),
            "--provider", str(provider), "--validator", str(validator),
            "--grammar", "general-frame", "--resource-backend", "cpu-metal-resources",
            "--completion", completion, "--max-pending", "1", "--repeat", "4",
            "--output-dir", str(output))
        receipt = json.loads((output / "result.json").read_text())
        assert receipt["accepted"] is True, receipt
        for route in ("reference", "candidate"):
            for iteration in receipt[route]["result"]["iterations"]:
                assert iteration["mipmap_command_count"] == 1
                assert iteration["physical_texture_oracle"] is False
                retained = iteration["retained_mipmap_textures_after_destroy"]
                assert (retained > 0) == (completion == "before-next-work")
                digest = iteration["semantic_digest"]
                if expected_digest is None:
                    expected_digest = digest
                assert digest == expected_digest


def main():
    provider, validator, runner = map(pathlib.Path, sys.argv[1:])
    repo_root = pathlib.Path(__file__).resolve().parents[2]
    python_launcher = repo_root / "scripts" / "run_python.sh"
    spec = importlib.util.spec_from_file_location("tape_runner", runner)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    cpu_input_staging_tests(module)
    rejected_staging_retention_tests(module)
    native_free_general_frame_contract_tests(module)
    def inspect_bundle(bundle):
        _, events, refs, _, _, _ = module.load_bundle(bundle)
        return module.run_validator(validator, "inspect", events, refs)
    with tempfile.TemporaryDirectory(prefix="dxmt9-cpu-tape-") as temporary:
        root = pathlib.Path(temporary)
        native_general_mipmap_receipt_tests(module, provider, validator, runner, root)
        source, bundle = root / "source", root / "bundle"
        run(str(provider), "--write-completion-fixture", str(source))
        pack = [sys.executable, str(runner), "pack", "--events", str(source / "events.bin"),
                "--output-dir", str(bundle), "--validator", str(validator)]
        for blob in sorted(source.glob("*.bin")):
            if blob.name != "events.bin":
                pack.extend(("--blob", str(blob)))
        run(*pack)
        plain_inspection = inspect_bundle(bundle)
        # The fixture has draw publication at event 5 and Present at event 8;
        # event 6 only unbinds state and publishes no synthetic source.
        for route in ("compatibility", "production-slot"):
            plan = {"schema": "dxmt9.render_tape.completion_plan.v1",
                    "events_sha256": hashlib.sha256((source / "events.bin").read_bytes()).hexdigest(),
                    "events_count": 9, "repeat": 4, "replay_route": route,
                    "submits": [{"event": 5, "submission": 1},
                                {"event": 8, "submission": 1}],
                    "releases": [{"at_event": 7, "event": 5, "submission": 1},
                                 {"at_event": 9, "event": 8, "submission": 1}]}
            options = dict(events_sha256=plan["events_sha256"], repeat=4, route=route)
            encoded = module.encode_cpu_completion_plan(plan, **options)
            assert len(encoded) == 720
            assert encoded[48:80].hex() == plan["events_sha256"]
            for field, value in (("repeat", True), ("events_count", 0),
                                 ("events_sha256", "00" * 32),
                                 ("releases", plan["releases"][:1]),
                                 ("submits", [{"event": 5, "submission": 2},
                                              {"event": 8, "submission": 1}])):
                broken = copy.deepcopy(plan)
                broken[field] = value
                try:
                    module.encode_cpu_completion_plan(broken, **options)
                except ValueError:
                    pass
                else:
                    raise AssertionError(("accepted malformed completion plan", field))
            plan_path = root / (route + "-plan.json")
            plan_path.write_text(json.dumps(plan))
            output = root / (route + "-external")
            run(sys.executable, str(runner), "cpu-experiment", str(bundle),
                "--provider", str(provider), "--validator", str(validator),
                "--replay-route", route, "--completion-plan", str(plan_path),
                "--repeat", "4", "--require-path", "--output-dir", str(output))
            receipt = json.loads((output / "result.json").read_text())
            assert receipt["accepted"] is True, receipt
            assert receipt["result"]["completion"] == "external"
            assert (output / "completion-plan.bin").read_bytes() == encoded
            for iteration in receipt["result"]["iterations"]:
                assert [(row["event"], row["event_submission"], row["released_at_event"])
                        for row in iteration["trace"]] == [(5, 1, 7), (8, 1, 9)]
        reference = None
        for route in ("compatibility", "production-slot"):
            for completion in ("after-command", "after-destroy"):
                output = root / (route + "-" + completion)
                run(sys.executable, str(runner), "cpu-experiment", str(bundle),
                    "--provider", str(provider), "--validator", str(validator),
                    "--replay-route", route, "--completion", completion,
                    "--repeat", "4", "--require-path", "--output-dir", str(output))
                receipt = json.loads((output / "result.json").read_text())
                assert receipt["accepted"] is True and receipt["gpu_evidence"] is False
                evidence = receipt["result"]
                signatures = [item["semantic_digest"] for item in evidence["iterations"]]
                for iteration in evidence["iterations"]:
                    assert [(row["event"], row["released_at_event"])
                            for row in iteration["trace"]] == (
                        [(5, 7), (8, 9)] if completion == "after-destroy"
                        else [(5, 5), (8, 8)])
                assert len(set(signatures)) == 1
                if reference is not None:
                    assert reference == signatures[0], (route, completion, reference, signatures)
                reference = signatures[0]
                options = dict(repeat=4, route=route, completion=completion, require_path=True,
                               input_inspection=plain_inspection)
                assert module.cpu_experiment_accepts(evidence, **options)
                for field in ("completed", "resources_released", "reset_ok", "semantic_digest"):
                    broken = copy.deepcopy(evidence)
                    broken["iterations"][1][field] = False if field == "reset_ok" else -1
                    assert not module.cpu_experiment_accepts(broken, **options), field
                if completion == "after-destroy":
                    broken = copy.deepcopy(evidence)
                    broken["iterations"][0]["retained_before_completion"] = False
                    assert not module.cpu_experiment_accepts(broken, **options)
                broken = copy.deepcopy(evidence)
                broken["iterations"][1]["trace"][0]["iteration"] = 1
                assert not module.cpu_experiment_accepts(broken, **options)

                for field, value in (("storage_generation", 0),
                                     ("storage_page_count", 0),
                                     ("storage_first_page", -1),
                                     ("released_at_event", 1),
                                     ("released_at_event", 999),
                                     ("event", 999), ("event_submission", 2)):
                    broken = copy.deepcopy(evidence)
                    broken["iterations"][0]["trace"][0][field] = value
                    assert not module.cpu_experiment_accepts(broken, **options), field
                    del broken["iterations"][0]["trace"][0][field]
                    assert not module.cpu_experiment_accepts(broken, **options), field
                broken = copy.deepcopy(evidence)
                broken["completion_plan_sha256"] = "00" * 32
                assert not module.cpu_experiment_accepts(broken, **options)
        cpu_validation_environment_contract(module, root, bundle, provider, validator, evidence)
        # These are real adapter failure executions, not edited success JSON.
        for label, switches, failure in (
            ("submit-fault", ("--fault", "before-submit"), "fault-before-submit"),
            ("release-fault", ("--fault", "before-release"), "fault-before-release"),
            ("zero-budget", ("--max-pending", "0"), "max-pending-capacity-exhausted"),
        ):
            output = root / label
            failed = subprocess.run(
                [sys.executable, str(runner), "cpu-experiment", str(bundle),
                 "--provider", str(provider), "--validator", str(validator),
                 "--repeat", "2", "--output-dir", str(output), *switches],
                text=True, capture_output=True, timeout=30)
            assert failed.returncode == 1, (label, failed.stdout, failed.stderr)
            receipt = json.loads((output / "result.json").read_text())
            assert receipt["accepted"] is False
            assert receipt["result"]["status"] == "unresolved"
            prefix = receipt["result"]["iterations"]
            assert len(prefix) == 1 and prefix[0]["failure"] == failure, prefix
            assert prefix[0]["fault_triggered"] == (label != "zero-budget")
            assert receipt["result"]["phase_accounting"] == "exclusive-wall"
            assert receipt["result"]["clock"] == "steady_clock"
            assert module._cpu_native_time_valid(receipt["result"], prefix[0])
            phases = prefix[0]["phase_ns"]
            assert set(phases) == {"setup", "replay", "observe", "completion", "cleanup"}
            assert all(type(value) is int and value >= 0 for value in phases.values())
            assert sum(phases.values()) <= prefix[0]["elapsed_ns"]
        capacity_source, capacity_bundle = root / "capacity-source", root / "capacity-bundle"
        run(str(provider), "--write-capacity-continuation-fixture", str(capacity_source))
        run(sys.executable, str(runner), "pack", "--events",
            str(capacity_source / "events.bin"), "--blob", str(capacity_source / "blob.bin"),
            "--output-dir", str(capacity_bundle), "--validator", str(validator))
        capacity_inspection = inspect_bundle(capacity_bundle)
        capacity_reference = None
        for route in ("compatibility", "production-slot"):
            for completion in ("after-command", "after-destroy", "external"):
                switches = ["--completion", completion] if completion != "external" else []
                plan = None
                if completion == "external":
                    plan = {"schema": "dxmt9.render_tape.completion_plan.v1",
                            "events_sha256": hashlib.sha256(
                                (capacity_source / "events.bin").read_bytes()).hexdigest(),
                            "events_count": 10, "repeat": 4, "replay_route": route,
                            "submits": [{"event": 6, "submission": 1},
                                        {"event": 9, "submission": 1}],
                            "releases": [{"at_event": 8, "event": 6, "submission": 1},
                                         {"at_event": 10, "event": 9, "submission": 1}]}
                    plan_path = root / ("capacity-" + route + "-plan.json")
                    plan_path.write_text(json.dumps(plan))
                    switches = ["--completion-plan", str(plan_path)]
                output = root / ("capacity-" + route + "-" + completion)
                run(sys.executable, str(runner), "cpu-experiment", str(capacity_bundle),
                    "--provider", str(provider), "--validator", str(validator),
                    "--replay-route", route, "--publication", "draw-island",
                    "--repeat", "4", "--require-path", "--output-dir", str(output),
                    *switches)
                receipt = json.loads((output / "result.json").read_text())
                assert receipt["accepted"] is True, receipt
                evidence = receipt["result"]
                options = dict(repeat=4, route=route, completion=completion,
                               completion_plan=plan, require_path=True,
                               publication="draw-island",
                               input_inspection=capacity_inspection)
                assert module.cpu_experiment_accepts(evidence, **options)
                bounded_phase_receipt_tests(module, evidence, options)
                for iteration in evidence["iterations"]:
                    assert iteration["draw_count"] == 9
                    assert iteration["grouped_ranges"] == 1
                    assert iteration["grouped_events"] == 2
                    assert iteration["grouped_observed_draws"] == 9
                    assert iteration["grouped_publication_verified"] is True
                    assert iteration["capacity_rotated"] == 0
                    assert [(row["first_event"], row["last_event"], row["event"])
                            for row in iteration["trace"]] == [(5, 6, 6), (9, 9, 9)]
                    assert [row["released_at_event"] for row in iteration["trace"]] == (
                        [6, 9] if completion == "after-command" else [8, 10])
                    if route == "production-slot":
                        assert iteration["direct_draws"] == 7, iteration
                        assert iteration["compatibility_draws"] == 2, iteration
                        assert iteration["capacity_rejected"] == 1, iteration
                        assert iteration["populated_fallback"] == 1, iteration
                    if capacity_reference is not None:
                        assert iteration["semantic_digest"] == capacity_reference
                    capacity_reference = iteration["semantic_digest"]
                for field, value in (("grouped_publication_verified", False),
                                     ("grouped_observed_draws", 0),
                                     ("grouped_events", 3), ("capacity_rotated", 1)):
                    broken = copy.deepcopy(evidence)
                    broken["iterations"][0][field] = value
                    assert not module.cpu_experiment_accepts(broken, **options), field
                broken = copy.deepcopy(evidence)
                broken["iterations"][0]["trace"][0]["first_event"] = 4
                assert not module.cpu_experiment_accepts(broken, **options)
                broken = copy.deepcopy(evidence)
                broken["publication"] = "per-command"
                assert not module.cpu_experiment_accepts(broken, **options)

        # Execute boundary mistakes; editing receipts alone would not prove
        # that the runtime refuses to publish/release an incomplete island.
        early_plan = copy.deepcopy(plan)
        early_plan["submits"][0]["event"] = 5
        early_plan["releases"][0]["event"] = 5
        early_plan_path = root / "capacity-early-plan.json"
        early_plan_path.write_text(json.dumps(early_plan))
        for label, input_bundle, switches, expected_status in (
            ("capacity-per-command", capacity_bundle, [], "unsupported-grammar"),
            ("vacuous-group", bundle, ["--publication", "draw-island"],
             "vacuous-publication-plan"),
            ("early-group-submit", capacity_bundle,
             ["--publication", "draw-island", "--completion-plan", str(early_plan_path)],
             "unresolved"),
        ):
            output = root / label
            failed = subprocess.run(
                [sys.executable, str(runner), "cpu-experiment", str(input_bundle),
                 "--provider", str(provider), "--validator", str(validator),
                 "--replay-route", "production-slot", "--repeat", "4",
                 "--output-dir", str(output), *switches],
                text=True, capture_output=True, timeout=30)
            assert failed.returncode == 1, (label, failed.stdout, failed.stderr)
            receipt = json.loads((output / "result.json").read_text())
            assert receipt["accepted"] is False, receipt
            assert receipt["result"]["status"] == expected_status, receipt
            if label == "early-group-submit":
                prefix = receipt["result"]["iterations"]
                assert len(prefix) == 1 and prefix[0]["submitted"] == 0, prefix
                assert prefix[0]["failure"], prefix

        dynamic_source, dynamic_bundle = root / "dynamic-source", root / "dynamic-bundle"
        run(str(provider), "--write-dynamic-completion-fixture", str(dynamic_source))
        dynamic_pack = [sys.executable, str(runner), "pack", "--events",
                        str(dynamic_source / "events.bin"), "--output-dir",
                        str(dynamic_bundle), "--validator", str(validator)]
        for blob in sorted(dynamic_source.glob("blob_*.bin")):
            dynamic_pack += ["--blob", str(blob)]
        run(*dynamic_pack)
        dynamic_inspection = inspect_bundle(dynamic_bundle)
        dynamic_reference = None
        for route in ("compatibility", "production-slot"):
            for completion in ("after-command", "after-destroy"):
                output = root / ("dynamic-" + route + "-" + completion)
                completed = subprocess.run(
                    [sys.executable, str(runner), "cpu-experiment", str(dynamic_bundle),
                     "--provider", str(provider), "--validator", str(validator),
                     "--replay-route", route, "--completion", completion,
                     "--resource-backend", "cpu-metal-resources", "--require-backing-oracle",
                     "--repeat", "4", "--require-path", "--output-dir", str(output)],
                    text=True, capture_output=True, timeout=30)
                receipt = json.loads((output / "result.json").read_text())
                if receipt["provider_exit_code"] == 77:
                    return 77
                assert completed.returncode == 0 and receipt["accepted"] is True, receipt
                assert receipt["gpu_evidence"] is False
                evidence = receipt["result"]
                for iteration in evidence["iterations"]:
                    assert iteration["draw_count"] == 2
                    assert iteration["captured_A_required_retention"] == (completion == "after-destroy")
                    assert iteration["buffer_backing_oracle"] == "real-metal-cpu-visible"
                    assert [(row["event"], row["released_at_event"])
                            for row in iteration["trace"]] == (
                        [(5, 11), (7, 11), (10, 11)] if completion == "after-destroy"
                        else [(5, 5), (7, 7), (10, 10)])
                    if dynamic_reference is None:
                        dynamic_reference = iteration["semantic_digest"]
                    assert iteration["semantic_digest"] == dynamic_reference
                options = dict(repeat=4, route=route, completion=completion,
                               require_path=True, resource_backend="cpu-metal-resources",
                               require_backing_oracle=True,
                               input_inspection=dynamic_inspection)
                for field in ("buffer_backing_observed", "buffer_backing_oracle",
                              "buffer_backing_revision_advanced"):
                    broken = copy.deepcopy(evidence)
                    broken["iterations"][0][field] = False
                    assert not module.cpu_experiment_accepts(broken, **options)
            # Explicit event schedules must drive the same real backing oracle
            # as the presets; the delayed schedule retains A across Discard B.
            for delayed in (False, True):
                plan = {"schema": "dxmt9.render_tape.completion_plan.v1",
                        "events_sha256": hashlib.sha256(
                            (dynamic_source / "events.bin").read_bytes()).hexdigest(),
                        "events_count": 11, "repeat": 4, "replay_route": route,
                        "submits": [{"event": event, "submission": 1}
                                    for event in (5, 7, 10)],
                        "releases": [{"at_event": at, "event": event, "submission": 1}
                                     for event, at in zip(
                                         (5, 7, 10), (9, 9, 11) if delayed else (5, 7, 11))]}
                plan_path = root / f"dynamic-{route}-{delayed}-plan.json"
                plan_path.write_text(json.dumps(plan))
                output = root / f"dynamic-{route}-{delayed}-external"
                run(sys.executable, str(runner), "cpu-experiment", str(dynamic_bundle),
                    "--provider", str(provider), "--validator", str(validator),
                    "--replay-route", route, "--completion-plan", str(plan_path),
                    "--resource-backend", "cpu-metal-resources", "--require-backing-oracle",
                    "--repeat", "4", "--require-path", "--output-dir", str(output))
                receipt = json.loads((output / "result.json").read_text())
                assert receipt["accepted"] is True, receipt
                for iteration in receipt["result"]["iterations"]:
                    assert iteration["captured_A_required_retention"] == delayed
                    assert iteration["semantic_digest"] == dynamic_reference
                    assert iteration["buffer_backing_oracle"] == "real-metal-cpu-visible"
                    assert [(row["event"], row["released_at_event"])
                            for row in iteration["trace"]] == [
                                (row["event"], row["at_event"]) for row in plan["releases"]]
        # Three outstanding sources with budget two must fail on the third.
        # This catches accidentally reducing the pending count to a boolean.
        bounded_plan = copy.deepcopy(plan)
        for release in bounded_plan["releases"]:
            release["at_event"] = 11
        bounded_plan_path = root / "dynamic-pending-two-plan.json"
        bounded_plan_path.write_text(json.dumps(bounded_plan))
        bounded_output = root / "dynamic-pending-two"
        bounded = subprocess.run(
            [sys.executable, str(runner), "cpu-experiment", str(dynamic_bundle),
             "--provider", str(provider), "--validator", str(validator),
             "--replay-route", "production-slot", "--repeat", "4",
             "--resource-backend", "cpu-metal-resources", "--max-pending", "2",
             "--completion-plan", str(bounded_plan_path),
             "--output-dir", str(bounded_output)],
            text=True, capture_output=True, timeout=30)
        bounded_receipt = json.loads((bounded_output / "result.json").read_text())
        assert bounded.returncode == 1 and bounded_receipt["accepted"] is False
        bounded_prefix = bounded_receipt["result"]["iterations"]
        assert len(bounded_prefix) == 1, bounded_prefix
        assert bounded_prefix[0]["failure"] == "max-pending-capacity-exhausted"
        assert bounded_prefix[0]["submitted"] == 2
        assert bounded_prefix[0]["completed"] == 0

        rejected_output = root / "dynamic-without-metal"
        rejected = subprocess.run(
            [sys.executable, str(runner), "cpu-experiment", str(dynamic_bundle),
             "--provider", str(provider), "--validator", str(validator),
             "--output-dir", str(rejected_output)],
            text=True, capture_output=True, timeout=30)
        rejected_receipt = json.loads((rejected_output / "result.json").read_text())
        assert rejected.returncode == 1 and rejected_receipt["accepted"] is False
        assert rejected_receipt["result"]["reason"] == "dynamic-backend-required"
        managed_source, managed_bundle = root / "managed-source", root / "managed-bundle"
        run(str(provider), "--write-managed-allocation-failure-fixture", str(managed_source))
        managed_pack = [sys.executable, str(runner), "pack", "--events",
                        str(managed_source / "events.bin"), "--output-dir",
                        str(managed_bundle), "--validator", str(validator)]
        for blob in sorted(managed_source.glob("*.bin")):
            if blob.name != "events.bin":
                managed_pack += ["--blob", str(blob)]
        run(*managed_pack)
        managed_inspection = inspect_bundle(managed_bundle)
        managed_reference = None
        for route in ("compatibility", "production-slot"):
            for completion in ("after-command", "after-destroy"):
                for fault in ("none", "managed-allocation"):
                    output = root / f"managed-{route}-{completion}-{fault}"
                    run(sys.executable, str(runner), "cpu-experiment", str(managed_bundle),
                        "--provider", str(provider), "--validator", str(validator),
                        "--replay-route", route, "--completion", completion,
                        "--resource-backend", "cpu-metal-resources",
                        "--fault", fault, "--repeat", "4", "--require-path",
                        "--output-dir", str(output))
                    receipt = json.loads((output / "result.json").read_text())
                    diagnostic = fault == "managed-allocation"
                    assert receipt["accepted"] is not diagnostic, receipt
                    assert receipt["diagnostic"] is diagnostic
                    assert receipt["fault_oracle_passed"] is diagnostic
                    assert receipt["forced_environment"]["DXMT9_OFFLOAD_COMMIT_REPLAY"] == (
                        "1" if diagnostic else "0")
                    for iteration in receipt["result"]["iterations"]:
                        if managed_reference is None:
                            managed_reference = iteration["semantic_digest"]
                        assert iteration["semantic_digest"] == managed_reference
                    if diagnostic:
                        options = dict(repeat=4, route=route, completion=completion,
                                       require_path=True, fault=fault,
                                       resource_backend="cpu-metal-resources",
                                       input_inspection=managed_inspection)
                        for field, value in (("fault_triggered", False),
                                             ("fault_preeffect_ownership_unchanged", False),
                                             ("fault_retry_succeeded", False),
                                             ("fault_receipt", "unproven"),
                                             ("fault_mutation_applies", 0),
                                             ("fault_mutation_applies", 2),
                                             ("fault_mutation_applies", True)):
                            broken = copy.deepcopy(receipt["result"])
                            broken["iterations"][0][field] = value
                            assert not module.cpu_experiment_accepts(broken, **options)
        wrong_fault = root / "managed-fault-on-default-buffer"
        rejected = subprocess.run(
            [sys.executable, str(runner), "cpu-experiment", str(bundle),
             "--provider", str(provider), "--validator", str(validator),
             "--resource-backend", "cpu-metal-resources",
             "--fault", "managed-allocation", "--output-dir", str(wrong_fault)],
            text=True, capture_output=True, timeout=30)
        receipt = json.loads((wrong_fault / "result.json").read_text())
        assert rejected.returncode == 1 and receipt["accepted"] is False
        assert receipt["fault_oracle_passed"] is False
        assert receipt["result"]["reason"] == "managed-allocation-fault-requires-managed-buffer"

        # The end-user compare command runs both routes under one parent
        # transaction. It is exercised through run_python.sh, matching the
        # repository's public uv-owned Python invocation.
        for completion in ("after-command", "after-destroy"):
            output = root / ("compare-" + completion)
            compare = run(
                str(python_launcher), str(runner), "cpu-compare", str(bundle),
                "--provider", str(provider), "--validator", str(validator),
                "--completion", completion, "--repeat", "4", "--output-dir",
                str(output),
            )
            assert compare.stdout.count("\n") == 1, compare.stdout
            compare_receipt = json.loads((output / "result.json").read_text())
            assert compare_receipt["accepted"] is True, compare_receipt
            assert compare_receipt["timing_accepted"] is None
            assert compare_receipt["input"]["authenticated_once"] is True
            assert compare_receipt["comparison"]["accepted"] is True
            assert compare_receipt["comparison"]["first_differing_iteration"] is None
            assert compare_receipt["comparison"]["first_differing_draw"] is None
            assert compare_receipt["comparison"]["physical_layout_compared"] is False
            for child in ("reference", "candidate"):
                assert (output / child / "result.json").is_file()
                assert compare_receipt[child] == compare_receipt["children"][child]
            options = dict(
                repeat=4, completion=completion, publication="per-command",
                resource_backend="cpu-no-gpu", input_inspection=plain_inspection,
            )
            assert module.cpu_compare_accepts(
                compare_receipt["reference"], compare_receipt["candidate"],
                reference_input_components=compare_receipt["input"]["components"],
                candidate_input_components=compare_receipt["input"]["components"],
                **options,
            )
            physical_variant = copy.deepcopy(compare_receipt["candidate"])
            physical_iteration = physical_variant["result"]["iterations"][0]
            physical_iteration["command_count"] += 7
            physical_report = module.compare_cpu_experiment_results(
                compare_receipt["reference"], physical_variant,
                reference_input_components=compare_receipt["input"]["components"],
                candidate_input_components=compare_receipt["input"]["components"],
                **options,
            )
            assert physical_report["accepted"] is True, physical_report
            assert (physical_report["iterations"][0]["reference_physical_counts"]
                    != physical_report["iterations"][0]["candidate_physical_counts"])
            fabricated_draw = copy.deepcopy(compare_receipt["candidate"])
            fabricated_draw["result"]["iterations"][0]["direct_draws"] += 1
            fabricated_report = module.compare_cpu_experiment_results(
                compare_receipt["reference"], fabricated_draw,
                reference_input_components=compare_receipt["input"]["components"],
                candidate_input_components=compare_receipt["input"]["components"],
                **options,
            )
            assert fabricated_report["accepted"] is False, fabricated_report
            assert any(
                failure["name"] == "candidate-child-rejected"
                for failure in fabricated_report["failures"]
            ), fabricated_report
            draw_corruption = copy.deepcopy(compare_receipt["candidate"])
            observation = draw_corruption["result"]["iterations"][0]["draw_observations"][0]
            observation["cumulative_semantic_digest"] = str(
                int(observation["cumulative_semantic_digest"]) + 1
            )
            draw_report = module.compare_cpu_experiment_results(
                compare_receipt["reference"], draw_corruption,
                reference_input_components=compare_receipt["input"]["components"],
                candidate_input_components=compare_receipt["input"]["components"],
                **options,
            )
            assert draw_report["accepted"] is False
            assert draw_report["first_differing_draw"] == 1, draw_report
            assert any(
                failure["name"] == "draw-observation-mismatch"
                for failure in draw_report["failures"]
            ), draw_report
            for label, mutate in (
                ("boolean-ordinal", lambda value: value["result"]["iterations"][0][
                    "draw_observations"][0].__setitem__("ordinal", True)),
                ("nonexistent-publication-event", lambda value: value["result"][
                    "iterations"][0]["draw_observations"][0].__setitem__(
                        "publication_event", 999999)),
                ("noncanonical-digest", lambda value: value["result"]["iterations"][0][
                    "draw_observations"][0].__setitem__(
                        "cumulative_semantic_digest", "01")),
                ("uint64-overflow-digest", lambda value: value["result"]["iterations"][0][
                    "draw_observations"][0].__setitem__(
                        "cumulative_semantic_digest", "18446744073709551616")),
            ):
                broken = copy.deepcopy(compare_receipt["candidate"])
                mutate(broken)
                report = module.compare_cpu_experiment_results(
                    compare_receipt["reference"], broken,
                    reference_input_components=compare_receipt["input"]["components"],
                    candidate_input_components=compare_receipt["input"]["components"],
                    **options,
                )
                assert report["accepted"] is False, (label, report)
                assert any(
                    failure["name"] == "draw-observations-invalid"
                    for failure in report["failures"]
                ), (label, report)
            huge_range = copy.deepcopy(compare_receipt["candidate"])
            huge_range["result"]["iterations"][0]["trace"][0]["last_event"] = 10 ** 100
            huge_report = module.compare_cpu_experiment_results(
                compare_receipt["reference"], huge_range,
                reference_input_components=compare_receipt["input"]["components"],
                candidate_input_components=compare_receipt["input"]["components"],
                **options,
            )
            assert huge_report["accepted"] is False, huge_report

            wrapper_failure = copy.deepcopy(compare_receipt["candidate"])
            wrapper_failure["compare_wrapper_exit_code"] = 1
            wrapper_report = module.compare_cpu_experiment_results(
                compare_receipt["reference"], wrapper_failure,
                reference_input_components=compare_receipt["input"]["components"],
                candidate_input_components=compare_receipt["input"]["components"],
                **options,
            )
            assert any(
                failure["name"] == "candidate-wrapper-failed"
                for failure in wrapper_report["failures"]
            ), wrapper_report
            wrapper_failure["compare_wrapper_exit_code"] = False
            assert not module.cpu_compare_accepts(
                compare_receipt["reference"], wrapper_failure, **options)
            # Nested valid evidence and accepted=true cannot override failed
            # or malformed root execution metadata, on either route.
            for child_name in ("reference", "candidate"):
                for field, value in (("execution_valid", False),
                                     ("execution_valid", 1),
                                     ("execution_valid", None),
                                     ("provider_exit_code", 1),
                                     ("provider_exit_code", False),
                                     ("provider_exit_code", None)):
                    children = {name: compare_receipt[name]
                                for name in ("reference", "candidate")}
                    children[child_name] = copy.deepcopy(children[child_name])
                    children[child_name][field] = value
                    assert not module.cpu_compare_accepts(**children, **options), (
                        child_name, field, value)
            for label, mutate in (
                ("semantic-digest", lambda value: value["result"]["iterations"][1].__setitem__(
                    "semantic_digest", "f" * 64)),
                ("draw-count", lambda value: value["result"]["iterations"][1].__setitem__(
                    "draw_count", value["result"]["iterations"][1]["draw_count"] + 1)),
                ("reset", lambda value: value["result"]["iterations"][1].__setitem__(
                    "reset_ok", False)),
                ("input", lambda value: value.__setitem__("input_inspection", {"valid": False})),
                ("nonexecuted-route", lambda value: value["result"].__setitem__(
                    "replay_route", "compatibility")),
            ):
                broken = copy.deepcopy(compare_receipt["candidate"])
                mutate(broken)
                report = module.compare_cpu_experiment_results(
                    compare_receipt["reference"], broken,
                    reference_input_components=compare_receipt["input"]["components"],
                    candidate_input_components=compare_receipt["input"]["components"],
                    **options,
                )
                assert report["accepted"] is False, (label, report)
                assert report["failures"], (label, report)

        # A fake no-GPU candidate with only receipt-level success cannot pass:
        # the comparator still applies the strict child grammar and route gate.
        fake = copy.deepcopy(compare_receipt["candidate"])
        fake["accepted"] = True
        fake["result"] = {
            "schema": "dxmt9.render_tape.cpu_experiment.v1",
            "backend": "cpu-no-gpu", "status": "complete",
            "replay_route": "production-slot", "repeat": 4,
            "completion": "after-destroy", "publication": "per-command",
            "fault": "none", "immutable_input_owned": True,
            "deterministic": True, "iterations": [],
        }
        report = module.compare_cpu_experiment_results(
            compare_receipt["reference"], fake, repeat=4,
            completion="after-destroy", publication="per-command",
            resource_backend="cpu-no-gpu", input_inspection=plain_inspection,
            reference_input_components=compare_receipt["input"]["components"],
            candidate_input_components=compare_receipt["input"]["components"],
        )
        assert report["accepted"] is False and report["failures"], report
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
