import tempfile
import unittest
from pathlib import Path
import sys

import numpy as np
from PIL import Image

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.check import check_debug_result_schema as schema  # noqa: E402
from scripts.tools.debug_artifact_bundle import (  # noqa: E402
    ArtifactBundleError,
    BoundaryDumpBundle,
    RenderCaptureBundle,
    merge_debug_sections,
)


class DebugArtifactBundleTests(unittest.TestCase):
    def make_root(self) -> Path:
        temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(temp_dir.cleanup)
        return Path(temp_dir.name)

    def write_frame(self, root: Path, name: str = "source.png") -> Path:
        path = root / name
        pixels = np.zeros((4, 4, 3), dtype=np.uint8)
        pixels[:, :, 1] = 255
        Image.fromarray(pixels, "RGB").save(path)
        return path

    def base_result(self, root: Path, artifacts, diagnostics, *, require_video=False):
        result = {
            "schema": schema.SCHEMA,
            "module": "bundle-selftest",
            "boundary": "B4",
            "command": ["selftest"],
            "correlation": {
                "run_id": "bundle-run",
                "frame_id": 120,
                "seq_id": 45,
                "record_index": 3,
            },
            "environment": {},
            "limits": {
                "max_bytes": 1024 * 1024,
                "max_frames": 16,
                "max_duration_sec": 3.0,
            },
            "artifacts": artifacts,
            "diagnostics": diagnostics,
            "failure_category": "none",
        }
        required = {"dumps", "render-capture"}
        if require_video:
            required.add("video")
        errors = schema.validate_result(result, require=required)
        self.assertEqual(errors, [])
        for artifact in artifacts:
            path = root / artifact["path"]
            self.assertTrue(path.exists(), artifact)
        return result

    def test_boundary_dump_bundle_writes_manifest_and_schema_result_entries(self):
        root = self.make_root()
        bundle = BoundaryDumpBundle(root, run_id="bundle-run", max_bytes=1024 * 1024)
        sidecar = bundle.write_sidecar(
            "B4/chunk0007.bin",
            b"chunk-bytes",
            semantic="D9C chunk bytes",
            layout_version="d9c.v1",
        )
        bundle.add_dump(
            boundary="B4",
            phase="before",
            schema="dxmt9.boundary_dump.bridge_args.v1",
            correlation={"frame_id": 120, "seq_id": 45},
            payload={"record": "draw"},
            sidecars=[sidecar],
        )
        artifact = bundle.write_manifest()

        result = {
            "schema": schema.SCHEMA,
            "module": "bundle-selftest",
            "command": ["selftest"],
            "correlation": {"run_id": "bundle-run", "seq_id": 45},
            "environment": {},
            "limits": {"max_bytes": 1024 * 1024},
            "artifacts": [artifact],
            "diagnostics": bundle.debug_diagnostics(),
            "failure_category": "none",
        }

        self.assertEqual(schema.validate_result(result, require={"dumps"}), [])
        self.assertTrue((root / "boundary_dumps" / "manifest.json").exists())
        self.assertTrue((root / sidecar["path"]).exists())

    def test_boundary_dump_rejects_escaping_sidecar_path(self):
        root = self.make_root()
        bundle = BoundaryDumpBundle(root, run_id="bundle-run")

        with self.assertRaises(ArtifactBundleError):
            bundle.write_sidecar(
                "../leak.bin",
                b"bad",
                semantic="bad",
                layout_version="bad.v1",
            )

    def test_boundary_dump_budget_is_enforced_before_writing_large_sidecar(self):
        root = self.make_root()
        bundle = BoundaryDumpBundle(root, run_id="bundle-run", max_bytes=2)

        with self.assertRaises(ArtifactBundleError):
            bundle.write_sidecar(
                "oversized.bin",
                b"123",
                semantic="too large",
                layout_version="test.v1",
            )

        self.assertFalse((root / "boundary_dumps" / "oversized.bin").exists())

    def test_render_capture_bundle_writes_interval_frames_and_video_metadata(self):
        root = self.make_root()
        frame_source = self.write_frame(root)
        video_source = root / "source.mp4"
        video_source.write_bytes(b"fake-mp4")

        dumps = BoundaryDumpBundle(root, run_id="bundle-run")
        dumps.add_dump(
            boundary="B5",
            phase="after",
            schema="dxmt9.boundary_dump.draw_bindings.v1",
            correlation={"draw_index": 814},
            payload={"stream0": "bound"},
        )
        dump_artifact = dumps.write_manifest()

        capture = RenderCaptureBundle(
            root,
            run_id="bundle-run",
            mode="interval-range",
            source="internal_dump",
            start_frame=120,
            end_frame=130,
            interval=5,
        )
        capture.copy_frame(frame_source, frame_id=120)
        capture.copy_frame(frame_source, frame_id=125, capture_source="window_id")
        capture.add_video_segment(
            video_source,
            source="window_id",
            container="mp4",
            codec="h264",
            timebase="frame",
            nominal_fps=60.0,
            width=640,
            height=480,
            start_frame=120,
            end_frame=130,
            acceptance="extractable_frames",
        )
        frame_artifact = capture.write_frame_manifest()

        self.base_result(
            root,
            [dump_artifact, frame_artifact, *capture.video_artifacts()],
            merge_debug_sections(
                dumps.debug_diagnostics(),
                capture.render_capture_diagnostics(),
                capture.video_diagnostics(),
            ),
            require_video=True,
        )

    def test_render_capture_bundle_converts_bmp_sources_to_png_artifacts(self):
        root = self.make_root()
        bmp_source = self.write_frame(root, "source.bmp")
        capture = RenderCaptureBundle(
            root,
            run_id="bundle-run",
            mode="single-frame",
            source="internal_dump",
            frame_id=7,
        )

        frame = capture.copy_frame(bmp_source, frame_id=7, name="frame000007.bmp")

        self.assertEqual(frame["path"], "frames/frame000007.png")
        with Image.open(root / frame["path"]) as image:
            self.assertEqual(image.format, "PNG")


if __name__ == "__main__":
    unittest.main()
