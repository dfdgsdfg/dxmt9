import csv
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "run_3dmark05_semantic_replay_gate.py"


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


class SemanticReplayGateTests(unittest.TestCase):
    def write_fixture(self, root: Path, *, changed_pixels: int, owner_pixels: int,
                      color_owner_pixels: int,
                      conflict_pixels: int = 0,
                      both_cover_pixels: int = 0) -> tuple[Path, Path]:
        manifest = root / "manifest.json"
        index_file = root / "draw.index.bin"
        index_file.write_bytes(b"\x00\x00\x01\x00\x02\x00")
        manifest.write_text(json.dumps({
            "schema": "dxmt9.3dmark05.mini_replay_manifest.v1",
            "draws": [{
                "geometry": {"index_file": str(index_file)},
                "state": {"index_count": 3},
            }],
        }), encoding="utf-8")
        out = root / "gate"
        for subdir, delta in (("original", 0), ("cache-opt-lru32", -12)):
            target = out / subdir
            target.mkdir(parents=True)
            (target / "mini-replay-summary.json").write_text(json.dumps({
                "draw_count": 1,
                "index_cache_estimate": {
                    "replay_lru32_miss_delta": delta,
                    "replay_lru32_miss_delta_pct": -25.0 if delta else 0.0,
                },
            }), encoding="utf-8")
        write_csv(out / "color-compare-summary.csv", [{
            "area": "full",
            "width": "4",
            "height": "4",
            "compared_pixels": "16",
            "changed_pixels": str(changed_pixels),
            "changed_pct": "0.0",
            "before_active_pixels": "2",
            "before_active_pct": "12.5",
            "after_active_pixels": "2",
            "after_active_pct": "12.5",
            "max_delta": "5" if changed_pixels else "0",
            "mean_abs_delta": "0.0",
            "rms_delta": "0.0",
            "ssim": "1.0",
        }])
        write_csv(out / "primitive-id-canonical-draw000-summary.csv", [{
            "index_file": str(index_file),
            "before_primitive_order": "original",
            "after_primitive_order": "cache-opt-lru32",
            "triangle_count": "1",
            "primitive_identity_changed_pixels": str(owner_pixels),
            "primitive_identity_changed_bbox": "1,1-1,1" if owner_pixels else "",
            "color_changed_pixels": str(changed_pixels),
            "color_and_primitive_changed_pixels": str(color_owner_pixels),
            "max_color_delta": "5" if changed_pixels else "0",
            "max_color_delta_l1": "8" if changed_pixels else "0",
        }])
        if conflict_pixels:
            write_csv(out / "primitive-conflicts-draw000-summary.csv", [{
                "draw_index": "0",
                "encoder_draw_index": "7",
                "draw_ordinal": "99",
                "pixels": str(conflict_pixels),
                "color_changed_pixels": str(changed_pixels),
                "max_color_delta": "5" if changed_pixels else "0",
                "avg_color_delta": "1.0" if changed_pixels else "0.0",
                "max_color_delta_l1": "8" if changed_pixels else "0",
                "avg_color_delta_l1": "2.0" if changed_pixels else "0.0",
                "both_cover_pixels": str(both_cover_pixels),
                "max_abs_depth_delta": "0.125",
                "avg_abs_depth_delta": "0.0625",
                "max_uv0_delta": "1.5",
                "avg_uv0_delta": "0.25",
                "max_projected_tex7_delta": "4.0",
                "avg_projected_tex7_delta": "1.0",
                "max_tex1_delta": "8.0",
                "avg_tex1_delta": "2.0",
                "max_tex6_delta": "16.0",
                "avg_tex6_delta": "4.0",
                "index_file": str(index_file),
            }])
        return manifest, out

    def run_gate(self, manifest: Path, out: Path) -> dict[str, object]:
        subprocess.run([
            sys.executable,
            str(SCRIPT),
            str(manifest),
            "--output-dir",
            str(out),
            "--summarize-only",
            "--primitive-draw-indices",
            "0",
        ], check=True, text=True, capture_output=True)
        return json.loads((out / "semantic-gate-summary.json").read_text(encoding="utf-8"))

    def test_summarize_only_reports_final_writer_hazard(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest, out = self.write_fixture(
                Path(temp),
                changed_pixels=2,
                owner_pixels=7,
                color_owner_pixels=2,
            )
            summary = self.run_gate(manifest, out)
            self.assertEqual(summary["verdict"], "fail-final-writer-hazard")
            self.assertEqual(summary["owner_compare"]["canonical_owner_changed_pixels"], 7)
            self.assertEqual(summary["owner_compare"]["canonical_color_and_owner_changed_pixels"], 2)

    def test_summarize_only_reports_exact_pass(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest, out = self.write_fixture(
                Path(temp),
                changed_pixels=0,
                owner_pixels=0,
                color_owner_pixels=0,
            )
            summary = self.run_gate(manifest, out)
            self.assertEqual(summary["verdict"], "pass-exact")

    def test_summarize_only_aggregates_conflict_summary(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest, out = self.write_fixture(
                Path(temp),
                changed_pixels=2,
                owner_pixels=7,
                color_owner_pixels=2,
                conflict_pixels=11,
                both_cover_pixels=3,
            )
            summary = self.run_gate(manifest, out)
            conflict = summary["conflict_compare"]
            self.assertEqual(conflict["conflict_summary_rows"], 1)
            self.assertEqual(conflict["conflict_pixels"], 11)
            self.assertEqual(conflict["conflict_color_changed_pixels"], 2)
            self.assertEqual(conflict["conflict_both_cover_pixels"], 3)
            self.assertEqual(conflict["conflict_max_abs_depth_delta"], 0.125)


if __name__ == "__main__":
    unittest.main()
