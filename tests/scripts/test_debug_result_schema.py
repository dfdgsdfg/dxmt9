import unittest
from pathlib import Path
import sys

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.check import check_debug_result_schema as schema  # noqa: E402


def comprehensive_result():
    return {
        "schema": schema.SCHEMA,
        "module": "wsi",
        "boundary": "B6",
        "command": ["wine", "wsi_present_x64.exe"],
        "correlation": {
            "run_id": "2026-05-16T10-31-22Z-wsi-present",
            "frame_id": 120,
            "present_id": 120,
            "seq_id": 45,
            "chunk_id": 7,
            "record_index": 3,
        },
        "environment": {"DXMT_TRACE_QUEUE": "1"},
        "limits": {
            "max_frames": 16,
            "max_duration_sec": 5.0,
            "max_bytes": 16 * 1024 * 1024,
        },
        "artifacts": [
            {
                "role": "log",
                "path": "logs/dxmt9.log",
                "format": "text",
            },
            {
                "role": "capture",
                "path": "frames/frame000120.png",
                "format": "png",
                "source": "window_id",
                "byte_size": 4096,
                "hash": "sha256:abc",
                "width": 640,
                "height": 480,
                "pixel_format": "rgba8",
                "pitch": 2560,
            },
            {
                "role": "boundary-dump-manifest",
                "path": "boundary_dumps/manifest.json",
                "format": "json",
            },
        ],
        "diagnostics": {
            "wsi": {
                "layer_acquisition": "macdrv_functions",
                "window_title": "dxmt9 WSI test",
                "capture_source": "window_id",
            },
            "headless": {
                "active": False,
            },
            "dumps": [
                {
                    "boundary": "B6",
                    "phase": "after",
                    "schema": "dxmt9.boundary_dump.wsi_present.v1",
                    "path": "boundary_dumps/B6/frame000120_after.json",
                    "sidecars": [
                        {
                            "path": "boundary_dumps/chunk0007.bin",
                            "semantic": "D9C chunk bytes",
                            "layout_version": "d9c.v1",
                            "endianness": "little",
                            "byte_size": 128,
                            "hash": "sha256:def",
                        }
                    ],
                }
            ],
            "render_capture": {
                "mode": "interval-range",
                "start_frame": 120,
                "end_frame": 130,
                "interval": 5,
                "source": "internal_dump",
                "dropped_frames": [
                    {
                        "frame_id": 130,
                        "source": "internal_dump",
                        "reason": "runtime did not emit requested frame",
                    }
                ],
                "frames": [
                    {
                        "frame_id": 120,
                        "source": "internal_dump",
                        "path": "frames/frame000120.png",
                        "byte_size": 4096,
                        "hash": "sha256:001",
                    },
                    {
                        "frame_id": 125,
                        "source": "window_id",
                        "path": "frames/frame000125.png",
                        "byte_size": 4096,
                        "hash": "sha256:002",
                    },
                ],
            },
            "video_segments": [
                {
                    "path": "video/present_0120_0130.mp4",
                    "source": "window_id",
                    "container": "mp4",
                    "codec": "h264",
                    "timebase": "frame",
                    "nominal_fps": 60.0,
                    "width": 640,
                    "height": 480,
                    "start_frame": 120,
                    "end_frame": 130,
                    "byte_size": 8192,
                    "hash": "sha256:003",
                    "acceptance": "extractable_frames",
                }
            ],
        },
        "failure_category": "none",
    }


class DebugResultSchemaTests(unittest.TestCase):
    def assertValid(self, result, require=None):
        self.assertEqual(schema.validate_result(result, require=set(require or [])), [])

    def assertInvalidContains(self, result, needle, require=None):
        errors = schema.validate_result(result, require=set(require or []))
        self.assertTrue(errors)
        self.assertTrue(
            any(needle in error for error in errors),
            f"expected {needle!r} in {errors!r}",
        )

    def test_comprehensive_schema_covers_wsi_dumps_frames_and_video(self):
        self.assertValid(
            comprehensive_result(),
            require=["wsi", "dumps", "render-capture", "video"],
        )

    def test_full_screen_capture_is_not_layer_success_proof(self):
        result = comprehensive_result()
        result["diagnostics"]["wsi"]["capture_source"] = "full_screen"

        self.assertInvalidContains(
            result,
            "visible_output_proves_layer must be false",
            require=["wsi"],
        )

        result["diagnostics"]["wsi"]["visible_output_proves_layer"] = False
        self.assertValid(result, require=["wsi"])

    def test_wsi_only_result_does_not_require_capture_limits(self):
        result = {
            "schema": schema.SCHEMA,
            "module": "wsi",
            "command": ["wine", "wsi_present_x64.exe"],
            "environment": {},
            "artifacts": [],
            "diagnostics": {
                "wsi": {
                    "layer_acquisition": "acquisition_failure",
                    "window_title": "dxmt9 WSI test",
                    "capture_source": "none",
                    "failure_reason": "probe stopped before capture",
                },
            },
            "failure_category": "wsi-layer-acquisition",
        }

        self.assertValid(result, require=["wsi"])

    def test_headless_result_cannot_claim_wsi_or_window_capture(self):
        result = comprehensive_result()
        result["diagnostics"]["headless"] = {
            "active": True,
            "reason": "non-Darwin CI host",
        }

        self.assertInvalidContains(result, "headless result must not claim WSI")

        result["diagnostics"]["wsi"] = {
            "layer_acquisition": "unavailable",
            "unavailable_reason": "headless host",
            "capture_source": "unavailable",
        }
        result["artifacts"][1]["source"] = "internal_dump"
        self.assertValid(result, require=["headless"])

    def test_boundary_dump_requires_joinable_correlation(self):
        result = comprehensive_result()
        result["correlation"] = {"run_id": "run-without-join-key"}

        self.assertInvalidContains(
            result,
            "must include at least one join key",
            require=["dumps"],
        )

    def test_binary_sidecars_need_layout_and_hash_metadata(self):
        result = comprehensive_result()
        del result["diagnostics"]["dumps"][0]["sidecars"][0]["hash"]

        self.assertInvalidContains(result, ".hash must be a non-empty string")

    def test_boundary_dump_phase_allows_derived_payloads(self):
        result = comprehensive_result()
        result["diagnostics"]["dumps"][0]["phase"] = "derived"

        self.assertValid(result, require=["dumps"])

    def test_render_capture_requires_per_frame_source(self):
        result = comprehensive_result()
        del result["diagnostics"]["render_capture"]["frames"][0]["source"]

        self.assertInvalidContains(
            result,
            "diagnostics.render_capture.frames[0].source is required",
            require=["render-capture"],
        )

    def test_dropped_frames_require_reason(self):
        result = comprehensive_result()
        del result["diagnostics"]["render_capture"]["dropped_frames"][0]["reason"]

        self.assertInvalidContains(
            result,
            "diagnostics.render_capture.dropped_frames[0].reason must be a non-empty string",
            require=["render-capture"],
        )

    def test_artifact_paths_must_stay_under_result_directory(self):
        result = comprehensive_result()
        result["artifacts"][1]["path"] = "/tmp/frame000120.png"

        self.assertInvalidContains(result, "must be a relative artifact path")

    def test_video_segment_requires_bounded_timing_metadata(self):
        result = comprehensive_result()
        segment = result["diagnostics"]["video_segments"][0]
        del segment["start_frame"]
        del segment["end_frame"]

        self.assertInvalidContains(
            result,
            "must include start/end frame or start/end time",
            require=["video"],
        )


if __name__ == "__main__":
    unittest.main()
