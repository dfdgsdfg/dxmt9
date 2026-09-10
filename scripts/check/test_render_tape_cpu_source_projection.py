#!/usr/bin/env python3
"""Native-free tests for the CPU source buffer-closure validator."""

from __future__ import annotations

import copy
import importlib.util
import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "scripts" / "tools" / "render_tape_cpu_source_projection.py"
SPEC = importlib.util.spec_from_file_location("render_tape_cpu_source_projection", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def fixture() -> tuple[dict, dict]:
    identity_a = {"kind": 2, "generation": 1, "object_id": "7"}
    identity_b = {"kind": 2, "generation": 2, "object_id": "7"}
    inspection = {
        "event_types": [2, 4, 5, 3, 2, 5],
        "command_event_indices": [2, 5],
        "buffer_definitions": [
            {"event": 1, "identity": identity_a, "usage": 0, "pool": 0, "size": 64},
            {"event": 5, "identity": identity_b, "usage": 0, "pool": 0, "size": 64},
        ],
        "buffer_mutations": [
            {
                "event": 2,
                "identity": identity_a,
                "mutation_kind": 1,
                "disposition": 0,
                "byte_offset": 0,
                "byte_size": 16,
            }
        ],
        "buffer_destroys": [{"event": 4, "identity": identity_a}],
    }
    trace = [
        {
            "event": 3,
            "source": 1,
            "source_generation": 1,
            "buffer_closure": [identity_a],
        },
        {
            "event": 6,
            "source": 2,
            "source_generation": 1,
            "buffer_closure": [identity_b],
        },
    ]
    result = {"iterations": [{"trace": copy.deepcopy(trace)}, {"trace": copy.deepcopy(trace)}]}
    return result, inspection


def rejects(result: dict, inspection: dict) -> None:
    try:
        MODULE.validate_cpu_source_closures(result, inspection)
    except ValueError:
        return
    raise AssertionError("malformed source closure was accepted")


def main() -> int:
    result, inspection = fixture()
    assert MODULE.validate_cpu_source_closures(result, inspection) == [
        {"event": 3, "buffer_closure": [{"kind": 2, "generation": 1, "object_id": "7"}]},
        {"event": 6, "buffer_closure": [{"kind": 2, "generation": 2, "object_id": "7"}]},
    ]

    for label, mutate in (
        ("missingclosure", lambda r: r["iterations"][0]["trace"][0].pop("buffer_closure")),
        ("ghost", lambda r: r["iterations"][0]["trace"][0]["buffer_closure"].__setitem__(
            0, {"kind": 2, "generation": 1, "object_id": "99"})),
        ("stale", lambda r: r["iterations"][0]["trace"][1]["buffer_closure"].__setitem__(
            0, {"kind": 2, "generation": 1, "object_id": "7"})),
        ("wronggeneration", lambda r: r["iterations"][0]["trace"][0]["buffer_closure"].__setitem__(
            0, {"kind": 2, "generation": 2, "object_id": "7"})),
        ("unknownkind", lambda r: r["iterations"][0]["trace"][0]["buffer_closure"].__setitem__(
            0, {"kind": 1, "generation": 1, "object_id": "7"})),
        ("boolint", lambda r: r["iterations"][0]["trace"][0]["buffer_closure"][0].__setitem__(
            "generation", True)),
        ("duplicates", lambda r: r["iterations"][0]["trace"][0]["buffer_closure"].append(
            copy.deepcopy(r["iterations"][0]["trace"][0]["buffer_closure"][0]))),
        ("noncanonicalids", lambda r: r["iterations"][0]["trace"][0]["buffer_closure"][0].__setitem__(
            "object_id", "07")),
        ("lifecycle", lambda i: i["buffer_definitions"].append({
            "event": 3, "identity": {"kind": 2, "generation": 3, "object_id": "7"},
            "usage": 0, "pool": 0, "size": 64})),
        ("tamperediteration", lambda r: r["iterations"][1]["trace"][0]["buffer_closure"].__setitem__(
            0, {"kind": 2, "generation": 1, "object_id": "8"})),
        ("missingevent", lambda r: r["iterations"][0]["trace"].pop()),
    ):
        broken_result, broken_inspection = copy.deepcopy(result), copy.deepcopy(inspection)
        (mutate(broken_inspection) if label == "lifecycle" else mutate(broken_result))
        try:
            MODULE.validate_cpu_source_closures(broken_result, broken_inspection)
        except ValueError:
            pass
        else:
            raise AssertionError(label)

    omitted_command = copy.deepcopy(inspection)
    omitted_command["command_event_indices"] = [2]
    rejects(result, omitted_command)

    ordered_result = {
        "iterations": [{"trace": [{
            "event": 4,
            "source": 1,
            "source_generation": 1,
            "buffer_closure": [
                {"kind": 2, "generation": 1, "object_id": "7"},
                {"kind": 2, "generation": 1, "object_id": "8"},
            ],
        }]}]
    }
    ordered_inspection = {
        "event_types": [2, 2, 4, 5],
        "command_event_indices": [3],
        "buffer_definitions": [
            {"event": 1, "identity": {"kind": 2, "generation": 1, "object_id": "7"},
             "usage": 0, "pool": 0, "size": 64},
            {"event": 2, "identity": {"kind": 2, "generation": 1, "object_id": "8"},
             "usage": 0, "pool": 0, "size": 64},
        ],
        "buffer_mutations": [],
        "buffer_destroys": [],
    }
    assert MODULE.validate_cpu_source_closures(ordered_result, ordered_inspection)
    ordered_result["iterations"][0]["trace"][0]["buffer_closure"].reverse()
    rejects(ordered_result, ordered_inspection)

    # A binding inherited by a command is valid even when the command's raw
    # table has no corresponding row; only the authenticated lifetime matters.
    inherited_result, inherited_inspection = fixture()
    inherited_inspection["raw_command_buffer_table"] = {"3": []}
    assert MODULE.validate_cpu_source_closures(inherited_result, inherited_inspection)
    projection_binding_tests()
    print("render_tape_cpu_source_projection: PASS")
    return 0


def projection_binding_tests() -> None:
    result, inspection = fixture()
    components = {"events": {"bytes": 1, "sha256": "event"}, "blobs": []}
    images = [{"path": "provider", "bytes": 1, "sha256": "image"}]
    native = copy.deepcopy(result)
    native["loaded_images"] = copy.deepcopy(images)
    receipt = {
        "schema": "dxmt9.render_tape.cpu_experiment_artifact.v1",
        "accepted": True,
        "provider_exit_code": 0,
        "provider": {"path": "provider", "sha256": "provider"},
        "input_components": copy.deepcopy(components),
        "input_inspection": copy.deepcopy(inspection),
        "requested": {"repeat": 2},
        "result": native,
        "gpu_evidence": False,
    }
    sources = MODULE.validate_cpu_source_closures(native, inspection)
    projection = {
        "schema": "dxmt9.render_tape.cpu_source_projection.v1",
        "accepted": True,
        "failures": [],
        "input_components": copy.deepcopy(components),
        "input_inspection": copy.deepcopy(inspection),
        "provider": {"path": "provider", "sha256": "provider"},
        "validator": {"path": "validator", "sha256": "validator"},
        "loaded_images": copy.deepcopy(images),
        "repeat": 2,
        "sources": copy.deepcopy(sources),
    }
    assert MODULE.validate_cpu_projection_binding(
        projection, receipt, inspection, components, "provider", "validator"
    ) == sources
    assert "compare_wrapper_exit_code" not in receipt

    def rejects_binding(mutate, *, mutate_receipt=False) -> None:
        broken_projection = copy.deepcopy(projection)
        broken_receipt = copy.deepcopy(receipt)
        mutate(broken_receipt if mutate_receipt else broken_projection)
        try:
            MODULE.validate_cpu_projection_binding(
                broken_projection, broken_receipt, inspection, components,
                "provider", "validator"
            )
        except ValueError:
            return
        raise AssertionError("projection binding mutation was accepted")

    for mutate in (
        lambda value: value.__setitem__("schema", "wrong"),
        lambda value: value.__setitem__("accepted", False),
        lambda value: value["failures"].append({"name": "tampered"}),
        lambda value: value["sources"][0]["buffer_closure"].__setitem__(
            0, {"kind": 2, "generation": 1, "object_id": "99"}),
        lambda value: value["sources"][1]["buffer_closure"].__setitem__(
            0, {"kind": 2, "generation": 1, "object_id": "7"}),
        lambda value: value.__setitem__("input_components", {"different": True}),
        lambda value: value.__setitem__("input_inspection", {"different": True}),
        lambda value: value["provider"].__setitem__("sha256", "wrong"),
        lambda value: value["validator"].__setitem__("sha256", "wrong"),
        lambda value: value.__setitem__("repeat", 1),
        lambda value: value.__setitem__("loaded_images", []),
    ):
        rejects_binding(mutate)
    rejects_binding(lambda value: value.__setitem__("accepted", False), mutate_receipt=True)
    rejects_binding(lambda value: value.__setitem__("diagnostic", True), mutate_receipt=True)
    rejects_binding(lambda value: value.__setitem__("metal_validation_diagnostic", True),
                    mutate_receipt=True)
    rejects_binding(lambda value: value.__setitem__("provider_exit_code", 1), mutate_receipt=True)
    rejects_binding(lambda value: value["provider"].__setitem__("sha256", "wrong"),
                    mutate_receipt=True)
    rejects_binding(lambda value: value.__setitem__("input_components", {"different": True}),
                    mutate_receipt=True)
    rejects_binding(lambda value: value.__setitem__("input_inspection", {"different": True}),
                    mutate_receipt=True)
    rejects_binding(lambda value: value["result"].__setitem__("iterations", []),
                    mutate_receipt=True)
    rejects_binding(lambda value: value["requested"].__setitem__("repeat", 3),
                    mutate_receipt=True)
    rejects_binding(lambda value: value["result"].__setitem__("loaded_images", []),
                    mutate_receipt=True)
    missing_metadata = copy.deepcopy(inspection)
    missing_metadata.pop("buffer_definitions")
    try:
        MODULE.validate_cpu_projection_binding(
            projection, receipt, missing_metadata, components, "provider", "validator"
        )
    except ValueError:
        pass
    else:
        raise AssertionError("missing metadata was accepted")


if __name__ == "__main__":
    raise SystemExit(main())
