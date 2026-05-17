import tempfile
import unittest
from pathlib import Path
import sys
from argparse import Namespace

import numpy as np
from PIL import Image

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.run_apps import run_experiment
from scripts.check import check_debug_result_schema as debug_schema


class CaptureMetricTests(unittest.TestCase):
    def write_image(self, pixels: np.ndarray) -> Path:
        temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(temp_dir.cleanup)
        path = Path(temp_dir.name) / "image.png"
        Image.fromarray(pixels.astype(np.uint8), "RGB").save(path)
        return path

    def test_black_frame_is_classified_as_black_screen(self):
        path = self.write_image(np.zeros((4, 4, 3), dtype=np.uint8))
        metrics = run_experiment.image_luma_metrics(path)

        self.assertEqual(metrics["mean_luma"], 0.0)
        self.assertEqual(metrics["variance"], 0.0)
        self.assertTrue(run_experiment.is_black_screen(metrics))

    def test_flat_nonblack_frame_is_not_black_screen(self):
        pixels = np.full((4, 4, 3), 64, dtype=np.uint8)
        path = self.write_image(pixels)
        metrics = run_experiment.image_luma_metrics(path)

        self.assertGreater(metrics["mean_luma"], run_experiment.BLACK_LUMA_THRESHOLD)
        self.assertFalse(run_experiment.is_black_screen(metrics))

    def test_low_mean_high_variance_frame_is_not_black_screen(self):
        pixels = np.zeros((4, 4, 3), dtype=np.uint8)
        pixels[0, 0] = [255, 255, 255]
        path = self.write_image(pixels)
        metrics = run_experiment.image_luma_metrics(path)

        self.assertLessEqual(metrics["mean_luma"], run_experiment.BLACK_LUMA_THRESHOLD * 2)
        self.assertGreater(metrics["variance"], run_experiment.BLACK_VARIANCE_THRESHOLD)
        self.assertFalse(run_experiment.is_black_screen(metrics))

    def test_capture_frame_cli_override_wins(self):
        app = Namespace(capture_frame=3)

        self.assertEqual(
            run_experiment.effective_capture_frame(app, Namespace(capture_frame=9)),
            9,
        )
        self.assertEqual(
            run_experiment.effective_capture_frame(app, Namespace(capture_frame=None)),
            3,
        )

    def test_capture_request_parses_interval_and_exports_runtime_env(self):
        app = Namespace(capture_frame=3)
        request = run_experiment.build_capture_request(
            app,
            Namespace(
                capture_frame=None,
                capture_frames=None,
                capture_range="120:130:5",
                capture_video=None,
                capture_video_acceptance="triage",
                capture_max_frames=16,
                capture_max_seconds=4.0,
                capture_max_bytes=1024 * 1024,
            ),
        )

        self.assertEqual(request.mode, "interval-range")
        self.assertEqual(request.requested_frames, [120, 125, 130])
        env = run_experiment.capture_request_environment(
            request,
            Path("/tmp/out"),
            Path("/tmp/out/actual.bmp"),
        )
        self.assertEqual(env["DXMT_CAPTURE_FRAME"], "120")
        self.assertEqual(env["DXMT_CAPTURE_FRAMES"], "120,125,130")
        self.assertEqual(env["DXMT_CAPTURE_RANGE"], "120:130:5")
        self.assertEqual(env["DXMT_EXPERIMENT_CAPTURE_DIR"], "/tmp/out/internal_frames")

    def test_capture_request_parses_video_duration_and_caps_it(self):
        app = Namespace(capture_frame=3)
        request = run_experiment.build_capture_request(
            app,
            Namespace(
                capture_frame=None,
                capture_frames="7,9",
                capture_range=None,
                capture_video="10s",
                capture_video_acceptance="human_review",
                capture_max_frames=16,
                capture_max_seconds=2.0,
                capture_max_bytes=1024 * 1024,
            ),
        )

        self.assertEqual(request.mode, "frame-list")
        self.assertEqual(request.requested_frames, [7, 9])
        self.assertEqual(request.video_duration_sec, 2.0)
        self.assertEqual(request.video_acceptance, "human_review")

    def test_capture_source_prefers_internal_dump(self):
        temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(temp_dir.cleanup)
        root = Path(temp_dir.name)
        actual = root / "actual.png"
        dump = root / "actual.bmp"
        actual.write_bytes(b"not an image")
        dump.write_bytes(b"not an image")

        source = run_experiment.classify_capture_source(
            dump,
            actual,
            {"capture_mode": "window_id"},
        )

        self.assertEqual(source, "internal_dump")

    def test_capture_source_reports_window_and_fullscreen_modes(self):
        temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(temp_dir.cleanup)
        root = Path(temp_dir.name)
        actual = root / "actual.png"
        dump = root / "actual.bmp"
        actual.write_bytes(b"not an image")

        self.assertEqual(
            run_experiment.classify_capture_source(
                dump,
                actual,
                {"capture_mode": "window_id"},
            ),
            "window_capture",
        )
        self.assertEqual(
            run_experiment.classify_capture_source(
                dump,
                actual,
                {"capture_mode": "full_screen"},
            ),
            "full_screen",
        )

    def test_capture_source_falls_back_for_existing_actual_without_metadata(self):
        temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(temp_dir.cleanup)
        root = Path(temp_dir.name)
        actual = root / "actual.png"
        dump = root / "actual.bmp"
        actual.write_bytes(b"not an image")

        self.assertEqual(
            run_experiment.classify_capture_source(dump, actual, None),
            "external_capture",
        )

    def test_provider_locator_failure_classifies_missing_unixlib_by_name(self):
        temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(temp_dir.cleanup)
        log = Path(temp_dir.name) / "dxmt9.log"
        log.write_text(
            "\n".join(
                [
                    "[winemetal-bridge] debug: builtin unixlib lookup: info=1000 status=0xc0000135 handle=0x0",
                    "00c4:fixme:virtual:NtQueryVirtualMemory (0xffffffffffffffff,0x11f330,info_class=1002,0x11f340,16,0x0) Unknown information class",
                    "[winemetal-bridge] debug: provider candidate[env]: info=1002 name=\\??\\Z:\\tmp\\winemetal.so status=0xc0000003 module=0x0 handle=0x0",
                    "[winemetal-abi] error: abi-hash unix-call failed status=0xc0000003; refusing to attach winemetal.dll",
                ]
            ),
            encoding="utf-8",
        )

        failures = run_experiment.extract_wine_provider_locator_failures(log)

        self.assertEqual(
            [failure["category"] for failure in failures],
            [
                "memory_wine_load_unixlib_by_name_unsupported",
                "builtin_unixlib_not_found",
            ],
        )

    def test_single_frame_window_capture_is_not_reported_as_dropped(self):
        temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(temp_dir.cleanup)
        root = Path(temp_dir.name)
        actual = self.write_image(np.full((4, 4, 3), 96, dtype=np.uint8))
        actual = actual.rename(root / "actual.png")
        request = run_experiment.CaptureRequest(
            mode="single-frame",
            requested_frames=[0],
            max_frames=1,
            max_duration_sec=3.0,
            max_bytes=1024 * 1024,
        )

        captured, dropped = run_experiment.collect_capture_frame_records(
            capture_request=request,
            output_dir=root,
            actual_dump_path=root / "actual.bmp",
            actual_path=actual,
            scheduled_window_frames={},
            capture_source="window_capture",
        )

        self.assertEqual(dropped, [])
        self.assertEqual(len(captured), 1)
        self.assertEqual(captured[0]["frame_id"], 0)
        self.assertEqual(captured[0]["path"], actual)
        self.assertEqual(captured[0]["source"], "window_capture")

    def test_debug_result_filters_dropped_frame_that_was_captured(self):
        temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(temp_dir.cleanup)
        root = Path(temp_dir.name)
        actual = self.write_image(np.full((4, 4, 3), 96, dtype=np.uint8))
        actual = actual.rename(root / "actual.png")
        log = root / "dxmt9.log"
        log.write_text("log\n")
        debug_result = root / "debug_result.json"
        app = Namespace(name="debug-capture", window_title="dxmt9 test")

        run_experiment.write_experiment_debug_result(
            app=app,
            output_dir=root,
            debug_result_path=debug_result,
            log_path=log,
            actual_path=actual,
            command=["bash", "launcher.sh"],
            environment={"DXMT_CAPTURE_FRAME": "0"},
            capture_frame=0,
            capture_source="window_capture",
            capture_error=None,
            window_info={
                "capture_mode": "window_id",
                "window_id": 42,
                "window_title": "dxmt9 test",
            },
            captured_frames=[
                {"frame_id": 0, "path": actual, "source": "window_capture"},
            ],
            dropped_frames=[
                {
                    "frame_id": 0,
                    "source": "none",
                    "reason": "requested frame was not emitted by internal dump or window capture",
                },
            ],
        )

        payload = debug_schema.load_json(debug_result)
        capture = payload["diagnostics"]["render_capture"]
        self.assertNotIn("dropped_frames", capture)
        self.assertEqual(payload["failure_category"], "none")

    def test_experiment_debug_result_wraps_single_frame_capture(self):
        temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(temp_dir.cleanup)
        root = Path(temp_dir.name)
        actual = self.write_image(np.full((4, 4, 3), 128, dtype=np.uint8))
        actual = actual.rename(root / "actual.png")
        log = root / "dxmt9.log"
        log.write_text("log\n")
        debug_result = root / "debug_result.json"
        app = Namespace(name="debug-capture", window_title="dxmt9 test")

        path = run_experiment.write_experiment_debug_result(
            app=app,
            output_dir=root,
            debug_result_path=debug_result,
            log_path=log,
            actual_path=actual,
            command=["bash", "launcher.sh"],
            environment={"DXMT_CAPTURE_FRAME": "7"},
            capture_frame=7,
            capture_source="window_capture",
            capture_error=None,
            window_info={
                "capture_mode": "window_id",
                "window_id": 42,
                "window_title": "dxmt9 test",
            },
        )

        self.assertEqual(path, debug_result)
        payload = debug_schema.load_json(debug_result)
        self.assertEqual(
            debug_schema.validate_result(payload, require={"wsi", "render-capture"}),
            [],
        )
        self.assertEqual(payload["diagnostics"]["render_capture"]["mode"], "single-frame")
        self.assertEqual(payload["diagnostics"]["render_capture"]["source"], "window_id")
        self.assertTrue((root / "frames" / "manifest.json").exists())

    def test_experiment_debug_result_wraps_interval_frames_drops_and_video(self):
        temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(temp_dir.cleanup)
        root = Path(temp_dir.name)
        frame120 = self.write_image(np.full((4, 4, 3), 64, dtype=np.uint8))
        frame120 = frame120.rename(root / "frame120.png")
        frame125 = self.write_image(np.full((4, 4, 3), 96, dtype=np.uint8))
        frame125 = frame125.rename(root / "frame125.png")
        video = root / "source.mov"
        video.write_bytes(b"fake-video")
        sidecar = root / "internal_frames" / "frame000130.bmp.skipped.json"
        sidecar.parent.mkdir()
        sidecar.write_text(
            """{
  "schema": "dxmt9.render_capture.skip.v1",
  "frame_id": 130,
  "present_id": 130,
  "source": "internal_dump",
  "reason": "artifact-write-failed",
  "counters": {
    "present_count": 130,
    "requested_count": 3,
    "captured_count": 2,
    "dropped_count": 1
  }
}
""",
            encoding="utf-8",
        )
        log = root / "dxmt9.log"
        log.write_text("log\n")
        debug_result = root / "debug_result.json"
        app = Namespace(name="debug-capture", window_title=None)
        request = run_experiment.CaptureRequest(
            mode="interval-range",
            requested_frames=[120, 125, 130],
            max_frames=8,
            max_duration_sec=3.0,
            max_bytes=1024 * 1024,
            start_frame=120,
            end_frame=130,
            interval=5,
            video_start_frame=120,
            video_end_frame=130,
            video_duration_sec=3.0,
            video_acceptance="extractable_frames",
        )

        path = run_experiment.write_experiment_debug_result(
            app=app,
            output_dir=root,
            debug_result_path=debug_result,
            log_path=log,
            actual_path=root / "missing.png",
            command=["bash", "launcher.sh"],
            environment={"DXMT_CAPTURE_RANGE": "120:130:5"},
            capture_frame=120,
            capture_source="internal_dump",
            capture_error=None,
            window_info=None,
            capture_request=request,
            captured_frames=[
                {"frame_id": 120, "path": frame120, "source": "internal_dump"},
                {"frame_id": 125, "path": frame125, "source": "window_capture"},
            ],
            dropped_frames=[
                {
                    "frame_id": 130,
                    "source": "internal_dump",
                    "reason": "artifact-write-failed",
                    "sidecar_path": str(sidecar),
                    "counters": {"present_count": 130, "requested_count": 3},
                }
            ],
            video_path=video,
            video_error=None,
        )

        self.assertEqual(path, debug_result)
        payload = debug_schema.load_json(debug_result)
        self.assertEqual(
            debug_schema.validate_result(payload, require={"render-capture", "video"}),
            [],
        )
        capture = payload["diagnostics"]["render_capture"]
        self.assertEqual(capture["mode"], "interval-range")
        self.assertEqual(capture["dropped_frames"][0]["frame_id"], 130)
        self.assertEqual(capture["dropped_frames"][0]["sidecar_path"], "internal_frames/frame000130.bmp.skipped.json")
        self.assertTrue(any(artifact["path"] == "internal_frames/frame000130.bmp.skipped.json" for artifact in payload["artifacts"]))
        self.assertEqual(payload["diagnostics"]["video_segments"][0]["acceptance"], "extractable_frames")
        self.assertEqual(payload["failure_category"], "render-frame-sequence")


if __name__ == "__main__":
    unittest.main()
