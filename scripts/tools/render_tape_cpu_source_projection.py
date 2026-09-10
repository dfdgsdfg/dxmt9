#!/usr/bin/env python3
"""Validate the native CPU source buffer-closure observation.

This is a structural validator for an observation produced by the native
CPU-resource executor.  It does not reconstruct D3D state and it does not
claim that the native visitor visited every required resource.  The caller is
responsible for authenticating the complete result, input inspection, and
binary hashes before calling :func:`validate_cpu_source_closures`.
"""

from __future__ import annotations

from typing import Any


# Keep this tied to include/dxmt9/device_c.h's D9C_CHUNK_HANDLE_KIND_BUFFER.
BUFFER_KIND = 2
OBJECT_DEFINE = 2
OBJECT_DESTROY = 3
RESOURCE_MUTATION = 4
COMMAND_CHUNK = 5
UINT32_MAX = 0xFFFFFFFF
UINT64_MAX = 0xFFFFFFFFFFFFFFFF
MAX_BUFFERS = 512


def _invalid(detail: str) -> ValueError:
    return ValueError(f"invalid CPU source buffer closure: {detail}")


def _mapping(value: Any, name: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise _invalid(f"{name} must be an object")
    return value


def _list(value: Any, name: str) -> list[Any]:
    if not isinstance(value, list):
        raise _invalid(f"{name} must be an array")
    return value


def _uint(value: Any, name: str, *, maximum: int) -> int:
    # bool is an int subclass, but is never a valid wire integer here.
    if type(value) is not int or value < 0 or value > maximum:
        raise _invalid(f"{name} must be an unsigned integer")
    return value


def _positive_uint(value: Any, name: str, *, maximum: int) -> int:
    result = _uint(value, name, maximum=maximum)
    if result == 0:
        raise _invalid(f"{name} must be nonzero")
    return result


def _decimal_uint64(value: Any, name: str) -> str:
    if (not isinstance(value, str) or not value or len(value) > 20
            or (len(value) != 1 and value[0] == "0")
            or any(character < "0" or character > "9" for character in value)):
        raise _invalid(f"{name} must be a canonical decimal uint64 string")
    if int(value) == 0 or int(value) > UINT64_MAX:
        raise _invalid(f"{name} must be a nonzero uint64")
    return value


def _identity(value: Any, name: str) -> tuple[int, int, str]:
    row = _mapping(value, name)
    if set(row) != {"kind", "generation", "object_id"}:
        raise _invalid(f"{name} has an unexpected identity shape")
    kind = _uint(row["kind"], f"{name}.kind", maximum=UINT32_MAX)
    if kind != BUFFER_KIND:
        raise _invalid(f"{name}.kind is not BUFFER ({BUFFER_KIND})")
    generation = _positive_uint(
        row["generation"], f"{name}.generation", maximum=UINT32_MAX
    )
    object_id = _decimal_uint64(row["object_id"], f"{name}.object_id")
    return (kind, generation, object_id)


def _identity_row(identity: tuple[int, int, str]) -> dict[str, Any]:
    return {
        "kind": identity[0],
        "generation": identity[1],
        "object_id": identity[2],
    }


def _metadata_rows(
    input_inspection: dict[str, Any],
) -> tuple[list[Any], list[Any], list[Any], list[int], list[int]]:
    event_types = _list(input_inspection.get("event_types"), "event_types")
    if not event_types:
        raise _invalid("event_types must not be empty")
    normalized_types: list[int] = []
    for index, value in enumerate(event_types, 1):
        normalized_types.append(
            _positive_uint(value, f"event_types[{index}]", maximum=UINT32_MAX)
        )
        if normalized_types[-1] > 7:
            raise _invalid(f"event_types[{index}] is unknown")

    command_indices = _list(
        input_inspection.get("command_event_indices"),
        "command_event_indices",
    )
    normalized_commands: list[int] = []
    previous = 0
    for index, value in enumerate(command_indices):
        command_index = _uint(
            value, f"command_event_indices[{index}]", maximum=UINT32_MAX
        )
        if command_index >= len(normalized_types):
            raise _invalid("command event index is outside event_types")
        event = command_index + 1
        if event <= previous:
            raise _invalid("command_event_indices must be strictly increasing")
        if normalized_types[event - 1] != COMMAND_CHUNK:
            raise _invalid("command_event_indices contains a non-command event")
        normalized_commands.append(event)
        previous = event

    expected_commands = [
        index + 1
        for index, event_type in enumerate(normalized_types)
        if event_type == COMMAND_CHUNK
    ]
    if normalized_commands != expected_commands:
        raise _invalid("command_event_indices must enumerate every command event")

    definitions = _list(
        input_inspection.get("buffer_definitions"), "buffer_definitions"
    )
    mutations = _list(
        input_inspection.get("buffer_mutations"), "buffer_mutations"
    )
    destroys = _list(
        input_inspection.get("buffer_destroys"), "buffer_destroys"
    )
    return definitions, mutations, destroys, normalized_types, normalized_commands


def _event(row: dict[str, Any], name: str, event_count: int) -> int:
    value = _positive_uint(row.get("event"), f"{name}.event", maximum=UINT32_MAX)
    if value > event_count:
        raise _invalid(f"{name}.event is outside event_types")
    return value


def _validate_metadata(
    definitions: list[Any],
    mutations: list[Any],
    destroys: list[Any],
    event_types: list[int],
) -> dict[int, tuple[tuple[int, int, str], ...]]:
    """Validate metadata and return the live identity after every event."""

    # The three metadata arrays are independent native observations.  An event
    # ordinal may occur in exactly one of them, and each row must agree with
    # the authenticated event type.
    metadata_events: dict[int, str] = {}
    definition_rows: list[tuple[int, dict[str, Any], tuple[int, int, str]]] = []
    mutation_rows: list[tuple[int, dict[str, Any], tuple[int, int, str]]] = []
    destroy_rows: list[tuple[int, dict[str, Any], tuple[int, int, str]]] = []

    def reserve(event: int, kind: str) -> None:
        previous = metadata_events.get(event)
        if previous is not None:
            raise _invalid(f"duplicate metadata event {event} ({previous}/{kind})")
        metadata_events[event] = kind

    for index, raw in enumerate(definitions):
        row = _mapping(raw, f"buffer_definitions[{index}]")
        event = _event(row, f"buffer_definitions[{index}]", len(event_types))
        if event_types[event - 1] != OBJECT_DEFINE:
            raise _invalid("buffer definition ordinal does not name ObjectDefine")
        identity = _identity(row.get("identity"), f"buffer_definitions[{index}].identity")
        _uint(row.get("usage"), f"buffer_definitions[{index}].usage", maximum=UINT32_MAX)
        _uint(row.get("pool"), f"buffer_definitions[{index}].pool", maximum=UINT32_MAX)
        _positive_uint(
            row.get("size"), f"buffer_definitions[{index}].size", maximum=UINT64_MAX
        )
        reserve(event, "define")
        definition_rows.append((event, row, identity))

    for index, raw in enumerate(mutations):
        row = _mapping(raw, f"buffer_mutations[{index}]")
        event = _event(row, f"buffer_mutations[{index}]", len(event_types))
        if event_types[event - 1] != RESOURCE_MUTATION:
            raise _invalid("buffer mutation ordinal does not name ResourceMutation")
        identity = _identity(row.get("identity"), f"buffer_mutations[{index}].identity")
        _uint(
            row.get("mutation_kind"),
            f"buffer_mutations[{index}].mutation_kind",
            maximum=UINT32_MAX,
        )
        _uint(
            row.get("disposition"),
            f"buffer_mutations[{index}].disposition",
            maximum=UINT32_MAX,
        )
        offset = _uint(
            row.get("byte_offset"),
            f"buffer_mutations[{index}].byte_offset",
            maximum=UINT64_MAX,
        )
        byte_size = _uint(
            row.get("byte_size"),
            f"buffer_mutations[{index}].byte_size",
            maximum=UINT64_MAX,
        )
        if offset + byte_size > UINT64_MAX:
            raise _invalid("buffer mutation byte range overflows uint64")
        reserve(event, "mutation")
        mutation_rows.append((event, row, identity))

    for index, raw in enumerate(destroys):
        row = _mapping(raw, f"buffer_destroys[{index}]")
        event = _event(row, f"buffer_destroys[{index}]", len(event_types))
        if event_types[event - 1] != OBJECT_DESTROY:
            raise _invalid("buffer destroy ordinal does not name ObjectDestroy")
        identity = _identity(row.get("identity"), f"buffer_destroys[{index}].identity")
        reserve(event, "destroy")
        destroy_rows.append((event, row, identity))

    actions: dict[int, tuple[str, dict[str, Any], tuple[int, int, str]]] = {}
    for event, row, identity in definition_rows:
        actions[event] = ("define", row, identity)
    for event, row, identity in mutation_rows:
        actions[event] = ("mutation", row, identity)
    for event, row, identity in destroy_rows:
        actions[event] = ("destroy", row, identity)

    active_by_object: dict[str, tuple[int, int, str]] = {}
    highest_generation: dict[str, int] = {}
    live_at_event: dict[int, tuple[tuple[int, int, str], ...]] = {}
    definition_size: dict[tuple[int, int, str], int] = {}

    for event in range(1, len(event_types) + 1):
        action = actions.get(event)
        if action is not None:
            kind, row, identity = action
            _kind, generation, object_id = identity
            if kind == "define":
                if object_id in active_by_object:
                    raise _invalid(f"buffer object {object_id} is redefined while alive")
                previous_generation = highest_generation.get(object_id, 0)
                if generation <= previous_generation:
                    raise _invalid("buffer redefine must increase generation")
                active_by_object[object_id] = identity
                highest_generation[object_id] = generation
                definition_size[identity] = int(row["size"])
            elif kind == "mutation":
                current = active_by_object.get(object_id)
                if current != identity:
                    raise _invalid("buffer mutation references a stale or unknown identity")
                offset = int(row["byte_offset"])
                byte_size = int(row["byte_size"])
                if offset + byte_size > definition_size[identity]:
                    raise _invalid("buffer mutation range exceeds buffer size")
            else:
                current = active_by_object.get(object_id)
                if current != identity:
                    raise _invalid("buffer destroy references a stale or unknown identity")
                del active_by_object[object_id]
        live = tuple(sorted(active_by_object.values(), key=_identity_sort_key))
        if len(live) > MAX_BUFFERS:
            raise _invalid("more than 512 buffers are alive")
        live_at_event[event] = live

    return live_at_event


def _identity_sort_key(identity: tuple[int, int, str]) -> tuple[int, int, int]:
    return identity[0], identity[1], int(identity[2])


def _strict_equal(left: Any, right: Any) -> bool:
    """Compare JSON-shaped evidence without bool/int coercion."""
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return (left.keys() == right.keys()
                and all(_strict_equal(left[key], right[key]) for key in left))
    if isinstance(left, list):
        return len(left) == len(right) and all(
            _strict_equal(a, b) for a, b in zip(left, right)
        )
    return left == right


def _validate_closure(
    value: Any,
    name: str,
    live: tuple[tuple[int, int, str], ...],
) -> list[dict[str, Any]]:
    rows = _list(value, name)
    if len(rows) > MAX_BUFFERS:
        raise _invalid(f"{name} exceeds the 512-buffer bound")
    result: list[dict[str, Any]] = []
    previous: tuple[int, int, int] | None = None
    live_set = set(live)
    for index, raw in enumerate(rows):
        identity = _identity(raw, f"{name}[{index}]")
        sort_key = _identity_sort_key(identity)
        if previous is not None and sort_key <= previous:
            raise _invalid(f"{name} is not canonical sorted and unique")
        if identity not in live_set:
            raise _invalid(f"{name}[{index}] is not alive at its command")
        result.append(_identity_row(identity))
        previous = sort_key
    return result


def validate_cpu_source_closures(
    native_result: Any, input_inspection: Any
) -> list[dict[str, Any]]:
    """Validate and normalize native per-command buffer source closures.

    The returned list is ordered exactly as the authenticated command-event
    list.  Every iteration must report the same event/closure projection;
    native allocator and timing fields are deliberately outside that equality.
    """

    result = _mapping(native_result, "native_result")
    inspection = _mapping(input_inspection, "input_inspection")
    definitions, mutations, destroys, event_types, command_indices = _metadata_rows(
        inspection
    )
    lifetime = _validate_metadata(definitions, mutations, destroys, event_types)
    iterations = _list(result.get("iterations"), "native_result.iterations")
    if not iterations:
        raise _invalid("native_result.iterations must not be empty")

    expected: list[dict[str, Any]] | None = None
    for iteration_index, raw_iteration in enumerate(iterations):
        iteration = _mapping(raw_iteration, f"iterations[{iteration_index}]")
        trace = _list(iteration.get("trace"), f"iterations[{iteration_index}].trace")
        if len(trace) != len(command_indices):
            raise _invalid("trace event count does not match command_event_indices")
        events: list[int] = []
        projection: list[dict[str, Any]] = []
        for trace_index, raw_trace in enumerate(trace):
            row = _mapping(raw_trace, f"iterations[{iteration_index}].trace[{trace_index}]")
            event = _positive_uint(
                row.get("event"),
                f"iterations[{iteration_index}].trace[{trace_index}].event",
                maximum=UINT32_MAX,
            )
            if event > len(event_types):
                raise _invalid("trace event is outside event_types")
            if events and event <= events[-1]:
                raise _invalid("trace events must be strictly increasing")
            # These are the pre-existing native source/event identity fields;
            # allocator locators and timing fields remain outside this
            # projection's equality relation.
            _uint(
                row.get("source"),
                f"iterations[{iteration_index}].trace[{trace_index}].source",
                maximum=UINT32_MAX,
            )
            _positive_uint(
                row.get("source_generation"),
                f"iterations[{iteration_index}].trace[{trace_index}].source_generation",
                maximum=UINT64_MAX,
            )
            events.append(event)
            closure = _validate_closure(
                row.get("buffer_closure"),
                f"iterations[{iteration_index}].trace[{trace_index}].buffer_closure",
                lifetime[event],
            )
            projection.append({"event": event, "buffer_closure": closure})
        if events != command_indices:
            raise _invalid("trace events are not exactly command_event_indices")
        if expected is None:
            expected = projection
        elif projection != expected:
            raise _invalid("iterations disagree on the source buffer closure")

    # iterations is nonempty, so expected is necessarily populated.
    assert expected is not None
    return expected


def validate_cpu_projection_binding(
    projection: Any,
    reference_receipt: Any,
    current_inspection: Any,
    current_components: Any,
    provider_sha256: Any,
    validator_sha256: Any,
) -> list[dict[str, Any]]:
    """Validate the provenance binding around a source projection.

    This binds the projection to the caller-authenticated current input,
    executable identities, and the accepted on-disk reference child receipt.
    It intentionally does not hash the projection file, reference receipt, or
    loaded-image paths; those bytes and files must be authenticated by the
    caller before this pure value check.
    """

    projection_row = _mapping(projection, "projection")
    receipt = _mapping(reference_receipt, "reference_receipt")
    inspection = _mapping(current_inspection, "current_inspection")
    components = _mapping(current_components, "current_components")
    if type(provider_sha256) is not str or type(validator_sha256) is not str:
        raise _invalid("provider and validator hashes must be strings")

    if projection_row.get("schema") != "dxmt9.render_tape.cpu_source_projection.v1":
        raise _invalid("projection schema is not accepted")
    if projection_row.get("accepted") is not True:
        raise _invalid("projection is not accepted")
    failures = projection_row.get("failures")
    if not isinstance(failures, list) or failures:
        raise _invalid("projection contains failures")
    if not _strict_equal(projection_row.get("input_inspection"), inspection):
        raise _invalid("projection input inspection does not match current input")
    if not _strict_equal(projection_row.get("input_components"), components):
        raise _invalid("projection input components do not match current input")

    provider = _mapping(projection_row.get("provider"), "projection.provider")
    validator = _mapping(projection_row.get("validator"), "projection.validator")
    if provider.get("sha256") != provider_sha256:
        raise _invalid("projection provider hash does not match")
    if validator.get("sha256") != validator_sha256:
        raise _invalid("projection validator hash does not match")

    if receipt.get("schema") != "dxmt9.render_tape.cpu_experiment_artifact.v1":
        raise _invalid("reference receipt schema is not accepted")
    if receipt.get("accepted") is not True:
        raise _invalid("reference receipt is not accepted")
    if (receipt.get("diagnostic", False) is not False
            or receipt.get("metal_validation_diagnostic", False) is not False):
        raise _invalid("reference receipt is diagnostic")
    receipt_failures = receipt.get("failures")
    if receipt_failures is not None and (not isinstance(receipt_failures, list)
                                         or receipt_failures):
        raise _invalid("reference receipt contains failures")
    if type(receipt.get("provider_exit_code")) is not int:
        raise _invalid("reference provider exit code is not an integer")
    if receipt["provider_exit_code"] != 0:
        raise _invalid("reference provider did not exit successfully")
    receipt_provider = _mapping(receipt.get("provider"), "reference_receipt.provider")
    if receipt_provider.get("sha256") != provider_sha256:
        raise _invalid("reference provider hash does not match")
    if not _strict_equal(receipt.get("input_inspection"), inspection):
        raise _invalid("reference input inspection does not match current input")
    if not _strict_equal(receipt.get("input_components"), components):
        raise _invalid("reference input components do not match current input")

    native = _mapping(receipt.get("result"), "reference_receipt.result")
    sources = validate_cpu_source_closures(native, inspection)
    repeat = projection_row.get("repeat")
    if type(repeat) is not int or isinstance(repeat, bool) or repeat < 2:
        raise _invalid("projection repeat must be an integer of at least two")
    if repeat != len(native["iterations"]):
        raise _invalid("projection repeat does not match native iterations")
    requested = receipt.get("requested")
    if requested is not None:
        requested_row = _mapping(requested, "reference_receipt.requested")
        if "repeat" in requested_row:
            requested_repeat = requested_row["repeat"]
            if (type(requested_repeat) is not int or isinstance(requested_repeat, bool)
                    or requested_repeat < 2 or requested_repeat != repeat):
                raise _invalid("projection repeat does not match reference request")

    native_images = native.get("loaded_images")
    if not isinstance(native_images, list):
        raise _invalid("reference native result has no loaded_images list")
    if not _strict_equal(projection_row.get("loaded_images"), native_images):
        raise _invalid("projection loaded_images differ from reference native result")
    if not _strict_equal(projection_row.get("sources"), sources):
        raise _invalid("projection sources differ from native closures")
    return sources


__all__ = [
    "BUFFER_KIND",
    "validate_cpu_projection_binding",
    "validate_cpu_source_closures",
]
