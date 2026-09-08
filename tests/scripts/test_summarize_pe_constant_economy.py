"""R-BENCH-6.4 complete-matrix and fail-closed receipt tests."""
import importlib.util
from pathlib import Path
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/tools/summarize_pe_constant_economy.py"
spec = importlib.util.spec_from_file_location("pe_constant_economy", SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def row(family, count, shape):
    return (f"{module.TAG} family={family} count={count} shape={shape} "
            "calls=50000000 warmup=10000 start=8 thread_id=123 process_id=456 cpu_scope=process qpc_ticks=10000000 "
            "qpc_frequency=10000000 kernel_100ns=10000 user_100ns=100000 "
            "hr_or=0 precheck=1 postcheck=1 source_unchanged=1")


def fixture():
    return "\n".join([*(row(*key) for key in sorted(module.EXPECTED)), module.FOOTER,
                      "REACHED:inline_constants:device_release=0", "PASS:inline_constants", module.TERMINAL])


class ConstantEconomyTests(unittest.TestCase):
    def test_exact_matrix_and_raw_units(self):
        report = module.parse_text(fixture())
        self.assertEqual(len(report["cells"]), 66)
        self.assertEqual(report["cells"][0]["wall_ns_per_call"], 20.0)
        self.assertEqual(report["cells"][0]["process_cpu_ns_per_call"], 0.22)
        self.assertEqual(report["cpu_scope"], "process")
        self.assertEqual(sum(cell["count"] == 0 for cell in report["cells"]), 6)

    def test_missing_every_cell(self):
        for key in sorted(module.EXPECTED):
            with self.subTest(key=key), self.assertRaises(ValueError):
                module.parse_text(fixture().replace(row(*key) + "\n", ""))

    def test_duplicate_cell_field_and_footer(self):
        first = row(*sorted(module.EXPECTED)[0])
        for text in (fixture().replace(first, first + "\n" + first),
                     fixture().replace("count=0", "count=0 count=0", 1),
                     fixture() + "\n" + module.FOOTER,
                     fixture() + "\n" + first):
            with self.subTest(text=text[:60]), self.assertRaises(ValueError):
                module.parse_text(text)

    def test_bad_scalar_or_witness(self):
        for old, new in (("calls=50000000", "calls=0"), ("warmup=10000", "warmup=999"),
                         ("start=8", "start=9"), ("count=3", "count=2"),
                         ("hr_or=0", "hr_or=1"), ("precheck=1", "precheck=0"),
                         ("postcheck=1", "postcheck=0"), ("thread_id=123", "thread_id=0"),
                         ("process_id=456", "process_id=0"),
                         ("cpu_scope=process", "cpu_scope=thread"),
                         ("source_unchanged=1", "source_unchanged=0"),
                         ("qpc_ticks=10000000", "qpc_ticks=0"),
                         ("qpc_frequency=10000000", "qpc_frequency=0"),
                         ("user_100ns=100000", "user_100ns=-1"),
                         ("user_100ns=100000", "user_100ns=1e5"),
                         ("user_100ns=100000", "user_100ns=18446744073709551616"),
                         ("shape=equal", "shape=unknown"), ("family=vs_float", "family=other")):
            with self.subTest(new=new), self.assertRaises(ValueError):
                module.parse_text(fixture().replace(old, new, 1))

    def test_missing_termination_and_failures(self):
        for terminal in (module.FOOTER, "REACHED:inline_constants:device_release=0",
                         "PASS:inline_constants", module.TERMINAL):
            with self.subTest(terminal=terminal), self.assertRaises(ValueError):
                module.parse_text(fixture().replace(terminal, ""))
        for suffix in ("FAIL: getter", "SKIP: timer", "PIXEL_SIGNATURE wrong-selector"):
            with self.subTest(suffix=suffix), self.assertRaises(ValueError):
                module.parse_text(fixture() + "\n" + suffix)

    def test_changed_identity_and_frequency(self):
        for old, new in (("thread_id=123", "thread_id=124"),
                         ("process_id=456", "process_id=457"),
                         ("qpc_frequency=10000000", "qpc_frequency=10000001")):
            with self.subTest(new=new), self.assertRaises(ValueError):
                module.parse_text(fixture().replace(old, new, 1))

    def test_truncation_extra_field_and_zero_cpu(self):
        text = fixture()
        for broken in (text.replace(module.TAG, module.TAG[:-1], 1),
                       text.replace("hr_or=0", "extra=0 hr_or=0", 1),
                       text.replace("postcheck=1", "postcheck", 1),
                       text.replace("kernel_100ns=10000", "kernel_100ns=0").replace(
                           "user_100ns=100000", "user_100ns=0")):
            with self.assertRaises(ValueError):
                module.parse_text(broken)

    def test_legacy_unqualified_cpu_scope_rejected(self):
        for field in ("cpu_scope=process ", "process_id=456 "):
            with self.assertRaises(ValueError):
                module.parse_text(fixture().replace(field, ""))


if __name__ == "__main__":
    unittest.main()
