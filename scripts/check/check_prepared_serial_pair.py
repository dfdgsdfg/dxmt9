"""Check same-capture prepared serial consumer qualification, not performance."""
import hashlib
import json
import pathlib
import re
import sys


def check_pair(reference: pathlib.Path, candidate: pathlib.Path) -> dict:
    receipts = [json.loads((root / "result.json").read_text())
                for root in (reference, candidate)]
    a, b = receipts
    for receipt in receipts:
        assert receipt["accepted"] and receipt["readback_artifacts_valid"]
        assert receipt["loaded_images_stable"] and not receipt["diagnostic"]
        result = receipt["result"]
        assert result["pixel_deterministic"] and result["status"] == "complete"
        assert result["reset"] == "cold-device"
        assert len(result["iterations"]) >= 2
    for field in ("provider", "validator", "input_components", "source_boundary_mode"):
        assert a[field] == b[field], field
    for field in ("source_plan_sha256", "loaded_images", "replay_route"):
        assert a["result"][field] == b["result"][field], field
    envs = [dict(r["renderer_environment"]) for r in receipts]
    assert envs[0].pop("DXMT9_PREPARED_SERIAL_READS") == "0"
    assert envs[1].pop("DXMT9_PREPARED_SERIAL_READS") == "1"
    assert envs[0] == envs[1], "other renderer environment differs"
    counter_values = []
    for root in (reference, candidate):
        stderr = (root / "provider.stderr").read_text()
        def counter(name):
            values = re.findall(r"\b" + name + r"=(\d+)", stderr)
            assert len(values) == 1, (name, values)
            return int(values[0])
        assert counter("gpu_command_buffer_errors") == 0
        counter_values.append(counter("prepared_serial_reads"))
    assert counter_values[0] == 0 and counter_values[1] > 0, "vacuous consumer gate"
    baseline = None
    artifacts = 0
    for root, receipt in zip((reference, candidate), receipts):
        for iteration in receipt["result"]["iterations"]:
            assert iteration["reset_ok"] and not iteration["warmup"]
            assert iteration["provider_result"]["status"] == "complete"
            for name in ("source_rgba", "output_rgba"):
                artifact = iteration["readback_artifacts"][name]
                relative = pathlib.Path(artifact["path"])
                assert not relative.is_absolute() and ".." not in relative.parts
                data = (root / "readback" / relative).read_bytes()
                assert len(data) == artifact["bytes"] and len(data) > 4
                assert hashlib.sha256(data).hexdigest() == artifact["sha256"]
                assert len(data) % 4 == 0 and any(
                    data[i:i + 4] != data[:4] for i in range(4, len(data), 4)
                ), "uniform/degenerate image"
                if baseline is None:
                    baseline = data
                assert data == baseline, "GPU bytes differ"
                artifacts += 1
    return {"accepted": True, "scope": "same-capture GPU correctness only",
            "prepared_serial_reads": counter_values[1],
            "byte_exact_artifacts": artifacts,
            "rgba_bytes": len(baseline),
            "rgba_sha256": hashlib.sha256(baseline).hexdigest(),
            "provider_sha256": a["provider"]["sha256"],
            "source_plan_sha256": a["result"]["source_plan_sha256"]}


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: check_prepared_serial_pair.py REFERENCE CANDIDATE")
    print(json.dumps(check_pair(*(pathlib.Path(p) for p in sys.argv[1:])), indent=2))
