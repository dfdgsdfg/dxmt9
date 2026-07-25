# Vertex Remap Locality Discriminator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Determine whether index/parameter-buffer write locality or primitive order owns the 3.86x hidden VS-write density delta measured in `replay.03`, using an offline four-lane mini-replay discriminator.

**Architecture:** Add a `--vertex-order` mode to the existing standalone mini-replay harness that permutes vertex storage and rewrites indices while preserving triangle order, vertex bytes, payload size, and `base_vertex`. Run four lanes over one GT1 frame60 encoder2 manifest and compare Xcode encoder counters. No runtime code changes.

**Tech Stack:** Python 3 (`scripts/tools/run_3dmark05_mini_replay.py`), `unittest`, Meson test registration, the 3DMark05 perf probe wrapper, Xcode GPU frame capture.

## Global Constraints

- Do not modify anything under `src/`. This is a harness-only change.
- Do not add any `DXMT9_*` / `DXMT_*` environment variable and do not add any perf counter.
- Target row is 3DMark05 GT1 `frame60`, `encoder 2`. Do not substitute another row.
- The replay binary hardcodes `MTLIndexTypeUInt16`; every selected draw must have uint16 indices.
- All 3DMark05 runs must pass a positive `--timeout`. `--no-gputrace` scouts use `120`; gputrace runs use `420`.
- Keep at least 2 GiB free on the repository volume before any gputrace run. The wrapper enforces this.
- Trace artifacts live under `traces/<app-runid>/`, analysis under `traces/<app-runid>/analysis/`. `traces/` is gitignored; never commit raw captures.
- Python style in `scripts/tools/`: 4-space indent, `from __future__ import annotations`, type hints, `raise SystemExit(...)` for user-facing failures.
- Script tests are `unittest` files under `tests/scripts/` registered in `tests/meson.build` with `suite: ['scripts']`, `workdir: meson.project_source_root()`, `is_parallel: false`.
- Recovery thresholds for the verdict are `0.15` and `0.7` exactly as defined in the spec.

---

## File Structure

| File | Responsibility |
|---|---|
| `scripts/tools/run_3dmark05_mini_replay.py` (modify) | Add four pure permutation helpers, one per-draw remap driver, and the `--vertex-order` CLI wiring. |
| `tests/scripts/test_mini_replay_vertex_order.py` (create) | Prove the fetch-equivalence invariant and the permutation's structural properties without a GPU. |
| `tests/meson.build` (modify) | Register the new test so CI actually gates it. |
| `docs/superpowers/specs/2026-07-25-vertex-remap-design.md` (modify) | Correct the unreachable `base_vertex` fallback clause. |
| `docs/perfomance/mini-replay-bisection/mini-replay-bisection-vertexremap.01.md` (create) | Record the experiment and its verdict. |
| `agents/rules/metal_debugging.rules.md` (modify) | Document `--vertex-order` in the mini-replay flag row. |

The permutation helpers are pure functions over `(indices, base_vertex, payload, offset, stride)`. They know nothing about manifests, files, or Metal. The per-draw driver is the only part that touches manifest dictionaries and writes files. This split is what makes the invariant testable without a GPU.

---

### Task 1: Pure vertex permutation helpers

**Files:**
- Modify: `scripts/tools/run_3dmark05_mini_replay.py` — insert after `uint16_indices` (currently ends at line 232)
- Modify: `docs/superpowers/specs/2026-07-25-vertex-remap-design.md` — the "Precondition" paragraph in "Permutation rules"
- Create: `tests/scripts/test_mini_replay_vertex_order.py`
- Modify: `tests/meson.build` — after the `dxmt9-run-experiment-renderer-defaults` block (currently ends at line 179)

**Interfaces:**
- Consumes: `uint16_indices(payload: bytes) -> list[int]`, already present in the harness.
- Produces:
  - `vertex_slot_capacity(payload_bytes: int, offset: int, stride: int) -> int`
  - `vertex_reference_order(indices: list[int], base_vertex: int, vertex_order: str) -> list[int]`
  - `vertex_slot_assignment(order: list[int], base_vertex: int, slot_count: int) -> list[int]`
  - `apply_vertex_permutation(payload: bytes, offset: int, stride: int, new_for_old: list[int]) -> bytes`
  - `rewrite_indices_for_permutation(indices: list[int], base_vertex: int, order: list[int]) -> list[int]`

**Background the implementer needs.** The replay shader fetches a vertex at byte address `offset + (index + base_vertex) * stride`, reading from a stream buffer bound at Metal offset 0. `offset`, `stride`, and `base_vertex` reach the shader through a `DrawVolatile` struct at vertex buffer index 5 (`run_3dmark05_mini_replay.py:1418`). The draw call passes `indexBufferOffset:0` and no `baseVertex` parameter, so `[[vertex_id]]` is the raw index value and `base_vertex` is applied inside the shader. Call `index + base_vertex` a **fetch slot**. Slots start at byte `offset`, not at byte 0 of the dumped payload — this is the one detail easiest to get wrong.

- [ ] **Step 1: Write the failing test**

Create `tests/scripts/test_mini_replay_vertex_order.py`:

```python
import struct
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "tools"))

import run_3dmark05_mini_replay as mini  # noqa: E402


STRIDE = 8
SLOT_COUNT = 5
MODES = ("first-reference", "scatter")
# Two triangles referencing slots 4, 2 and 0, leaving slots 1 and 3 unreferenced.
INDICES = [4, 2, 4, 0, 2, 4]


def make_payload(slot_count: int = SLOT_COUNT, offset: int = 0) -> bytes:
    """Each slot is filled with a distinct repeated byte so fetches are identifiable."""
    body = b"".join(bytes([0x10 + slot]) * STRIDE for slot in range(slot_count))
    return b"\xee" * offset + body


def fetch(payload: bytes, offset: int, stride: int, slot: int) -> bytes:
    start = offset + slot * stride
    return payload[start:start + stride]


def permute(indices, base_vertex, mode, offset=0, slot_count=SLOT_COUNT):
    payload = make_payload(slot_count=slot_count, offset=offset)
    capacity = mini.vertex_slot_capacity(len(payload), offset, STRIDE)
    order = mini.vertex_reference_order(indices, base_vertex, mode)
    new_for_old = mini.vertex_slot_assignment(order, base_vertex, capacity)
    new_payload = mini.apply_vertex_permutation(payload, offset, STRIDE, new_for_old)
    new_indices = mini.rewrite_indices_for_permutation(indices, base_vertex, order)
    return payload, new_payload, order, new_for_old, new_indices, capacity


class VertexSlotCapacityTest(unittest.TestCase):
    def test_capacity_accounts_for_offset(self):
        self.assertEqual(mini.vertex_slot_capacity(40, 0, 8), 5)
        self.assertEqual(mini.vertex_slot_capacity(40, 8, 8), 4)
        self.assertEqual(mini.vertex_slot_capacity(43, 3, 8), 5)

    def test_rejects_nonpositive_stride(self):
        with self.assertRaises(SystemExit):
            mini.vertex_slot_capacity(40, 0, 0)

    def test_rejects_payload_too_small_for_one_vertex(self):
        with self.assertRaises(SystemExit):
            mini.vertex_slot_capacity(8, 4, 8)


class VertexReferenceOrderTest(unittest.TestCase):
    def test_first_reference_order(self):
        self.assertEqual(
            mini.vertex_reference_order(INDICES, 0, "first-reference"),
            [4, 2, 0],
        )

    def test_base_vertex_shifts_slots(self):
        self.assertEqual(
            mini.vertex_reference_order([0, 1, 0], 2, "first-reference"),
            [2, 3],
        )

    def test_scatter_is_a_permutation_of_first_reference(self):
        first = mini.vertex_reference_order(INDICES, 0, "first-reference")
        scatter = mini.vertex_reference_order(INDICES, 0, "scatter")
        self.assertCountEqual(scatter, first)

    def test_scatter_differs_from_first_reference(self):
        first = mini.vertex_reference_order(INDICES, 0, "first-reference")
        scatter = mini.vertex_reference_order(INDICES, 0, "scatter")
        self.assertNotEqual(scatter, first)

    def test_scatter_is_deterministic(self):
        self.assertEqual(
            mini.vertex_reference_order(INDICES, 0, "scatter"),
            mini.vertex_reference_order(INDICES, 0, "scatter"),
        )

    def test_rejects_unknown_mode(self):
        with self.assertRaises(SystemExit):
            mini.vertex_reference_order(INDICES, 0, "sideways")


class VertexSlotAssignmentTest(unittest.TestCase):
    def test_assignment_is_a_bijection(self):
        for mode in MODES:
            with self.subTest(mode=mode):
                _, _, _, new_for_old, _, capacity = permute(INDICES, 0, mode)
                self.assertEqual(len(new_for_old), capacity)
                self.assertCountEqual(new_for_old, range(capacity))

    def test_referenced_slots_land_at_base_vertex_onwards(self):
        order = mini.vertex_reference_order(INDICES, 0, "first-reference")
        new_for_old = mini.vertex_slot_assignment(order, 0, SLOT_COUNT)
        for position, slot in enumerate(order):
            self.assertEqual(new_for_old[slot], position)

    def test_unreferenced_slots_fill_remaining_positions_ascending(self):
        order = mini.vertex_reference_order(INDICES, 0, "first-reference")
        new_for_old = mini.vertex_slot_assignment(order, 0, SLOT_COUNT)
        # order = [4, 2, 0] -> slots 4,2,0 take new slots 0,1,2.
        # Unreferenced slots 1 and 3 take the remaining new slots 3 and 4.
        self.assertEqual(new_for_old, [2, 3, 1, 4, 0])

    def test_rejects_more_referenced_vertices_than_slots(self):
        with self.assertRaises(SystemExit):
            mini.vertex_slot_assignment([0, 1, 2], 0, 2)

    def test_rejects_slot_beyond_capacity(self):
        with self.assertRaises(SystemExit):
            mini.vertex_slot_assignment([0, 7], 0, 5)


class FetchEquivalenceTest(unittest.TestCase):
    """The load-bearing invariant: every index position must fetch the same bytes."""

    def test_fetch_equivalence(self):
        for mode in MODES:
            for offset in (0, 3, 8):
                with self.subTest(mode=mode, offset=offset):
                    payload, new_payload, _, _, new_indices, _ = permute(
                        INDICES, 0, mode, offset=offset
                    )
                    for old_index, new_index in zip(INDICES, new_indices):
                        self.assertEqual(
                            fetch(new_payload, offset, STRIDE, new_index),
                            fetch(payload, offset, STRIDE, old_index),
                        )

    def test_fetch_equivalence_with_base_vertex(self):
        indices = [0, 1, 0, 2, 1, 0]
        base_vertex = 2
        for mode in MODES:
            with self.subTest(mode=mode):
                payload, new_payload, _, _, new_indices, _ = permute(
                    indices, base_vertex, mode, slot_count=5
                )
                for old_index, new_index in zip(indices, new_indices):
                    self.assertEqual(
                        fetch(new_payload, 0, STRIDE, new_index + base_vertex),
                        fetch(payload, 0, STRIDE, old_index + base_vertex),
                    )

    def test_multiple_strides_share_one_order(self):
        """Extra streams reuse the same reference order with their own stride."""
        order = mini.vertex_reference_order(INDICES, 0, "first-reference")
        new_indices = mini.rewrite_indices_for_permutation(INDICES, 0, order)
        for stride in (4, 8, 12):
            with self.subTest(stride=stride):
                payload = b"".join(bytes([0x10 + slot]) * stride for slot in range(SLOT_COUNT))
                capacity = mini.vertex_slot_capacity(len(payload), 0, stride)
                new_for_old = mini.vertex_slot_assignment(order, 0, capacity)
                new_payload = mini.apply_vertex_permutation(payload, 0, stride, new_for_old)
                for old_index, new_index in zip(INDICES, new_indices):
                    self.assertEqual(
                        fetch(new_payload, 0, stride, new_index),
                        fetch(payload, 0, stride, old_index),
                    )


class PayloadPreservationTest(unittest.TestCase):
    def test_size_and_byte_multiset_preserved(self):
        for mode in MODES:
            for offset in (0, 3):
                with self.subTest(mode=mode, offset=offset):
                    payload, new_payload, _, _, _, _ = permute(
                        INDICES, 0, mode, offset=offset
                    )
                    self.assertEqual(len(new_payload), len(payload))
                    self.assertEqual(sorted(new_payload), sorted(payload))

    def test_bytes_before_offset_are_untouched(self):
        offset = 3
        payload, new_payload, _, _, _, _ = permute(
            INDICES, 0, "first-reference", offset=offset
        )
        self.assertEqual(new_payload[:offset], payload[:offset])


class RewrittenIndexTest(unittest.TestCase):
    def test_first_reference_indices_are_monotonic_on_first_use(self):
        order = mini.vertex_reference_order(INDICES, 0, "first-reference")
        new_indices = mini.rewrite_indices_for_permutation(INDICES, 0, order)
        seen: set[int] = set()
        expected_next = 0
        for value in new_indices:
            if value in seen:
                continue
            self.assertEqual(value, expected_next)
            seen.add(value)
            expected_next += 1

    def test_index_reuse_pattern_is_unchanged(self):
        """A permutation must not change which positions share a vertex."""
        for mode in MODES:
            with self.subTest(mode=mode):
                order = mini.vertex_reference_order(INDICES, 0, mode)
                new_indices = mini.rewrite_indices_for_permutation(INDICES, 0, order)
                for left in range(len(INDICES)):
                    for right in range(len(INDICES)):
                        self.assertEqual(
                            INDICES[left] == INDICES[right],
                            new_indices[left] == new_indices[right],
                        )

    def test_new_max_index_does_not_exceed_original(self):
        for mode in MODES:
            with self.subTest(mode=mode):
                order = mini.vertex_reference_order(INDICES, 0, mode)
                new_indices = mini.rewrite_indices_for_permutation(INDICES, 0, order)
                self.assertLessEqual(max(new_indices), max(INDICES))

    def test_rewritten_payload_round_trips_through_uint16(self):
        order = mini.vertex_reference_order(INDICES, 0, "first-reference")
        new_indices = mini.rewrite_indices_for_permutation(INDICES, 0, order)
        packed = struct.pack(f"<{len(new_indices)}H", *new_indices)
        self.assertEqual(mini.uint16_indices(packed), new_indices)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `python3 tests/scripts/test_mini_replay_vertex_order.py -v`
Expected: FAIL with `AttributeError: module 'run_3dmark05_mini_replay' has no attribute 'vertex_slot_capacity'`

- [ ] **Step 3: Write the implementation**

Insert into `scripts/tools/run_3dmark05_mini_replay.py` immediately after the `uint16_indices` function:

```python
def vertex_slot_capacity(payload_bytes: int, offset: int, stride: int) -> int:
    """Number of addressable vertex slots in a dumped stream payload.

    The replay shader fetches a vertex at `offset + (index + base_vertex) *
    stride`, so slots start at `offset`, not at byte 0 of the payload.
    """
    if stride <= 0:
        raise SystemExit(f"vertex stride must be positive, got {stride}")
    if offset < 0:
        raise SystemExit(f"vertex offset must be non-negative, got {offset}")
    usable = payload_bytes - offset
    if usable < stride:
        raise SystemExit(
            f"stream payload of {payload_bytes} bytes at offset {offset} "
            f"cannot hold one {stride}-byte vertex"
        )
    return usable // stride


def vertex_reference_order(indices: list[int],
                          base_vertex: int,
                          vertex_order: str) -> list[int]:
    """Distinct fetch slots referenced by `indices`, ordered per `vertex_order`.

    `first-reference` returns them in order of first appearance, which makes the
    rewritten first-reference index sequence exactly 0, 1, 2, ... `scatter`
    returns them ordered by a multiplicative hash of the slot value, which
    decorrelates the layout from both slot value and reference order. The hash
    sort is used instead of an RNG so the permutation is reproducible across
    Python versions.
    """
    seen: set[int] = set()
    first_reference: list[int] = []
    for index in indices:
        slot = index + base_vertex
        if slot < 0:
            raise SystemExit(
                f"index {index} with base_vertex {base_vertex} yields negative slot"
            )
        if slot not in seen:
            seen.add(slot)
            first_reference.append(slot)
    if vertex_order == "first-reference":
        return first_reference
    if vertex_order == "scatter":
        return sorted(first_reference,
                      key=lambda slot: ((slot * 2654435761) % (2 ** 32), slot))
    raise SystemExit(f"unsupported vertex order: {vertex_order}")


def vertex_slot_assignment(order: list[int],
                           base_vertex: int,
                           slot_count: int) -> list[int]:
    """Map every old slot to a new slot. Returns `new_for_old`.

    Referenced slots land at `base_vertex + k` in `order` order, so the
    rewritten index for `order[k]` is simply `k`. Unreferenced slots fill the
    remaining positions in ascending order, which keeps the payload size and
    byte multiset identical to the source and leaves `base_vertex` valid.

    `base_vertex + len(order) <= slot_count` always holds for real input,
    because every index is non-negative, so `base_vertex <= min(order)` and
    therefore `base_vertex + len(order) <= max(order) + 1 <= slot_count`. The
    check below is a defensive assertion, not a reachable branch.
    """
    if len(order) > slot_count:
        raise SystemExit(
            f"{len(order)} referenced vertices exceed {slot_count} payload slots"
        )
    for slot in order:
        if slot >= slot_count:
            raise SystemExit(
                f"referenced slot {slot} escapes {slot_count} payload slots"
            )
    if base_vertex + len(order) > slot_count:
        raise SystemExit(
            f"base_vertex {base_vertex} plus {len(order)} referenced vertices "
            f"exceed {slot_count} payload slots"
        )
    new_for_old: list[int] = [-1] * slot_count
    taken: set[int] = set()
    for position, slot in enumerate(order):
        target = base_vertex + position
        new_for_old[slot] = target
        taken.add(target)
    spare = (slot for slot in range(slot_count) if slot not in taken)
    for slot in range(slot_count):
        if new_for_old[slot] < 0:
            new_for_old[slot] = next(spare)
    return new_for_old


def apply_vertex_permutation(payload: bytes,
                             offset: int,
                             stride: int,
                             new_for_old: list[int]) -> bytes:
    """Relocate each vertex slot. Reads from `payload`, writes into a copy."""
    out = bytearray(payload)
    for old_slot, new_slot in enumerate(new_for_old):
        src = offset + old_slot * stride
        dst = offset + new_slot * stride
        out[dst:dst + stride] = payload[src:src + stride]
    return bytes(out)


def rewrite_indices_for_permutation(indices: list[int],
                                    base_vertex: int,
                                    order: list[int]) -> list[int]:
    """Rewrite each index to its slot's position in `order`."""
    position = {slot: index for index, slot in enumerate(order)}
    rewritten = [position[index + base_vertex] for index in indices]
    if rewritten and max(rewritten) > 0xffff:
        raise SystemExit(
            f"rewritten index {max(rewritten)} exceeds uint16 range"
        )
    return rewritten
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `python3 tests/scripts/test_mini_replay_vertex_order.py -v`
Expected: PASS, all tests OK

- [ ] **Step 5: Register the test in Meson**

Add to `tests/meson.build` after the `dxmt9-run-experiment-renderer-defaults` test block:

```meson
test('dxmt9-mini-replay-vertex-order', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_mini_replay_vertex_order.py')],
     workdir: meson.project_source_root(),
     suite: ['scripts'],
     is_parallel: false)
```

- [ ] **Step 6: Run the test through Meson**

Run: `meson test -C build dxmt9-mini-replay-vertex-order --print-errorlogs`
Expected: `1/1 dxmt9-mini-replay-vertex-order  OK`

If `build/` does not exist yet, run `meson setup build` first.

- [ ] **Step 7: Correct the spec's unreachable fallback clause**

In `docs/superpowers/specs/2026-07-25-vertex-remap-design.md`, replace the paragraph beginning `Precondition: bv + |F| <= S.` with:

```markdown
`bv + |F| <= S` always holds and needs no fallback. Every index is
non-negative, so `bv <= min(F)`, and therefore
`bv + |F| <= max(F) + 1 <= S`. The implementation keeps the check as a
defensive assertion rather than a reachable branch, and `base_vertex` is never
rewritten.
```

- [ ] **Step 8: Commit**

```bash
git add scripts/tools/run_3dmark05_mini_replay.py \
        tests/scripts/test_mini_replay_vertex_order.py \
        tests/meson.build \
        docs/superpowers/specs/2026-07-25-vertex-remap-design.md
git commit -m "test(tools): prove vertex remap fetch equivalence

Adds pure permutation helpers for the mini-replay vertex remap lane plus a
registered unittest covering the load-bearing fetch-equivalence invariant:
every index position must read the same vertex bytes before and after the
permutation.

Also corrects the design doc's base_vertex fallback clause, which is
provably unreachable because non-negative indices force base_vertex to be
at most the minimum referenced slot."
```

---

### Task 2: `--vertex-order` CLI wiring

**Files:**
- Modify: `scripts/tools/run_3dmark05_mini_replay.py` — new `remap_draw_vertex_layout`, `materialize_replay_draws` (line 287), its call site (line 1489), the summary dict (line 1588), argparse (line 1703)
- Modify: `tests/scripts/test_mini_replay_vertex_order.py` — add an end-to-end class

**Interfaces:**
- Consumes: the five helpers from Task 1, plus existing `resolve_path`, `uint16_indices`, `transform_index_payload`, `load_manifest`.
- Produces: `remap_draw_vertex_layout(draw: dict[str, Any], ordinal: int, vertex_dir: Path, vertex_order: str) -> None`, mutating `draw` in place; and `materialize_replay_draws(draws, output_dir, primitive_order, draw_order, vertex_order)` with `vertex_order` as a new fifth positional parameter.

**Manifest schema the implementer needs.** Per draw, `draw["geometry"]` holds `index_file`, `index_bytes`, `stream0_file`, `stream0_bytes`, and `streams`, a list whose entries carry `stream`, `metal_slot`, `file`, `bytes`, `offset`, `stride`. The `streams` list includes stream 0 (with `metal_slot: 1`). `draw["state"]` holds `base_vertex`, `stream0_stride`, `stream0_offset`, and `index_type`. The source emitter reads `geometry["stream0_file"]` for slot 1 and `geometry["streams"]` only for `stream > 0`, so both must be updated to stay consistent.

- [ ] **Step 1: Write the failing test**

First add `json`, `subprocess`, and `tempfile` to the import block at the top of
`tests/scripts/test_mini_replay_vertex_order.py`, so the file's imports stay in
one place:

```python
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
```

Then append the following to the same file, before the `if __name__` block:

```python
SCRIPT = REPO_ROOT / "scripts" / "tools" / "run_3dmark05_mini_replay.py"


def write_manifest(root: Path) -> Path:
    """One draw, one stream, uint16 indices, base_vertex 0."""
    payload = make_payload()
    stream_path = root / "draw000.stream0.bin"
    stream_path.write_bytes(payload)
    index_path = root / "draw000.index.bin"
    index_path.write_bytes(struct.pack(f"<{len(INDICES)}H", *INDICES))
    manifest = {
        "schema": "dxmt9.3dmark05.mini_replay_manifest.v1",
        "draws": [{
            "row": "60/2",
            "seq": 60,
            "encoder": 2,
            "encoder_draw_index": 0,
            "draw_ordinal": 1,
            "geometry": {
                "index_file": str(index_path),
                "index_bytes": index_path.stat().st_size,
                "stream0_file": str(stream_path),
                "stream0_bytes": stream_path.stat().st_size,
                "streams": [{
                    "stream": 0,
                    "metal_slot": 1,
                    "file": str(stream_path),
                    "bytes": stream_path.stat().st_size,
                    "offset": 0,
                    "stride": STRIDE,
                }],
            },
            "state": {
                "index_count": len(INDICES),
                "base_vertex": 0,
                "stream0_stride": STRIDE,
                "stream0_offset": 0,
                "index_type": "uint16",
            },
            "uniforms": {},
        }],
    }
    manifest_path = root / "manifest.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    return manifest_path


class MaterializeVertexOrderTest(unittest.TestCase):
    def test_original_leaves_payloads_untouched(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = json.loads(write_manifest(root).read_text(encoding="utf-8"))
            replay = mini.materialize_replay_draws(
                manifest["draws"], root / "out", "original", "original", "original"
            )
            geometry = replay[0]["geometry"]
            self.assertNotIn("vertex_order", geometry)
            self.assertEqual(
                Path(geometry["stream0_file"]).read_bytes(), make_payload()
            )

    def test_first_reference_rewrites_stream_and_index(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = json.loads(write_manifest(root).read_text(encoding="utf-8"))
            original_stream = make_payload()
            replay = mini.materialize_replay_draws(
                manifest["draws"], root / "out", "original", "original", "first-reference"
            )
            geometry = replay[0]["geometry"]
            state = replay[0]["state"]
            self.assertEqual(geometry["vertex_order"], "first-reference")
            self.assertEqual(state["base_vertex"], 0)

            new_stream = Path(geometry["stream0_file"]).read_bytes()
            new_indices = mini.uint16_indices(
                Path(geometry["index_file"]).read_bytes()
            )
            self.assertEqual(len(new_stream), len(original_stream))
            self.assertEqual(geometry["stream0_bytes"], len(new_stream))
            self.assertEqual(geometry["index_bytes"], 2 * len(INDICES))
            self.assertEqual(geometry["streams"][0]["file"], geometry["stream0_file"])
            self.assertEqual(geometry["streams"][0]["bytes"], len(new_stream))

            for old_index, new_index in zip(INDICES, new_indices):
                self.assertEqual(
                    fetch(new_stream, 0, STRIDE, new_index),
                    fetch(original_stream, 0, STRIDE, old_index),
                )

    def test_composes_after_primitive_order(self):
        """Lane D: sorted primitive order plus a scattered layout."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = json.loads(write_manifest(root).read_text(encoding="utf-8"))
            original_stream = make_payload()
            replay = mini.materialize_replay_draws(
                manifest["draws"], root / "out", "sort-min-index", "original", "scatter"
            )
            geometry = replay[0]["geometry"]
            self.assertEqual(geometry["index_order"], "sort-min-index")
            self.assertEqual(geometry["vertex_order"], "scatter")

            sorted_indices = mini.uint16_indices(
                mini.transform_index_payload(
                    struct.pack(f"<{len(INDICES)}H", *INDICES), "sort-min-index"
                )
            )
            new_stream = Path(geometry["stream0_file"]).read_bytes()
            new_indices = mini.uint16_indices(
                Path(geometry["index_file"]).read_bytes()
            )
            for old_index, new_index in zip(sorted_indices, new_indices):
                self.assertEqual(
                    fetch(new_stream, 0, STRIDE, new_index),
                    fetch(original_stream, 0, STRIDE, old_index),
                )

    def test_index_reuse_estimate_is_unchanged_by_remap(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = json.loads(write_manifest(root).read_text(encoding="utf-8"))
            replay = mini.materialize_replay_draws(
                manifest["draws"], root / "out", "original", "original", "first-reference"
            )
            estimate = mini.index_cache_estimate(replay)
            self.assertEqual(
                estimate["original_lru32_misses"], estimate["replay_lru32_misses"]
            )


class VertexOrderCliTest(unittest.TestCase):
    def test_cli_records_vertex_order_in_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root)
            output_dir = root / "out"
            result = subprocess.run(
                [
                    sys.executable, str(SCRIPT), str(manifest_path),
                    "--output-dir", str(output_dir),
                    "--vertex-order", "first-reference",
                ],
                capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads(
                (output_dir / "dxmt9_3dmark05_mini_replay_summary.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(summary["vertex_order"], "first-reference")

    def test_cli_rejects_unknown_vertex_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root)
            result = subprocess.run(
                [
                    sys.executable, str(SCRIPT), str(manifest_path),
                    "--output-dir", str(root / "out"),
                    "--vertex-order", "sideways",
                ],
                capture_output=True, text=True,
            )
            self.assertNotEqual(result.returncode, 0)
```

Before running, confirm the summary filename by inspecting how the harness writes its summary JSON:

Run: `grep -n "summary" scripts/tools/run_3dmark05_mini_replay.py | grep -i "write_text\|json.dump\|_summary"`

If the emitted name differs from `dxmt9_3dmark05_mini_replay_summary.json`, use the actual name in `VertexOrderCliTest`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `python3 tests/scripts/test_mini_replay_vertex_order.py -v`
Expected: FAIL — `materialize_replay_draws() takes 4 positional arguments but 5 were given`

- [ ] **Step 3: Implement the per-draw driver**

Insert into `scripts/tools/run_3dmark05_mini_replay.py` immediately before `materialize_replay_draws`:

```python
def remap_draw_vertex_layout(draw: dict[str, Any],
                            ordinal: int,
                            vertex_dir: Path,
                            vertex_order: str) -> None:
    """Permute this draw's vertex storage and rewrite its indices in place.

    Preserves triangle order, triangle composition, vertex bytes, payload size,
    and `base_vertex`. Every stream is permuted with one shared reference order
    because a D3D9 indexed draw uses the same vertex index for every stream.
    """
    geometry = draw["geometry"]
    state = draw["state"]
    index_source = resolve_path(str(geometry["index_file"]))
    indices = uint16_indices(index_source.read_bytes())
    if not indices:
        raise SystemExit(f"draw {ordinal}: index payload is empty")
    base_vertex = int(state.get("base_vertex", 0))
    order = vertex_reference_order(indices, base_vertex, vertex_order)

    stream_entries = geometry.get("streams") or []
    if not stream_entries:
        stream_entries = [{
            "stream": 0,
            "metal_slot": 1,
            "file": geometry["stream0_file"],
            "bytes": int(geometry.get("stream0_bytes", 0)),
            "offset": int(state.get("stream0_offset", 0)),
            "stride": int(state.get("stream0_stride", 0)),
        }]
        geometry["streams"] = stream_entries

    for entry in stream_entries:
        stream_index = int(entry.get("stream", 0))
        stride = int(entry.get("stride", 0))
        offset = int(entry.get("offset", 0))
        if stream_index == 0:
            stride = int(state.get("stream0_stride", stride))
            offset = int(state.get("stream0_offset", offset))
        source = resolve_path(str(entry.get("file", "")))
        if not source.exists():
            raise SystemExit(
                f"draw {ordinal}: missing stream{stream_index} payload: {source}"
            )
        payload = source.read_bytes()
        slot_count = vertex_slot_capacity(len(payload), offset, stride)
        new_for_old = vertex_slot_assignment(order, base_vertex, slot_count)
        permuted = apply_vertex_permutation(payload, offset, stride, new_for_old)
        target = vertex_dir / f"draw{ordinal:03d}-{vertex_order}.stream{stream_index}.bin"
        target.write_bytes(permuted)
        entry["file"] = str(target)
        entry["bytes"] = len(permuted)
        entry["vertex_order_source_file"] = str(source)
        if stream_index == 0:
            geometry["stream0_file"] = str(target)
            geometry["stream0_bytes"] = len(permuted)

    rewritten = rewrite_indices_for_permutation(indices, base_vertex, order)
    index_target = vertex_dir / f"draw{ordinal:03d}-{vertex_order}.index.bin"
    index_target.write_bytes(struct.pack(f"<{len(rewritten)}H", *rewritten))
    geometry.setdefault("index_order_source_file", str(index_source))
    geometry["index_file"] = str(index_target)
    geometry["index_bytes"] = index_target.stat().st_size
    geometry["vertex_order"] = vertex_order
    geometry["vertex_order_referenced_vertices"] = len(order)
```

- [ ] **Step 4: Wire it into `materialize_replay_draws`**

Change the signature and add the vertex pass. The function currently reads:

```python
def materialize_replay_draws(draws: list[dict[str, Any]],
                             output_dir: Path,
                             primitive_order: str,
                             draw_order: str) -> list[dict[str, Any]]:
```

Replace that signature with:

```python
def materialize_replay_draws(draws: list[dict[str, Any]],
                             output_dir: Path,
                             primitive_order: str,
                             draw_order: str,
                             vertex_order: str = "original") -> list[dict[str, Any]]:
```

Then insert this block after the `if primitive_order != "original":` block and before the `if draw_order == "reverse":` block. Order matters: the vertex pass must read the already-rewritten index payload so lane D composes correctly.

```python
    if vertex_order != "original":
        vertex_dir = output_dir / "vertex-order"
        vertex_dir.mkdir(parents=True, exist_ok=True)
        for ordinal, draw in enumerate(replay_draws):
            remap_draw_vertex_layout(draw, ordinal, vertex_dir, vertex_order)
```

- [ ] **Step 5: Add the CLI flag and thread it through**

In the argument parser, immediately after the `--draw-order` argument:

```python
    parser.add_argument(
        "--vertex-order",
        choices=("original", "first-reference", "scatter"),
        default="original",
        help=(
            "permute vertex storage and rewrite indices before replay while "
            "preserving triangle order, vertex bytes, payload size, and "
            "base_vertex; first-reference monotonizes index references, "
            "scatter decorrelates them for the locality discriminator"
        ),
    )
```

At the call site, pass the new argument:

```python
    replay_draws = materialize_replay_draws(
        draws,
        args.output_dir,
        args.primitive_order,
        args.draw_order,
        args.vertex_order,
    )
```

In the summary dict, immediately after the `"primitive_order": args.primitive_order,` entry:

```python
        "vertex_order": args.vertex_order,
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `python3 tests/scripts/test_mini_replay_vertex_order.py -v`
Expected: PASS, all tests OK

Run: `meson test -C build --suite scripts --print-errorlogs`
Expected: all scripts-suite tests OK, including `dxmt9-mini-replay-vertex-order`

- [ ] **Step 7: Verify the help text renders**

Run: `python3 scripts/tools/run_3dmark05_mini_replay.py --help | grep -A6 "vertex-order"`
Expected: the flag with its three choices

- [ ] **Step 8: Commit**

```bash
git add scripts/tools/run_3dmark05_mini_replay.py \
        tests/scripts/test_mini_replay_vertex_order.py
git commit -m "feat(tools): add mini-replay --vertex-order lanes

Permutes vertex storage and rewrites indices while preserving triangle
order, vertex bytes, payload size, and base_vertex. first-reference
monotonizes index references; scatter decorrelates them. The vertex pass
runs after the primitive-order pass so a sorted-order plus scattered-layout
discriminator lane composes correctly.

Coverage includes stream/index rewrite, multi-stream consistency, lane
composition, and an assertion that LRU reuse estimates are unchanged by a
pure permutation."
```

---

### Task 3: Capture the GT1 frame60 encoder2 dump

This task produces trace artifacts, not code. Nothing is committed. Two runs are needed because the depth-attachment dump requires a handle that is only known after reading an encoder breakdown.

**Files:**
- Produces: `traces/app-d3d9-3dmark05-vertexremap-scout-r1/` with `frame60.gputrace`, `analysis/geometry/`, `analysis/shaders/msl/`, `analysis/frame60-depth.bin`

- [ ] **Step 1: Confirm free space and a dry run**

```bash
df -h .
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix vertexremap-probe-r0 --frame 60 --no-gputrace --timeout 120 \
  --encoder-breakdown-seq 60 --measure-index-reuse --keep-frontmost --dry-run
```

Expected: the resolved command prints and the free-space guard passes. At least 2 GiB must be free for the later gputrace run.

- [ ] **Step 2: Run the breakdown scout to learn the depth handle and draw shape**

```bash
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix vertexremap-probe-r0 --frame 60 --no-gputrace --timeout 120 \
  --encoder-breakdown-seq 60 --measure-index-reuse --keep-frontmost
```

- [ ] **Step 3: Read encoder 2's depth handle, draw count, and index width**

```bash
OUT=experiments/output/app-d3d9-3dmark05-vertexremap-probe-r0
python3 - <<'EOF'
import csv, glob, collections
enc = glob.glob("experiments/output/app-d3d9-3dmark05-vertexremap-probe-r0/3dmark05-perf-encoders.csv")[0]
for row in csv.DictReader(open(enc)):
    if row.get("seq") == "60" and row.get("encoder") == "2":
        print("depth handle:", row.get("depth"))
        print("depth format/size:", row.get("depth_format"), row.get("depth_width"), row.get("depth_height"))
        print("draws:", row.get("draws"), "primitives:", row.get("primitives"))
draws = glob.glob("experiments/output/app-d3d9-3dmark05-vertexremap-probe-r0/3dmark05-perf-indexed-probe-draws.csv")[0]
types = collections.Counter()
count = 0
for row in csv.DictReader(open(draws)):
    if row.get("seq") == "60" and row.get("encoder") == "2":
        types[row.get("index_type", "")] += 1
        count += 1
print("encoder2 indexed draws:", count, "index types:", dict(types))
EOF
```

Record the `depth handle` value as `DEPTH_HANDLE` for the next step.

**Gate:** every encoder2 index type must be uint16. If any draw is uint32, exclude it via `--dump-indexed-geometry-vs` / `--dump-indexed-geometry-ps` filters or by narrowing the encoder-draw range, and record the exclusion in the leaf document. The replay binary cannot bind uint32 indices.

- [ ] **Step 4: Run the dump capture**

Substitute the recorded handle for `<DEPTH_HANDLE>`. `DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MIN/MAX` scopes the dump to the same 113-draw window `replay.03` used; the row filter selects seq 60 / encoder 2.

```bash
DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MIN=0 \
DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MAX=112 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix vertexremap-scout-r1 --frame 60 --timeout 420 \
  --probe-reverse-indexed-triangles-row 60/2 \
  --dump-indexed-geometry \
  --dump-indexed-geometry-cbufs \
  --dump-indexed-geometry-max-draws 113 \
  --dump-shaders \
  --dump-depth-attachment-handle <DEPTH_HANDLE> \
  --dump-depth-attachment-seq 60 \
  --dump-depth-attachment-enc 2 \
  --encoder-breakdown-seq 60 \
  --measure-index-reuse \
  --keep-frontmost
```

- [ ] **Step 5: Verify the artifacts landed**

```bash
TRACE=traces/app-d3d9-3dmark05-vertexremap-scout-r1
ls "$TRACE/frame60.gputrace" >/dev/null && echo "gputrace ok"
ls "$TRACE/analysis/geometry" | head
ls "$TRACE/analysis/geometry"/*.index.bin | wc -l
ls "$TRACE/analysis/shaders/msl" | head -3
ls -la "$TRACE/analysis"/frame60-depth.bin
```

Expected: a `frame60.gputrace` bundle, one index payload plus at least one stream0 payload per dumped draw, dumped MSL, and a non-empty depth sidecar. If the geometry directory is empty, the row filter did not match; re-check the `seq/encoder` values from Step 3 before rerunning.

---

### Task 4: Build the mini-replay manifest

**Files:**
- Produces: `traces/app-d3d9-3dmark05-vertexremap-scout-r1/analysis/frame60-mini-replay-manifest-encoder2.json`

**Note on Xcode budget.** The manifest builder requires a shader-summary CSV, which `analyze_shader_dumps.py` derives from the Xcode/dxmt joined CSV. That means one Xcode counter export is needed here, before the four lane exports — **five manual exports total**, not four. There is no CLI path to encoder counters.

- [ ] **Step 1: Export encoder counters for the dump capture**

Follow `agents/rules/metal_debugging.rules.md` section 2b exactly:

1. Open `traces/app-d3d9-3dmark05-vertexremap-scout-r1/frame60.gputrace` in Xcode.
2. Summary, then **Show Performance**.
3. Open **Counters** and wait until draw-counter profiling stops. Allow at least 60 seconds after the first rows appear.
4. **Export Encoder Counters** to `traces/app-d3d9-3dmark05-vertexremap-scout-r1/analysis/frame60-counters-xcode.csv`.

Verify the file actually landed in this run's directory — Xcode's save panel can retain an older `analysis` folder:

```bash
ls -la traces/app-d3d9-3dmark05-vertexremap-scout-r1/analysis/frame60-counters-xcode.csv
```

- [ ] **Step 2: Finalize to produce the joined and shader summaries**

```bash
bash scripts/tools/finalize_3dmark05_perf_probe.sh \
  --suffix vertexremap-scout-r1 --frame 60
ls traces/app-d3d9-3dmark05-vertexremap-scout-r1/analysis/ | grep -E "joined|shader-dump"
```

Expected: `frame60-xcode-dxmt-joined-summary.csv` and `frame60-shader-dump-summary.csv`.

- [ ] **Step 3: Build the manifest**

```bash
TRACE=traces/app-d3d9-3dmark05-vertexremap-scout-r1
OUT=experiments/output/app-d3d9-3dmark05-vertexremap-scout-r1
python3 scripts/tools/build_3dmark05_mini_replay_manifest.py \
  --shader-summary "$TRACE/analysis/frame60-shader-dump-summary.csv" \
  --probe-draws "$OUT/3dmark05-perf-indexed-probe-draws.csv" \
  --geometry-dir "$TRACE/analysis/geometry" \
  --shader-msl-dir "$TRACE/analysis/shaders/msl" \
  --row 60/2 \
  --encoder-draw-min 0 \
  --encoder-draw-max 112 \
  --output "$TRACE/analysis/frame60-mini-replay-manifest-encoder2.json"
```

- [ ] **Step 4: Verify manifest preconditions**

```bash
python3 - <<'EOF'
import json, collections
from pathlib import Path
path = Path("traces/app-d3d9-3dmark05-vertexremap-scout-r1/analysis/frame60-mini-replay-manifest-encoder2.json")
manifest = json.loads(path.read_text())
draws = manifest["draws"]
print("draws:", len(draws))
types = collections.Counter(d["state"].get("index_type", "") for d in draws)
print("index types:", dict(types))
print("base_vertex values:", sorted({int(d["state"].get("base_vertex", 0)) for d in draws}))
print("stream0 strides:", sorted({int(d["state"].get("stream0_stride", 0)) for d in draws}))
print("stream0 offsets:", sorted({int(d["state"].get("stream0_offset", 0)) for d in draws}))
extra = collections.Counter()
for d in draws:
    for s in d["geometry"].get("streams", []):
        if int(s.get("stream", 0)) > 0 and int(s.get("bytes", 0)) > 0:
            extra[int(s["stream"])] += 1
            if int(s.get("stride", 0)) <= 0:
                raise SystemExit(f"stream {s['stream']} has no stride; cannot permute")
print("extra streams in use:", dict(extra))
EOF
```

**Gates:**
- `draws` is greater than zero. An empty manifest means the row or draw-range filter is wrong.
- Every `index_type` is uint16.
- Every in-use extra stream reports a positive `stride`. If any is zero, the permutation cannot address that stream; stop and record it rather than producing a silently wrong lane.

Record the reported draw count. If it is not 113, that is acceptable — note the actual count in the leaf document and use it consistently, but be aware `replay.03`'s `1,710` / `442.6 B/inv` brackets were measured on 113 draws, so lane B is the authority for the upper bracket rather than the historical number.

---

### Task 5: Run the four lanes

**Files:**
- Produces: `traces/app-d3d9-3dmark05-vertexremap-scout-r1/analysis/lanes/lane{A,B,C,D}/`

- [ ] **Step 1: Run all four lanes**

```bash
TRACE=$PWD/traces/app-d3d9-3dmark05-vertexremap-scout-r1
MANIFEST=$TRACE/analysis/frame60-mini-replay-manifest-encoder2.json
DEPTH=$TRACE/analysis/frame60-depth.bin
LANES=$TRACE/analysis/lanes
mkdir -p "$LANES"

run_lane() {
  name=$1; prim=$2; vert=$3
  python3 scripts/tools/run_3dmark05_mini_replay.py "$MANIFEST" \
    --output-dir "$LANES/lane$name" \
    --primitive-order "$prim" \
    --vertex-order "$vert" \
    --depth-input "$DEPTH" \
    --texture-input-dir "$TRACE/analysis/geometry" \
    --compile --run \
    --capture-path "$LANES/lane$name/lane$name.gputrace" \
    --color-output "$LANES/lane$name/lane$name.color.bin"
}

run_lane A original       original
run_lane B sort-min-index original
run_lane C original       first-reference
run_lane D sort-min-index scatter
```

If `--texture-input-dir` finds no sidecars the harness proceeds with dummy textures; that is acceptable because all four lanes share it. Every lane must use the same depth sidecar.

- [ ] **Step 2: Apply the correctness gate — lane C must be bit-identical to lane A**

```bash
LANES=traces/app-d3d9-3dmark05-vertexremap-scout-r1/analysis/lanes
shasum -a 256 "$LANES"/lane*/lane*.color.bin
python3 - <<'EOF'
import hashlib
from pathlib import Path
lanes = Path("traces/app-d3d9-3dmark05-vertexremap-scout-r1/analysis/lanes")
digests = {}
for name in ("A", "B", "C", "D"):
    path = lanes / f"lane{name}" / f"lane{name}.color.bin"
    digests[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    print(name, digests[name])
if digests["A"] != digests["C"]:
    raise SystemExit(
        "CORRECTNESS GATE FAILED: lane C is a data-equivalent permutation and "
        "must produce identical pixels to lane A. This is a remap bug, not a "
        "finding. Do not interpret any counter delta."
    )
print("correctness gate: lane C == lane A")
EOF
```

**This gate is blocking.** If it fails, stop and debug the permutation. The most likely causes are a wrong `offset` for an extra stream, an extra-stream stride in the manifest that disagrees with what the dumped MSL assumes, or the vertex pass running before the primitive pass.

- [ ] **Step 3: Record the per-lane index reuse estimates**

```bash
LANES=traces/app-d3d9-3dmark05-vertexremap-scout-r1/analysis/lanes
for L in A B C D; do
  echo "== lane $L"
  python3 -c "
import json,sys
s=json.load(open('$LANES/lane$L/dxmt9_3dmark05_mini_replay_summary.json'))
e=s['index_cache_estimate']
print(' vertex_order', s['vertex_order'], 'primitive_order', s['primitive_order'])
print(' lru32 original', e['original_lru32_misses'], 'replay', e['replay_lru32_misses'])
"
done
```

Lane C's replay and original LRU32 misses must be equal: a pure permutation cannot change vertex reuse. Lanes B and D legitimately differ because their primitive order changed.

---

### Task 6: Export counters, compute the verdict, and record it

**Files:**
- Produces: `traces/app-d3d9-3dmark05-vertexremap-scout-r1/analysis/lane{A,B,C,D}-counters-xcode.csv` and `analysis/vertex-remap-verdict.md`
- Create: `docs/perfomance/mini-replay-bisection/mini-replay-bisection-vertexremap.01.md`
- Modify: `agents/rules/metal_debugging.rules.md`

- [ ] **Step 1: Export encoder counters for each lane**

For each lane in A, B, C, D, repeat the `metal_debugging.rules.md` section 2b sequence on `analysis/lanes/lane<L>/lane<L>.gputrace`, exporting to
`traces/app-d3d9-3dmark05-vertexremap-scout-r1/analysis/lane<L>-counters-xcode.csv`.

Close the bundle in Xcode between lanes so counter values are re-measured rather than served from cache.

```bash
ls -la traces/app-d3d9-3dmark05-vertexremap-scout-r1/analysis/lane?-counters-xcode.csv
```

Expected: four non-empty CSVs.

- [ ] **Step 2: Compute the recovery fractions**

```bash
python3 - <<'EOF'
import csv
from pathlib import Path

ANALYSIS = Path("traces/app-d3d9-3dmark05-vertexremap-scout-r1/analysis")


def lane_totals(name: str) -> dict[str, float]:
    gpu_ms = vs_write_mib = vs_invocations = 0.0
    with (ANALYSIS / f"lane{name}-counters-xcode.csv").open(encoding="utf-8-sig") as handle:
        for row in csv.DictReader(handle):
            def num(*keys):
                for key in keys:
                    for actual in row:
                        if actual.strip().lower() == key:
                            try:
                                return float(str(row[actual]).replace(",", "") or 0.0)
                            except ValueError:
                                return 0.0
                return 0.0
            gpu_ms += num("gpu time (ms)", "gpu_ms", "total gpu time (ms)")
            vs_write_mib += num("vs buffer device memory bytes written (mib)",
                                "vs_buffer_write_mib")
            vs_invocations += num("vs invocations", "vs_invocations")
    return {"gpu_ms": gpu_ms, "vs_write_mib": vs_write_mib,
            "vs_invocations": vs_invocations}


lanes = {name: lane_totals(name) for name in ("A", "B", "C", "D")}
for name, values in lanes.items():
    inv = values["vs_invocations"]
    values["b_per_inv"] = (values["vs_write_mib"] * 1024 * 1024 / inv) if inv else 0.0

print(f"{'lane':5} {'gpu_ms':>9} {'vs_write_mib':>13} {'vs_inv':>12} {'B/inv':>9} {'recovery':>9}")
denominator = lanes["A"]["b_per_inv"] - lanes["B"]["b_per_inv"]
for name, values in lanes.items():
    recovery = ((lanes["A"]["b_per_inv"] - values["b_per_inv"]) / denominator
                if denominator else float("nan"))
    values["recovery"] = recovery
    print(f"{name:5} {values['gpu_ms']:9.3f} {values['vs_write_mib']:13.3f} "
          f"{values['vs_invocations']:12.0f} {values['b_per_inv']:9.1f} {recovery:9.3f}")

inv_a, inv_c = lanes["A"]["vs_invocations"], lanes["C"]["vs_invocations"]
drift = abs(inv_c - inv_a) / inv_a * 100.0 if inv_a else 0.0
print(f"\nlane C vs A VS-invocation drift: {drift:.3f}% (gate: <= 1%)")

rc, rd = lanes["C"]["recovery"], lanes["D"]["recovery"]
if rc >= 0.7 and rd <= 0.15:
    print("VERDICT: (a) index/PB write locality owns it -> design the runtime lane")
elif rc <= 0.15 and rd >= 0.7:
    print("VERDICT: (b) primitive order owns it -> ABANDON vertex remap")
elif rc <= 0.15 and rd <= 0.15:
    print("VERDICT: lane B did not reproduce -> harness/manifest defect, re-verify")
elif 0.15 < rc < 0.7 and 0.15 < rd < 0.7:
    print("VERDICT: both contribute -> proceed partially, expected gain is lane C's value")
else:
    print("VERDICT: not separable by this experiment -> record numbers and redesign")
EOF
```

If the printed column headers do not match your export, inspect the real header row with
`head -1 traces/app-d3d9-3dmark05-vertexremap-scout-r1/analysis/laneA-counters-xcode.csv | tr ',' '\n' | nl`
and extend the `num(...)` key lists. Also consult
`scripts/tools/summarize_xcode_encoder_counters.py` for the canonical column names this repository already parses.

**Escalation gate.** If any lane's `B/inv` differs from lane A's by less than `10%`, that lane is inside Xcode replay noise. Re-export it five times and run
`python3 scripts/tools/analyze_xcode_replay_variance.py <lane CSVs> --output <report> --max-cv-pct 5`
before accepting the verdict.

**Sanity gate.** If lane B's `B/inv` is not near `442.6`, the manifest does not represent the same row as `replay.03`. Do not interpret any lane. Re-check the row filter and draw range from Task 3.

- [ ] **Step 3: Write the leaf document**

Create `docs/perfomance/mini-replay-bisection/mini-replay-bisection-vertexremap.01.md`, following the one-experiment-per-file convention used by `mini-replay-bisection-replay.03.md`:

```markdown
---
domain: mini-replay-bisection
workload: 3DMark05 GT1
subcategory: vertexremap
order: 01
title: Vertex Remap Separates Index Locality From Primitive Order
date: 2026-07-25
type: experiment-run
status: <accepted|rejected>
source: traces/app-d3d9-3dmark05-vertexremap-scout-r1/analysis/lane{A,B,C,D}-counters-xcode.csv; traces/app-d3d9-3dmark05-vertexremap-scout-r1/analysis/frame60-mini-replay-manifest-encoder2.json; docs/superpowers/specs/2026-07-25-vertex-remap-design.md
related: docs/perfomance/mini-replay-bisection/mini-replay-bisection-replay.03.md; docs/perfomance/hidden-backend-storage/overview.md
---

# Vertex Remap Separates Index Locality From Primitive Order

**Question / hypothesis.** `replay.03`'s `3.86x` hidden VS-write density delta
came from `sort-min-index`, which changes both index reference monotonicity and
primitive order. Which one owns the delta? If index locality owns it, a pure
vertex permutation recovers the win with no oracle requirement, because
triangle order and triangle composition are preserved exactly.

**Method.** Four lanes over one GT1 frame60 encoder2 manifest of <N> draws,
same build, same machine, same depth sidecar. Lane A original; lane B
`sort-min-index`; lane C original primitive order with a first-reference vertex
permutation; lane D `sort-min-index` with a scattered vertex permutation. Lane C
was gated bit-exact against lane A by SHA-256 of the replay color output.
Recovery is `(d(A) - d(X)) / (d(A) - d(B))` over VS write bytes per invocation.

**Result.**

| Lane | GPU ms | VS write MiB | VS invocations | B / invocation | recovery |
|---|---:|---:|---:|---:|---:|
| A | | | | | `0.000` |
| B | | | | | `1.000` |
| C | | | | | |
| D | | | | | |

Lane C color output SHA-256 matched lane A. Lane C VS-invocation drift versus
lane A was <X>%, inside the `1%` gate. Lane C's replay and original LRU32 miss
counts were equal, as a pure permutation requires.

**Verdict.** <verdict sentence and the action it implies>

**Related.** [mini-replay-bisection](index.md) · [mini-replay-bisection-replay.03](mini-replay-bisection-replay.03.md) ·
[hidden-backend-storage](../hidden-backend-storage/index.md) · [overview-3dmark05-gt1](../overview-3dmark05-gt1.md)
```

Fill every table cell and every `<...>` placeholder — `status:`, `<N>`, `<X>`,
and the verdict sentence — from Step 2's output. Do not commit the file with any
placeholder remaining.

- [ ] **Step 4: Document the new flag in the rules file**

In `agents/rules/metal_debugging.rules.md` section 9, the "Mini-replay + bisection" row of the experiment-class table lists `run_3dmark05_mini_replay.py` flags. Add `--vertex-order {original,first-reference,scatter}` to that flag list, and add one sentence to the surrounding prose stating that the vertex-order lanes are bit-exact permutations gated by a color-hash comparison against the `original` lane.

- [ ] **Step 5: Update the domain conclusions if the verdict changes direction**

Only if lane C recovered `>= 0.7`: add a row to the "Latest Conclusions" table in `docs/perfomance/hidden-backend-storage/overview.md` and update the "Proven GPU lever" row of the axis table in `docs/perfomance/overview-3dmark05-gt1.md`. If the verdict was abandon, add the negative finding to the same conclusions table so the lane is not retried.

- [ ] **Step 6: Commit**

```bash
git add docs/perfomance/mini-replay-bisection/mini-replay-bisection-vertexremap.01.md \
        agents/rules/metal_debugging.rules.md
# add the two overview files only if step 5 changed them
git commit -m "docs(perf): record vertex remap locality discriminator result

Four-lane GT1 frame60 encoder2 mini-replay separates index reference
locality from primitive order as owners of replay.03's 3.86x hidden
VS-write density delta."
```

- [ ] **Step 7: Verify the docs audit still passes**

Run: `meson test -C build --suite scripts --print-errorlogs`
Expected: all OK, including `dxmt9-mini-replay-vertex-order` and the perf-docs source audit (`test_audit_perf_docs_sources.py` checks that leaf `source:` paths are real).

---

## Verdict-dependent next step

This plan ends at a verdict, deliberately. Do not begin runtime work inside this plan.

- **Lane C recovered `>= 0.7`:** open a new design for the runtime lane. The known open problem is the shared-vertex-buffer memory bound: the permutation is per-draw, so a runtime cache keyed like `ReorderedIndexBufferCacheKey` (`VB contentRevision`, `IB contentRevision`, IB span) holds one remapped vertex buffer per draw span, and several draws sharing one vertex buffer hold several copies. Size that bound before writing code.
- **Lane D recovered `>= 0.7` and lane C did not:** record the negative result and stop. The win requires changing primitive order, which needs the final-color oracle that currently blocks that whole class.
- **Both intermediate:** the expected production gain is lane C's measured recovery, not `3.86x`. Re-scope before committing to runtime work.
