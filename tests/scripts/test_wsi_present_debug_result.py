import tempfile
import unittest
from pathlib import Path
import sys

from PIL import Image


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))
sys.path.insert(0, str(REPO_ROOT / "tests" / "integration" / "wsi_present"))

from scripts.check import check_debug_result_schema as schema  # noqa: E402
import run_wsi_present  # noqa: E402


class WsiPresentDebugResultTests(unittest.TestCase):
    def test_scaffold_result_uses_debug_schema(self):
        result = run_wsi_present.build_debug_result(
            command=["run_wsi_present.py", "emit-scaffold-result"],
            environment={},
            window_title="dxmt9 WSI test",
            failure_category="wsi-layer-acquisition",
            layer_acquisition="unavailable",
            capture_source="unavailable",
            reason_key="unavailable_reason",
            reason="scaffold",
            exit_code=0,
        )

        self.assertEqual(schema.validate_result(result, require={"wsi"}), [])
        self.assertEqual(result["module"], "wsi-present")

    def test_output_file_validates_with_wsi_requirement(self):
        result = run_wsi_present.build_debug_result(
            command=["wine64", "wsi_present_x64.exe"],
            environment={"WINEDLLOVERRIDES": "d3d9=n,b"},
            window_title="dxmt9 WSI test",
            failure_category="none",
            layer_acquisition="unavailable",
            capture_source="none",
            reason_key="unavailable_reason",
            reason="runner does not acquire CAMetalLayer directly",
            exit_code=0,
        )

        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "debug_result.json"
            run_wsi_present.emit_debug_result(result, output)
            loaded = schema.load_json(output)

        self.assertEqual(schema.validate_result(loaded, require={"wsi"}), [])

    def test_failure_classifier_separates_layer_and_present_failures(self):
        category, layer, reason = run_wsi_present.classify_failure("", "FAIL: CreateDevice hr=0x8876086c", 1)
        self.assertEqual(category, "wsi-layer-acquisition")
        self.assertEqual(layer, "acquisition_failure")
        self.assertIn("device", reason)

        category, layer, reason = run_wsi_present.classify_failure("", "FAIL: Present hr=0x88760868", 1)
        self.assertEqual(category, "wsi-visible-output")
        self.assertEqual(layer, "acquisition_failure")
        self.assertIn("Present", reason)

    def test_live_window_capture_artifact_and_stdout_identity_validate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            capture = root / "captures" / "frame.png"
            capture.parent.mkdir()
            Image.new("RGBA", (4, 3), (255, 0, 0, 255)).save(capture)

            evidence = run_wsi_present.parse_wsi_stdout(
                'OK: window hwnd=0x1234 title="dxmt9 WSI test"\n'
                "OK: 180 frames presented without error\n"
            )
            artifact = run_wsi_present.image_artifact(capture, root=root, source="window_id")
            result = run_wsi_present.build_debug_result(
                command=["wine64", "wsi_present_x64.exe"],
                environment={},
                window_title="dxmt9 WSI test",
                failure_category="none",
                layer_acquisition="macdrv_functions",
                capture_source="window_id",
                reason_key="failure_reason",
                reason="live window capture artifact recorded",
                artifacts=[artifact],
                exit_code=0,
                hwnd=evidence["hwnd"],
                presented_frames=evidence["presented_frames"],
            )

        self.assertEqual(schema.validate_result(result, require={"wsi"}), [])
        self.assertEqual(result["diagnostics"]["wsi"]["hwnd"], "0x1234")
        self.assertEqual(result["artifacts"][0]["width"], 4)


if __name__ == "__main__":
    unittest.main()
