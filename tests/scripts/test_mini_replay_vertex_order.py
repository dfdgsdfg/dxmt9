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
