"""Wine root manifest loader and resolver.

Spec: specs/experiments/runtime/{requirements,spec}.md.

Loads experiments/wine/manifest.toml and resolves a wine_id (per CLI flag,
env var, or CATALOGUE) to a concrete wine root path. Validates that the
referenced bin/wine exists and is executable.
"""

from __future__ import annotations

import os
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


class ManifestError(RuntimeError):
    pass


@dataclass(frozen=True)
class WineEntry:
    id: str
    source: str
    variant: str
    path: Path
    metal_surface_protocol: str
    version: str | None = None
    notes: str | None = None

    def wine_bin(self) -> Path:
        for rel in ("bin/wine", "bin/wine64"):
            candidate = self.path / rel
            if candidate.exists():
                return candidate
        raise ManifestError(
            f"wine entry id={self.id}: no bin/wine or bin/wine64 under {self.path}"
        )


_REQUIRED_FIELDS = ("id", "source", "variant", "path", "metal_surface_protocol")
_METAL_SURFACE_PROTOCOLS = frozenset(
    ("extescape-v1", "unsupported", "unknown")
)
_LEGACY_METAL_SURFACE_PROTOCOL_PREFIX = "legacy-macdrv-symbols:"


def legacy_metal_surface_protocol(runtime_id: str) -> str:
    return f"{_LEGACY_METAL_SURFACE_PROTOCOL_PREFIX}{runtime_id}"


def validate_wsi_spawn(
    entry: WineEntry, *, allow_unsupported_negative_test: bool = False
) -> None:
    """Fail closed before spawning a runtime without a supported WSI path."""
    if entry.metal_surface_protocol in (
        "extescape-v1",
        legacy_metal_surface_protocol(entry.id),
    ):
        return
    if allow_unsupported_negative_test and entry.metal_surface_protocol in (
        "unsupported",
        "unknown",
    ):
        return
    raise ManifestError(
        f"wine entry id={entry.id}: metal_surface_protocol "
        f"{entry.metal_surface_protocol!r} is not spawnable for windowed WSI; "
        "use the negative-compatibility opt-in only for an intentional "
        "unsupported/unknown test"
    )


def _expand_path(raw: str) -> Path:
    expanded = raw
    if "$HOME" in expanded:
        expanded = expanded.replace("$HOME", str(Path.home()))
    if "$REPO_ROOT" in expanded:
        expanded = expanded.replace("$REPO_ROOT", str(REPO_ROOT))
    p = Path(expanded)
    if not p.is_absolute():
        raise ManifestError(f"path must be absolute (after expansion): {raw!r}")
    return p


def load_manifest(path: Path) -> list[WineEntry]:
    """Parse manifest.toml at path. Raises ManifestError on malformed input."""
    with path.open("rb") as f:
        data = tomllib.load(f)
    raw_entries = data.get("wine", [])
    seen: dict[str, int] = {}
    entries: list[WineEntry] = []
    for idx, raw in enumerate(raw_entries):
        for field in _REQUIRED_FIELDS:
            if field not in raw:
                raise ManifestError(
                    f"wine[{idx}]: required field '{field}' missing"
                )
        wid = raw["id"]
        if wid in seen:
            raise ManifestError(
                f"wine[{idx}]: duplicate id '{wid}' (also at index {seen[wid]})"
            )
        seen[wid] = idx
        protocol = raw["metal_surface_protocol"]
        if protocol not in _METAL_SURFACE_PROTOCOLS and protocol != (
            legacy_metal_surface_protocol(wid)
        ):
            raise ManifestError(
                f"wine[{idx}]: invalid metal_surface_protocol {protocol!r}; "
                f"legacy entries must use "
                f"{legacy_metal_surface_protocol(wid)!r}"
            )
        entries.append(
            WineEntry(
                id=wid,
                source=raw["source"],
                variant=raw["variant"],
                path=_expand_path(raw["path"]),
                metal_surface_protocol=protocol,
                version=raw.get("version"),
                notes=raw.get("notes"),
            )
        )
    return entries


def resolve_wine_id(
    *,
    entries: list[WineEntry],
    cli_arg: str | None,
    env_var: str | None,
    catalogue_value: str | None,
    app_name: str,
) -> tuple[WineEntry, str]:
    """Resolve a wine_id by priority CLI > env > CATALOGUE.

    Returns (WineEntry, source_label_for_diagnostics).
    Raises ManifestError if nothing resolves or the id is unknown.
    """
    by_id = {e.id: e for e in entries}
    candidates = (
        ("--wine-id", cli_arg),
        ("DXMT_EXPERIMENT_WINE_ID", env_var),
        (f"CATALOGUE [[{app_name}]].wine_id", catalogue_value),
    )
    for source, value in candidates:
        if not value:
            continue
        entry = by_id.get(value)
        if entry is None:
            raise ManifestError(
                f"{source}={value!r} not found in experiments/wine/manifest.toml"
            )
        return entry, source
    raise ManifestError(
        f"app {app_name!r}: no wine_id resolved (CLI, env, and CATALOGUE all empty)"
    )


def _cli() -> int:
    import argparse
    p = argparse.ArgumentParser(description="Inspect the Wine manifest.")
    p.add_argument(
        "--manifest",
        type=Path,
        default=REPO_ROOT / "experiments" / "wine" / "manifest.toml",
        help="manifest path (default: experiments/wine/manifest.toml)",
    )
    p.add_argument("--list", action="store_true", help="list all entries")
    args = p.parse_args()
    try:
        entries = load_manifest(args.manifest)
    except ManifestError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    if args.list:
        for e in entries:
            ok = e.path.exists() and (e.path / "bin").exists()
            mark = "OK" if ok else "MISSING"
            print(f"{mark:8} {e.id:30} {e.source:12} {e.variant:10} {e.path}")
    return 0


if __name__ == "__main__":
    sys.exit(_cli())
