#!/usr/bin/env python3
"""R8.7–8.9: actual native policy qualification before timing-only execution."""

import json
import hashlib
import pathlib
import subprocess
import sys
import tempfile


def run(*command, expected=0):
    result = subprocess.run(command, capture_output=True, text=True, timeout=90)
    assert result.returncode == expected, (command, result.returncode,
                                           result.stdout, result.stderr)
    return result


def read(path):
    return json.loads(path.read_text())


def main():
    provider, validator, runner = (pathlib.Path(value).resolve() for value in sys.argv[1:4])
    repo = runner.parents[2]
    policies = repo / "experiments/render-tape-policies"
    with tempfile.TemporaryDirectory(prefix="dxmt9-cpu-policy-cli-") as temporary:
        root = pathlib.Path(temporary)
        source, bundle = root / "source", root / "bundle"
        run(str(provider), "--write-general-cpu-fixture", str(source))
        pack = [sys.executable, str(runner), "pack", "--events", str(source / "events.bin"),
                "--output-dir", str(bundle), "--validator", str(validator)]
        for blob in sorted(source.glob("*.bin")):
            if blob.name != "events.bin":
                pack += ["--blob", str(blob)]
        run(*pack)
        original_events = (bundle / "events.bin").read_bytes()
        for label, reference, candidate in (
            ("aa", "compatibility-exact", "compatibility-exact"),
            ("batch", "compatibility-exact", "compatibility-batch-4"),
            ("capacity", "production-exact", "production-headroom-5m"),
        ):
            output = root / label
            run(sys.executable, str(runner), "cpu-policy-compare", str(bundle),
                "--provider", str(provider), "--validator", str(validator),
                "--reference-policy", str(policies / (reference + ".json")),
                "--candidate-policy", str(policies / (candidate + ".json")),
                "--repeat", "2", "--warmup", "1", "--output-dir", str(output))
            report = read(output / "result.json")
            assert report["accepted"] is True, report
            for side in ("reference", "candidate"):
                verified = read(output / "verify" / side / "result.json")
                measured = read(output / "measure" / side / "result.json")
                assert verified["accepted"] is True
                assert measured["accepted"] is False
                assert measured["execution_valid"] is True
                assert measured["semantic_qualified"] is False
                assert verified["input_components"] == measured["input_components"]
                assert len(verified["result"]["iterations"]) == 2
                assert len(measured["result"]["iterations"]) == 3
                summary = report["measurement"][side]
                assert summary["accepted"] is True and summary["iterations"] == 2
                assert summary["measured_indices"] == [1, 2]
                assert summary["qualification_sha256"] == hashlib.sha256(
                    (output / summary["qualification_receipt"]).read_bytes()).hexdigest()
                assert summary["sha256"] == hashlib.sha256(
                    (output / summary["receipt"]).read_bytes()).hexdigest()
                assert (output / (side + "-policy.json")).is_file()
                for row in measured["result"]["iterations"]:
                    assert row["complete"] and row["reset_ok"]
                    assert row["full_oracle_calls"] == 0
                    assert row["semantic_digest"] == "0"
                    assert row["draw_observations"] == []
                    assert not row["logical_resource_oracle_complete"]
                    assert not any(row["phase_ns"].values())
            assert (bundle / "events.bin").read_bytes() == original_events

        # Rejection is before output creation, not a silent baseline run.
        malformed = root / "invalid.json"
        invalid = read(policies / "compatibility-exact.json")
        invalid["draw_command_limit"] = 1
        malformed.write_text(json.dumps(invalid))
        output = root / "invalid-output"
        result = subprocess.run(
            [sys.executable, str(runner), "cpu-policy-compare", str(bundle),
             "--provider", str(provider), "--validator", str(validator),
             "--reference-policy", str(malformed),
             "--candidate-policy", str(policies / "compatibility-exact.json"),
             "--output-dir", str(output)], capture_output=True, text=True, timeout=30)
        assert result.returncode != 0 and not output.exists()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
