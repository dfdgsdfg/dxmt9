#!/usr/bin/env python3

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import tomllib


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def main() -> int:
    root = repo_root()
    fixture_root = root / "tests" / "fixtures" / "corpus_sync"
    corpus_fixture = fixture_root / "corpus"
    upstream_fixture = fixture_root / "upstream"

    sync_script = root / "scripts" / "tools" / "sync_corpus.sh"
    drift_script = root / "scripts" / "check" / "check_drift.sh"

    with tempfile.TemporaryDirectory(prefix="dxmt9-corpus-sync-") as tempdir:
        temp_root = Path(tempdir)
        corpus = temp_root / "corpus"
        upstream = temp_root / "upstream"
        shutil.copytree(corpus_fixture, corpus)
        shutil.copytree(upstream_fixture, upstream)

        target_file = corpus / "arithmetic" / "mov.shader_test"
        untouched_file = corpus / "visual_c" / "sanity.shader_test"
        manifest = corpus / "MANIFEST.toml"

        before_untouched = read_text(untouched_file)

        subprocess.run(
            [
                "bash",
                str(sync_script),
                "--root",
                str(corpus),
                "--manifest",
                str(manifest),
                "--upstream-root",
                str(upstream),
                "--upstream-commit",
                "2222222",
                "--oracle-date",
                "2026-03-29",
            ],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

        updated = read_text(target_file)
        if "upstream-commit: 2222222" not in updated:
            raise SystemExit("sync did not refresh the provenance commit")
        if "source_kind: third-party-fixture" not in updated or "license_scope: third-party-fixture" not in updated:
            raise SystemExit("sync did not preserve license provenance")
        if "rgba(0.0, 1.0, 0.0, 1.0)" not in updated:
            raise SystemExit("sync did not replace the shader body")

        if read_text(untouched_file) != before_untouched:
            raise SystemExit("sync touched a non-vkd3d corpus file")

        manifest_data = tomllib.loads(read_text(manifest))
        tests = manifest_data["test"]
        vkd3d_entry = next(entry for entry in tests if entry["file"] == "arithmetic/mov.shader_test")
        if vkd3d_entry.get("upstream-commit") != "2222222":
            raise SystemExit("manifest did not refresh upstream-commit")
        if vkd3d_entry.get("oracle-date") != "2026-03-29":
            raise SystemExit("manifest did not refresh oracle-date")
        if vkd3d_entry.get("source_kind") != "third-party-fixture":
            raise SystemExit("manifest did not preserve source_kind")
        if vkd3d_entry.get("license_scope") != "third-party-fixture":
            raise SystemExit("manifest did not preserve license_scope")
        if vkd3d_entry.get("models") != ["ps_2_0"] or vkd3d_entry.get("opcodes") != ["DEF", "MOV"]:
            raise SystemExit("manifest did not preserve model/opcode metadata")

        drift = subprocess.run(
            [
                "bash",
                str(drift_script),
                "--root",
                str(corpus),
                "--manifest",
                str(manifest),
                "--upstream-root",
                str(upstream),
                "--upstream-commit",
                "2222222",
            ],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if "all tracked vkd3d files are aligned" not in drift.stdout:
            raise SystemExit("drift report did not confirm alignment")

        gaps = subprocess.run(
            [
                "python3",
                str(root / "scripts" / "tools" / "shader_corpus_tool.py"),
                "gaps",
                "--root",
                str(corpus),
                "--manifest",
                str(manifest),
                "--fail-on-metadata-gaps",
            ],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if "metadata gaps: none" not in gaps.stdout:
            raise SystemExit("gap report did not confirm complete manifest metadata")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
