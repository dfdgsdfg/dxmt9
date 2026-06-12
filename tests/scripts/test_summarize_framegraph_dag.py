#!/usr/bin/env python3
"""Regression tests for Frame Graph DAG summary parsing."""

from __future__ import annotations

import csv
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "summarize_framegraph_dag.py"


def load_module():
    spec = importlib.util.spec_from_file_location("summarize_framegraph_dag", SCRIPT)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def render_pass(index: int, color: str, depth: str, first: int, count: int) -> dict:
    return {
        "index": index,
        "kind": "Render",
        "color": [color],
        "depth": depth,
        "draws": {"first": first, "count": count},
        "state_profile": f"0x{index:x}",
        "load_store": {
            "color": ["Clear/Store" if index == 0 else "Load/Store"],
            "depth": "Clear/Store" if index == 0 else "Load/Store",
        },
    }


def sample_dag(blocked: bool = False) -> dict:
    rt_a = "0x300000a00000006"
    depth_a = "0x300000100000001"
    rt_b = "0x30005aa00000002"
    depth_b = "0x300000100000004"
    resources = [
        {
            "handle": rt_a,
            "residency": "Persistent",
            "first_use_pass": 0,
            "last_use_pass": 2,
            "accesses": [
                {"pass": 0, "kind": "clear", "stage": "fragment"},
                {"pass": 2, "kind": "write", "stage": "fragment"},
            ],
        },
        {
            "handle": depth_a,
            "residency": "Persistent",
            "first_use_pass": 0,
            "last_use_pass": 2,
            "accesses": [
                {"pass": 0, "kind": "clear", "stage": "fragment"},
                {"pass": 2, "kind": "write", "stage": "fragment"},
            ],
        },
    ]
    edges = [
        {"src_pass": 0, "dst_pass": 2, "resource": rt_a},
        {"src_pass": 0, "dst_pass": 2, "resource": depth_a},
    ]
    if blocked:
        resources[0]["accesses"].insert(
            1, {"pass": 1, "kind": "write", "stage": "fragment"}
        )
        edges.extend(
            [
                {"src_pass": 0, "dst_pass": 1, "resource": rt_a},
                {"src_pass": 1, "dst_pass": 2, "resource": rt_a},
            ]
        )
    return {
        "frame_id": 50,
        "chunk_seq_id": 50,
        "stage": "post-opt",
        "passes": [
            render_pass(0, rt_a, depth_a, 0, 14),
            render_pass(1, rt_b, depth_b, 14, 40),
            render_pass(2, rt_a, depth_a, 54, 79),
            {
                "index": 3,
                "kind": "Present",
                "color": [],
                "depth": None,
                "draws": {"first": 133, "count": 0},
                "state_profile": "0x0",
                "load_store": {"color": [], "depth": "DontCare/DontCare"},
            },
        ],
        "resources": resources,
        "edges": edges,
    }


class SummarizeFramegraphDagTests(unittest.TestCase):
    def test_safe_same_attachment_reentry_candidate(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "dag-frame50-chunk50-post-opt.json"
            path.write_text(json.dumps(sample_dag()), encoding="utf-8")

            summary, candidates = module.summarize_dag(module.read_dag(path))

            self.assertEqual(summary["same_attachment_pairs"], 1)
            self.assertEqual(summary["safe_relocatable_pairs"], 1)
            self.assertEqual(len(candidates), 1)
            row = candidates[0]
            self.assertEqual(row["a_pass"], 0)
            self.assertEqual(row["b_pass"], 2)
            self.assertEqual(row["distance"], 1)
            self.assertEqual(
                row["direct_edge_resources"],
                "0x300000100000001;0x300000a00000006",
            )
            self.assertEqual(row["intervening_same_attachment_accesses"], "")
            self.assertEqual(row["intervening_edge_count"], 0)
            self.assertEqual(row["safe_relocatable_candidate"], 1)

    def test_intervening_same_attachment_access_blocks_candidate(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "dag-frame50-chunk50-post-opt.json"
            path.write_text(json.dumps(sample_dag(blocked=True)), encoding="utf-8")

            summary, candidates = module.summarize_dag(module.read_dag(path))

            self.assertEqual(summary["same_attachment_pairs"], 1)
            self.assertEqual(summary["safe_relocatable_pairs"], 0)
            self.assertEqual(candidates[0]["safe_no_intervening_attachment_access"], 0)
            self.assertEqual(candidates[0]["safe_relocatable_candidate"], 0)
            self.assertIn(
                "0x300000a00000006@P1",
                candidates[0]["intervening_same_attachment_accesses"],
            )

    def test_cli_writes_summary_and_candidate_csv(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as temp:
            temp_path = Path(temp)
            dag_dir = temp_path / "dag"
            dag_dir.mkdir()
            (dag_dir / "dag-frame50-chunk50-post-opt.json").write_text(
                json.dumps(sample_dag()), encoding="utf-8"
            )
            candidate_csv = temp_path / "candidates.csv"
            summary_csv = temp_path / "summary.csv"
            markdown = temp_path / "summary.md"

            rc = module.main(
                [
                    str(dag_dir),
                    "--stage",
                    "post-opt",
                    "--csv",
                    str(candidate_csv),
                    "--summary-csv",
                    str(summary_csv),
                    "--markdown",
                    str(markdown),
                ]
            )

            self.assertEqual(rc, 0)
            with candidate_csv.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["safe_relocatable_candidate"], "1")
            with summary_csv.open(newline="", encoding="utf-8") as handle:
                summary_rows = list(csv.DictReader(handle))
            self.assertEqual(summary_rows[0]["safe_relocatable_pairs"], "1")
            self.assertIn("Frame Graph DAG Summary", markdown.read_text())


if __name__ == "__main__":
    unittest.main()
