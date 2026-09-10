#!/usr/bin/env python3
"""Qualify real source projection and preserve rejected child evidence."""

import hashlib
import importlib.util
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
from unittest import mock


def main():
    provider, validator, runner = map(pathlib.Path, sys.argv[1:])

    def run(*args, succeeds=True):
        result = subprocess.run(list(map(str, args)), capture_output=True,
                                text=True, timeout=60)
        if (result.returncode == 0) != succeeds:
            raise AssertionError(result.stderr[-2000:] + result.stdout[-2000:])
        return result

    with tempfile.TemporaryDirectory(prefix="dxmt9-cpu-source-projection-") as temporary:
        root = pathlib.Path(temporary)
        source, bundle = root / "source", root / "bundle"
        run(provider, "--write-general-cpu-fixture", source)
        pack = [sys.executable, runner, "pack", "--events", source / "events.bin",
                "--validator", validator, "--output-dir", bundle]
        for blob in sorted(source.glob("blob-*.bin")):
            pack.extend(("--blob", blob))
        run(*pack)
        output = root / "projection"
        command = [sys.executable, runner, "cpu-source-project", bundle,
                   "--provider", provider, "--validator", validator]
        run(*command, "--output-dir", output)
        receipt = json.loads((output / "result.json").read_text())
        projection_bytes = (output / "source-projection.json").read_bytes()
        projection = json.loads(projection_bytes)
        assert receipt["accepted"] is True
        assert receipt["source_projection_sha256"] == hashlib.sha256(projection_bytes).hexdigest()
        assert receipt["source_count"] == 3
        assert projection["repeat"] == 2
        assert projection["accepted"] is True
        assert projection["loaded_images"]
        assert projection["reference_receipt_sha256"] == hashlib.sha256(
            (output / "reference" / "result.json").read_bytes()).hexdigest()
        definitions = projection["input_inspection"]["buffer_definitions"]
        expected = sorted([row["identity"] for row in definitions],
                          key=lambda row: (row["kind"], row["generation"], int(row["object_id"])))
        assert len(expected) == 2
        assert [row["buffer_closure"] for row in projection["sources"]] == [expected, expected, []]
        assert [row["event"] for row in projection["sources"]] == [
            index + 1 for index in projection["input_inspection"]["command_event_indices"]]

        invalid = root / "invalid-repeat"
        run(*command, "--repeat", "1", "--output-dir", invalid, succeeds=False)
        assert not invalid.exists()
        failed = root / "failed-reference"
        run(sys.executable, runner, "cpu-source-project", bundle,
            "--provider", "/usr/bin/false", "--validator", validator,
            "--output-dir", failed, succeeds=False)
        rejected = json.loads((failed / "result.json").read_text())
        assert rejected["accepted"] is False
        assert rejected["source_count"] == 0
        assert (failed / "reference" / "provider.stdout").is_file()
        assert (failed / "source-projection.json").is_file()

        corrupt = root / "corrupt-input"
        shutil.copytree(bundle, corrupt)
        (corrupt / "manifest.json").write_text("{invalid json")
        rejected_input = root / "rejected-input"
        run(sys.executable, runner, "cpu-source-project", corrupt,
            "--provider", provider, "--validator", validator,
            "--output-dir", rejected_input, succeeds=False)
        rejected = json.loads((rejected_input / "source-projection.json").read_text())
        assert rejected["accepted"] is False and rejected["sources"] == []
        assert rejected["input_components"] is None
        assert "cannot read render tape manifest" in rejected["setup_error"]
        assert (rejected_input / "input" / "manifest.json").read_text() == "{invalid json"

        rejected_validator = root / "rejected-validator"
        run(sys.executable, runner, "cpu-source-project", bundle,
            "--provider", provider, "--validator", "/usr/bin/false",
            "--output-dir", rejected_validator, succeeds=False)
        rejected = json.loads((rejected_validator / "source-projection.json").read_text())
        assert rejected["accepted"] is False and rejected["sources"] == []
        assert "render tape validate failed" in rejected["setup_error"]

        # Reuse a real accepted receipt, changing only its inspection. Neither
        # valid input digests nor a valid native result may mask this mismatch.
        sys.path.insert(0, str(runner.resolve().parent))
        spec = importlib.util.spec_from_file_location("cpu_projection_runner", runner)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        child = json.loads((output / "reference" / "result.json").read_text())
        child["compare_wrapper_exit_code"] = 0
        child["input_inspection"] = {"different": True}
        mismatch = root / "inspection-mismatch"
        args = module.argparse.Namespace(bundle=bundle, provider=provider, validator=validator,
                                         output_dir=mismatch, repeat=2, timeout_seconds=60)
        with mock.patch.object(module, "_cpu_compare_child", return_value=child):
            assert module.cpu_source_project(args) == 1
        rejected = json.loads((mismatch / "source-projection.json").read_text())
        assert "projection-provenance-mismatch" in rejected["failures"]
        assert rejected["sources"] == []

        for route in ("compatibility", "production-slot"):
            candidate = root / route
            run(sys.executable, runner, "cpu-experiment", bundle,
                "--provider", provider, "--validator", validator,
                "--grammar", "general-frame", "--completion", "before-next-work",
                "--resource-backend", "cpu-metal-resources", "--max-pending", "1",
                "--repeat", "2", "--replay-route", route,
                "--source-projection", output, "--output-dir", candidate)
            result = json.loads((candidate / "result.json").read_text())
            assert result["accepted"] is True
            assert result["source_binding"]["projection_sha256"] == receipt["source_projection_sha256"]
            for iteration in result["result"]["iterations"]:
                assert iteration["deferred_buffer_mutations"] > 0
                assert iteration["deferred_discard_mutations"] > 0
                assert iteration["retained_buffers_after_mutation"] > 0
                assert iteration["source_oracle_checked_sources"] == 3
            assert module.cpu_experiment_accepts(
                result["result"], repeat=2, route=route, completion="before-next-work",
                require_path=False, resource_backend="cpu-metal-resources",
                input_inspection=result["input_inspection"], grammar="general-frame",
                source_binding=result["source_binding"])
            import copy
            for field, value in (("held_mutation_event", 1),
                                 ("held_mutation_identity", None),
                                 ("released_at_event", 1)):
                tampered = copy.deepcopy(result["result"])
                held = next(row for row in tampered["iterations"][0]["trace"]
                            if row["held_mutation_event"])
                held[field] = value
                assert not module.cpu_experiment_accepts(
                    tampered, repeat=2, route=route, completion="before-next-work",
                    require_path=False, resource_backend="cpu-metal-resources",
                    input_inspection=result["input_inspection"], grammar="general-frame",
                    source_binding=result["source_binding"])
            # A mutually agreeing pair is insufficient if both depart from
            # the immediate-completion reference used to make the oracle.
            for key, value in (("reference_semantic_digest", "0"),
                               ("reference_draw_digests", ["0", "0"])):
                binding = copy.deepcopy(result["source_binding"])
                binding[key] = value
                assert not module.cpu_experiment_accepts(
                    result["result"], repeat=2, route=route, completion="before-next-work",
                    require_path=False, resource_backend="cpu-metal-resources",
                    input_inspection=result["input_inspection"], grammar="general-frame",
                    source_binding=binding)

        tampered_projection = root / "tampered-projection"
        comparison = root / "compare"
        run(sys.executable, runner, "cpu-compare", bundle,
            "--provider", provider, "--validator", validator,
            "--grammar", "general-frame", "--completion", "before-next-work",
            "--resource-backend", "cpu-metal-resources", "--max-pending", "1",
            "--repeat", "2", "--source-projection", output, "--output-dir", comparison)
        comparison_result = json.loads((comparison / "result.json").read_text())
        assert comparison_result["accepted"] is True
        assert comparison_result["reference"]["source_binding"] == comparison_result["candidate"]["source_binding"]
        shutil.copytree(output, tampered_projection)
        (tampered_projection / "source-projection.json").write_text("{}")
        rejected_binding = root / "rejected-binding"
        run(sys.executable, runner, "cpu-experiment", bundle,
            "--provider", provider, "--validator", validator,
            "--grammar", "general-frame", "--completion", "before-next-work",
            "--resource-backend", "cpu-metal-resources", "--max-pending", "1",
            "--repeat", "2", "--source-projection", tampered_projection,
            "--output-dir", rejected_binding, succeeds=False)
        rejected = json.loads((rejected_binding / "result.json").read_text())
        assert rejected["accepted"] is False
        assert "source-projection-receipt-digest" in rejected["source_projection_rejected"]
        assert not (rejected_binding / "provider.stdout").exists()
    print("CPU source projection CLI: real closure, provenance, and failure preservation OK")


if __name__ == "__main__":
    main()
