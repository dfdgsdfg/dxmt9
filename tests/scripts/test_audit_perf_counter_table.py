#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
AUDIT_PATH = REPO_ROOT / "scripts" / "check" / "audit_perf_counter_table.py"

SPEC = importlib.util.spec_from_file_location("audit_perf_counter_table", AUDIT_PATH)
assert SPEC is not None and SPEC.loader is not None
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


class PerfCounterTableAuditTest(unittest.TestCase):
    def assert_ring_writer_evidence(
        self, implementation: str, expected: bool
    ) -> None:
        internal = """
struct Counters {
  PercentileRing sampleRing;
};
"""
        report = """
constexpr CounterEntry kCounterTable[] = {
  {"sample_p50", Kind::PercentileMs, nullptr, &Counters::sampleRing, 0.5},
};
"""
        _, _, missing, unknown, never_written = AUDIT.audit_texts(
            internal, report, [implementation]
        )
        self.assertEqual(missing, set())
        self.assertEqual(unknown, set())
        self.assertEqual(
            never_written,
            set() if expected else {"sampleRing"},
        )

    def test_multiple_table_rows_do_not_count_as_writer_evidence(self) -> None:
        internal = """
struct Counters {
  PercentileRing lonelyRing;
};
"""
        report = """
constexpr CounterEntry kCounterTable[] = {
  {"lonely_p50", Kind::PercentileMs, nullptr, &Counters::lonelyRing, 0.5},
  {"lonely_p95", Kind::PercentileMs, nullptr, &Counters::lonelyRing, 0.95},
};
"""
        _, rings, missing, unknown, never_written = AUDIT.audit_texts(
            internal, report, ["void unrelated() {}"]
        )
        self.assertEqual(rings, {"lonelyRing"})
        self.assertEqual(missing, set())
        self.assertEqual(unknown, set())
        self.assertEqual(never_written, {"lonelyRing"})

    def test_recorder_call_is_writer_evidence(self) -> None:
        self.assert_ring_writer_evidence(
            "recordRing(counters().sampleRing, nanoseconds);", True
        )

    def test_line_comment_is_not_writer_evidence(self) -> None:
        self.assert_ring_writer_evidence(
            "// recordRing(counters().sampleRing, nanoseconds);", False
        )

    def test_block_comment_is_not_writer_evidence(self) -> None:
        self.assert_ring_writer_evidence(
            "/* recordRing(counters().sampleRing, nanoseconds); */", False
        )

    def test_string_literal_is_not_writer_evidence(self) -> None:
        self.assert_ring_writer_evidence(
            'const char* text = "recordRing(counters().sampleRing, nanoseconds);";',
            False,
        )

    def test_address_only_is_not_writer_evidence(self) -> None:
        self.assert_ring_writer_evidence(
            "void observe(Counters& c) { auto* selected = &c.sampleRing; "
            "(void)selected; }",
            False,
        )

    def test_reference_return_only_is_not_writer_evidence(self) -> None:
        self.assert_ring_writer_evidence(
            "PercentileRing& select(Counters& c) { return c.sampleRing; }",
            False,
        )

    def test_indirect_pointer_mutation_is_writer_evidence(self) -> None:
        self.assert_ring_writer_evidence(
            "void record(Counters& c) { auto* selected = &c.sampleRing; "
            "recordRing(*selected, nanoseconds); }",
            True,
        )

    def test_selector_return_mutation_is_writer_evidence(self) -> None:
        self.assert_ring_writer_evidence(
            "PercentileRing& select(Counters& c) { return c.sampleRing; } "
            "void record(Counters& c) { "
            "recordRing(select(c), nanoseconds); }",
            True,
        )

    def test_pointer_array_mutation_is_writer_evidence(self) -> None:
        self.assert_ring_writer_evidence(
            "void record(Counters& c) { PercentileRing* selected[] = { "
            "&c.sampleRing }; recordRing(*selected[index], nanoseconds); }",
            True,
        )


if __name__ == "__main__":
    unittest.main()
