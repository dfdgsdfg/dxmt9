"""Wine prefix bootstrap for dxmt9 experiments.

Spec: specs/experiments/runtime/{requirements,spec}.md §4 (prefix lifecycle,
R-RT-4.x).

Creates experiments/prefixs/<name>/ via `wineboot --init`, then symlinks
experiments/apps_3rd/<name>/ into dosdevices/<letter>: as a relative symlink.
Counts `try_map_free_area mmap()` errors during wineboot (a known macOS-ASLR
symptom) and returns a `degraded` flag so the harness can record it in
result.json (R-RT-4.5).
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

# Support both module and direct script invocations through the project Python
# launcher by ensuring the repo root is on sys.path before the sibling import.
_REPO_ROOT_GUESS = Path(__file__).resolve().parents[2]
if str(_REPO_ROOT_GUESS) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT_GUESS))

from scripts.wine.resolve import (  # noqa: E402
    REPO_ROOT,
    ManifestError,
    WineEntry,
    load_manifest,
    resolve_wine_id,
)

# ---------------------------------------------------------------------------
# Constants (importable by run_experiment.py)
# ---------------------------------------------------------------------------

PREFIXES_ROOT: Path = REPO_ROOT / "experiments" / "prefixs"
APPS_3RD_ROOT: Path = REPO_ROOT / "experiments" / "apps_3rd"
DEFAULT_MANIFEST: Path = REPO_ROOT / "experiments" / "wine" / "manifest.toml"

# Matches the macOS-ASLR symptom logged by Wine when mmap() fails inside
# try_map_free_area. Each hit increments mmap_errors in BootstrapResult.
MMAP_ERROR_RE: re.Pattern[str] = re.compile(
    r"err:virtual:try_map_free_area mmap\(\) error"
)


# ---------------------------------------------------------------------------
# Result dataclass
# ---------------------------------------------------------------------------


@dataclass
class BootstrapResult:
    prefix_path: Path
    wine: WineEntry
    drive_letter: str
    mmap_errors: int
    degraded: bool


# ---------------------------------------------------------------------------
# Bootstrap function
# ---------------------------------------------------------------------------


def bootstrap(
    *,
    name: str,
    wine: WineEntry,
    drive_letter: str = "d",
    rebuild: bool = False,
) -> BootstrapResult:
    """Create or re-create a Wine prefix and wire apps_3rd as a drive letter.

    Args:
        name:         Logical name for the prefix (becomes the directory name).
        wine:         Resolved WineEntry providing the wine binary.
        drive_letter: Single alpha character for the dosdevices junction.
                      Defaults to "d". Must be exactly one alpha character.
        rebuild:      If True, delete and re-create the prefix directory.

    Returns:
        BootstrapResult with paths, mmap error count, and degraded flag.

    Raises:
        ValueError:   If drive_letter is not a single alpha character.
        RuntimeError: If wineboot exits non-zero and system.reg is absent
                      (indicating a genuine wineboot failure, not the benign
                      non-zero-but-still-initialized quirk).
    """
    # Validate and normalise the drive letter.
    if len(drive_letter) != 1 or not drive_letter.isalpha():
        raise ValueError(
            f"drive_letter must be exactly one alpha character, got {drive_letter!r}"
        )
    drive_letter = drive_letter.lower()

    prefix_path = PREFIXES_ROOT / name
    apps_path = APPS_3RD_ROOT / name

    # Optionally wipe a stale prefix.
    if rebuild and prefix_path.exists():
        shutil.rmtree(prefix_path)

    # Ensure both directories exist before running wineboot.
    prefix_path.mkdir(parents=True, exist_ok=True)
    apps_path.mkdir(parents=True, exist_ok=True)

    # Run wineboot --init. Capture stdout+stderr for mmap error counting.
    # No timeout: wineboot legitimately takes 15-90 s.
    wine_bin = wine.wine_bin()
    env = os.environ.copy()
    env["WINEPREFIX"] = str(prefix_path)

    cp = subprocess.run(
        [str(wine_bin), "wineboot", "--init"],
        capture_output=True,
        text=True,
        check=False,
        env=env,
    )

    combined_output = cp.stdout + cp.stderr

    # Count macOS-ASLR mmap errors. Non-zero count sets degraded=True.
    mmap_errors = len(MMAP_ERROR_RE.findall(combined_output))
    degraded = mmap_errors > 0

    # Non-zero returncode is only a real failure if wineboot never wrote
    # system.reg (Wine sometimes exits 1 after a successful init).
    system_reg = prefix_path / "system.reg"
    if cp.returncode != 0 and not system_reg.exists():
        sys.stderr.write(combined_output)
        raise RuntimeError(
            f"wineboot --init failed (returncode={cp.returncode}); see stderr above"
        )

    # Create dosdevices symlink: <prefix>/dosdevices/<letter>: -> relative path
    # to apps_3rd/<name> so the prefix is relocatable.
    dosdev = prefix_path / "dosdevices"
    dosdev.mkdir(parents=True, exist_ok=True)

    link_path = dosdev / f"{drive_letter}:"
    rel_target = os.path.relpath(apps_path, dosdev)

    # Remove any existing symlink or file at the link location before creating.
    if link_path.exists() or link_path.is_symlink():
        link_path.unlink()

    link_path.symlink_to(rel_target)

    return BootstrapResult(
        prefix_path=prefix_path,
        wine=wine,
        drive_letter=drive_letter,
        mmap_errors=mmap_errors,
        degraded=degraded,
    )


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------


def _cli() -> int:
    import argparse

    parser = argparse.ArgumentParser(
        description="Bootstrap a Wine prefix for a dxmt9 experiment."
    )
    parser.add_argument(
        "--name",
        required=True,
        help="Logical name for the prefix (directory name under experiments/prefixs/).",
    )
    parser.add_argument(
        "--wine-id",
        required=True,
        help="Wine manifest id to use (e.g. heroic-11.7).",
    )
    parser.add_argument(
        "--drive-letter",
        default="d",
        help="Dosdevices drive letter for apps_3rd junction (default: d).",
    )
    parser.add_argument(
        "--rebuild",
        action="store_true",
        help="Delete and recreate the prefix if it already exists.",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=DEFAULT_MANIFEST,
        help="Path to manifest.toml (default: experiments/wine/manifest.toml).",
    )

    args = parser.parse_args()

    try:
        entries = load_manifest(args.manifest)
        wine, source = resolve_wine_id(
            entries=entries,
            cli_arg=args.wine_id,
            env_var=None,
            catalogue_value=None,
            app_name="<cli>",
        )
    except ManifestError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    print(f"resolved wine via {source}: id={wine.id} path={wine.path}")

    result = bootstrap(
        name=args.name,
        wine=wine,
        drive_letter=args.drive_letter,
        rebuild=args.rebuild,
    )

    degraded_note = " (DEGRADED)" if result.degraded else ""
    print(f"prefix: {result.prefix_path}")
    print(f"junction: {result.drive_letter}: -> ../../apps_3rd/{args.name}")
    print(f"mmap_errors: {result.mmap_errors}{degraded_note}")

    return 0


if __name__ == "__main__":
    sys.exit(_cli())
