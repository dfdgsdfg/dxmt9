"""Install a Sikarugir-Engines pre-built Wine into experiments/wine/.

Spec: specs/winemetal/requirements.md §10.A.
Operator workflow: see experiments/wine/README.md.

Single command per machine:
    python3 scripts/wine/install_wine.py \\
        --engine sikarugir-cx-24.0.7 \\
        --target-id sikarugir-cx-24.0.7 \\
        --register-in-manifest

Steps performed:
    1. Fetch the engine tarball from Sikarugir-App/Engines.
    2. Extract the contained wswine.bundle into experiments/wine/<id>/.
    3. Fetch the Wrapper Template tarball from Sikarugir-App/Wrapper.
    4. Extract Frameworks/*.dylib (+ symlinks) and .framework bundles
       into experiments/wine/vendor/ (a single shared dependency dir
       used by every installed wine engine).
    5. Rename bin/wine and bin/wineserver to *.real, write shim scripts
       that export DYLD_FALLBACK_LIBRARY_PATH and
       DYLD_FALLBACK_FRAMEWORK_PATH pointing at vendor/ so Wine's
       dlopen() calls resolve the co-located dylibs / frameworks.
    6. Audit winemac.so for the _macdrv_functions symbol.
    7. Optionally append a [[wine]] entry to manifest.toml.

Layout on disk after install:
    experiments/wine/
    ├── manifest.toml         (committed)
    ├── README.md             (committed)
    ├── vendor/               (gitignored — shared dylib + framework dir)
    │   ├── libfreetype.6.dylib
    │   ├── libfreetype.dylib -> libfreetype.6.dylib
    │   ├── GStreamer.framework/
    │   └── SikarugirSdk.framework/
    └── <target-id>/          (gitignored — wswine.bundle contents)
        ├── bin/wine          (shim → wineloader)
        ├── bin/wine.real     (CrossOver Perl wrapper, unused)
        ├── bin/wineserver    (shim → real wineserver Mach-O)
        ├── bin/wineserver.real
        └── lib/wine/...

This script is idempotent: re-running with the same --target-id will
overwrite the install cleanly.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WINE_DIR = REPO_ROOT / "experiments" / "wine"
VENDOR_DIR = WINE_DIR / "vendor"
MANIFEST_PATH = WINE_DIR / "manifest.toml"

# Sikarugir release URLs (v1.0 release tag, per 2026-05-11 audit).
ENGINES_RELEASE = (
    "https://github.com/Sikarugir-App/Engines/releases/download/v1.0/{asset}"
)
WRAPPER_RELEASE = (
    "https://github.com/Sikarugir-App/Wrapper/releases/download/v1.0/{asset}"
)

# Default Wrapper Template revision known to ship libinotify/libfreetype.
DEFAULT_WRAPPER_TEMPLATE = "Template-1.0.11.tar.xz"

# Known-good engines. Keys are stable dxmt9 ids; values are the asset filename
# and the source/variant taxonomy used when --register-in-manifest is set.
KNOWN_ENGINES: dict[str, dict[str, str]] = {
    "sikarugir-cx-24.0.7": {
        "asset": "WS12WineCX24.0.7_7.tar.xz",
        "version": "wine-9.0 / SikarugirCX 24.0.7 r6",
        "variant": "patched",
    },
    "sikarugir-10.0": {
        "asset": "WS12WineSikarugir10.0_6.tar.xz",
        "version": "wine-10.0 / Sikarugir 10.0 r6",
        "variant": "patched",
    },
    "sikarugir-cx-23.7.1": {
        "asset": "WS12WineCX23.7.1_4.tar.xz",
        "version": "wine-8.x / SikarugirCX 23.7.1 r4",
        "variant": "patched",
    },
}

REQUIRED_SYMBOL = "_macdrv_functions"

SHIM_BODY = """#!/bin/bash
# dxmt9 shim — written by scripts/wine/install_wine.py.
# Sikarugir wine/wineserver expects runtime dylibs (FreeType, libinotify,
# GStreamer.framework, ICU, …) which install_wine.py stages under
# experiments/wine/vendor/. Export DYLD_FALLBACK_LIBRARY_PATH so Wine's
# dlopen() calls resolve them. The "real" binary lives next to this shim
# as *.real.
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
WINE_ROOT="$(cd -- "$HERE/.." && pwd)"
VENDOR_DIR="$(cd -- "$WINE_ROOT/../vendor" && pwd)"
export DYLD_FALLBACK_LIBRARY_PATH="$VENDOR_DIR:${DYLD_FALLBACK_LIBRARY_PATH:-}"
export DYLD_FALLBACK_FRAMEWORK_PATH="$VENDOR_DIR:${DYLD_FALLBACK_FRAMEWORK_PATH:-}"
TOOL=$(basename "${BASH_SOURCE[0]}")
exec "$HERE/$TOOL.real" "$@"
"""


@dataclass
class InstallResult:
    target_dir: Path
    winemac_so: Path
    has_required_symbol: bool
    dylibs_copied: int
    dylib_symlinks_created: int


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _download(url: str, dest: Path) -> None:
    print(f"  fetch {url}")
    with urllib.request.urlopen(url) as resp, dest.open("wb") as f:  # noqa: S310
        shutil.copyfileobj(resp, f)


def _has_symbol(lib: Path, symbol: str) -> bool:
    if not lib.exists():
        return False
    out = subprocess.check_output(
        ["/usr/bin/nm", "-gU", str(lib)], text=True, stderr=subprocess.DEVNULL
    )
    return bool(re.search(rf"\b{re.escape(symbol)}\b", out))


def _extract_wine_bundle(engine_tarball: Path, target_dir: Path) -> None:
    """Extract `wswine.bundle/*` from the Sikarugir Engines tarball into
    target_dir (replacing its contents). The tarball's top-level dir is
    `wswine.bundle/`; we strip that prefix.
    """
    if target_dir.exists():
        shutil.rmtree(target_dir)
    target_dir.mkdir(parents=True)
    with tarfile.open(engine_tarball, "r:xz") as tar:
        for member in tar.getmembers():
            if not member.name.startswith("wswine.bundle/"):
                continue
            # Strip leading "wswine.bundle/" so files land under target_dir/.
            stripped = member.name[len("wswine.bundle/"):]
            if not stripped:
                continue
            member.name = stripped
            tar.extract(member, target_dir, set_attrs=True, filter="data")


def _extract_frameworks(template_tarball: Path, dylib_dest: Path) -> tuple[int, int]:
    """Copy every Template-*.app/Contents/Frameworks/*.dylib AND every
    top-level .framework bundle (e.g. GStreamer.framework, SikarugirSdk.framework)
    from the Sikarugir Wrapper Template tarball into dylib_dest.

    The Template ships both real dylibs and version-aliasing symlinks
    (e.g. `libfreetype.dylib -> libfreetype.6.dylib`); both are
    preserved. Returns (regular_files_copied, symlinks_created).
    """
    dylib_dest.mkdir(parents=True, exist_ok=True)
    files = 0
    syms = 0
    dylib_pattern = re.compile(
        r"^Template-[^/]+\.app/Contents/Frameworks/([^/]+\.dylib)$"
    )
    framework_pattern = re.compile(
        r"^Template-[^/]+\.app/Contents/Frameworks/(?P<fw>[^/]+\.framework)/(?P<rest>.*)$"
    )
    with tarfile.open(template_tarball, "r:xz") as tar:
        for member in tar.getmembers():
            # Top-level dylib (with version-alias symlinks).
            m = dylib_pattern.match(member.name)
            if m:
                target = dylib_dest / m.group(1)
                if member.issym():
                    if target.exists() or target.is_symlink():
                        target.unlink()
                    target.symlink_to(member.linkname)
                    syms += 1
                elif member.isfile():
                    with tar.extractfile(member) as src, target.open("wb") as dst:  # type: ignore[union-attr]
                        shutil.copyfileobj(src, dst)
                    target.chmod(0o755)
                    files += 1
                continue

            # Framework bundle (.framework dir tree). Preserve internal layout.
            fw = framework_pattern.match(member.name)
            if not fw:
                continue
            target = dylib_dest / fw["fw"] / fw["rest"] if fw["rest"] else dylib_dest / fw["fw"]
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.issym():
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.exists() or target.is_symlink():
                    target.unlink()
                target.symlink_to(member.linkname)
                syms += 1
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                with tar.extractfile(member) as src, target.open("wb") as dst:  # type: ignore[union-attr]
                    shutil.copyfileobj(src, dst)
                target.chmod(0o755)
                files += 1
    return files, syms


def _install_shims(bundle_dir: Path) -> None:
    """Rename bin/wine -> bin/wine.real, write the shim. Same for wineserver."""
    bin_dir = bundle_dir / "bin"
    for tool in ("wine", "wineserver"):
        real = bin_dir / f"{tool}.real"
        shim = bin_dir / tool
        if real.exists():
            shim.unlink(missing_ok=True)  # re-running; drop old shim
        elif shim.exists():
            shim.rename(real)
        shim.write_text(SHIM_BODY)
        shim.chmod(0o755)


def _append_manifest_entry(target_id: str, engine: dict[str, str]) -> bool:
    """Append a [[wine]] entry for target_id. Returns True if added,
    False if an entry with that id already exists (skipped silently).
    """
    text = MANIFEST_PATH.read_text()
    if re.search(rf'^\s*id\s*=\s*"{re.escape(target_id)}"', text, re.M):
        return False
    entry = (
        f"\n# Auto-added by scripts/wine/install_wine.py for engine {engine['asset']}.\n"
        f"[[wine]]\n"
        f'id             = "{target_id}"\n'
        f'source         = "sikarugir"\n'
        f'variant        = "{engine["variant"]}"\n'
        f'version        = "{engine["version"]}"\n'
        f'path           = "$REPO_ROOT/experiments/wine/{target_id}"\n'
        f"requires_patch = true\n"
        f'patch_status   = "applied"\n'
        f'metal_surface_protocol = "legacy-macdrv-symbols"\n'
        f'notes          = "Installed via install_wine.py from {engine["asset"]}."\n'
    )
    MANIFEST_PATH.write_text(text.rstrip() + "\n" + entry)
    return True


# ---------------------------------------------------------------------------
# Main install pipeline
# ---------------------------------------------------------------------------


def install(
    *,
    engine_id: str,
    target_id: str,
    wrapper_template: str = DEFAULT_WRAPPER_TEMPLATE,
    register_in_manifest: bool = False,
) -> InstallResult:
    if engine_id not in KNOWN_ENGINES:
        raise ValueError(
            f"unknown engine {engine_id!r}; available: {sorted(KNOWN_ENGINES)}"
        )
    engine = KNOWN_ENGINES[engine_id]
    target_dir = WINE_DIR / target_id

    with tempfile.TemporaryDirectory(prefix="dxmt9-install-wine-") as td:
        tmp = Path(td)
        engine_tarball = tmp / engine["asset"]
        template_tarball = tmp / wrapper_template

        print(f"[1/6] Downloading engine {engine_id} ({engine['asset']})…")
        _download(ENGINES_RELEASE.format(asset=engine["asset"]), engine_tarball)

        print(f"[2/6] Extracting wswine.bundle → experiments/wine/{target_id}/")
        _extract_wine_bundle(engine_tarball, target_dir)

        print(f"[3/6] Downloading Wrapper Template ({wrapper_template})…")
        _download(WRAPPER_RELEASE.format(asset=wrapper_template), template_tarball)

        print("[4/6] Extracting Frameworks dylibs + .framework bundles → experiments/wine/vendor/")
        dylib_count, dylib_syms = _extract_frameworks(template_tarball, VENDOR_DIR)

    print("[5/6] Installing wine / wineserver shims…")
    _install_shims(target_dir)

    print("[6/6] Auditing winemac.so for _macdrv_functions…")
    winemac_so = target_dir / "lib" / "wine" / "x86_64-unix" / "winemac.so"
    ok = _has_symbol(winemac_so, REQUIRED_SYMBOL)
    if not ok:
        raise RuntimeError(
            f"{winemac_so}: required symbol {REQUIRED_SYMBOL!r} not visible — "
            "the engine bundle appears stripped. Refusing to register."
        )

    if register_in_manifest:
        added = _append_manifest_entry(target_id, engine)
        if added:
            print(f"  appended [[wine]] id={target_id!r} to manifest.toml")
        else:
            print(f"  manifest already has id={target_id!r}; left as-is")

    return InstallResult(
        target_dir=target_dir,
        winemac_so=winemac_so,
        has_required_symbol=ok,
        dylibs_copied=dylib_count,
        dylib_symlinks_created=dylib_syms,
    )


def _cli() -> int:
    p = argparse.ArgumentParser(
        description="Install a Sikarugir Wine bundle into experiments/wine/.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument(
        "--engine",
        required=True,
        choices=sorted(KNOWN_ENGINES),
        help="Sikarugir engine id (resolves to the Sikarugir-App/Engines asset).",
    )
    p.add_argument(
        "--target-id",
        required=True,
        help="Subdir name under experiments/wine/ and manifest id to register.",
    )
    p.add_argument(
        "--wrapper-template",
        default=DEFAULT_WRAPPER_TEMPLATE,
        help="Sikarugir Wrapper Template tarball providing the runtime dylibs.",
    )
    p.add_argument(
        "--register-in-manifest",
        action="store_true",
        help="Append a [[wine]] entry for this install to experiments/wine/manifest.toml.",
    )
    args = p.parse_args()

    try:
        result = install(
            engine_id=args.engine,
            target_id=args.target_id,
            wrapper_template=args.wrapper_template,
            register_in_manifest=args.register_in_manifest,
        )
    except Exception as e:  # noqa: BLE001
        print(f"error: {e}", file=sys.stderr)
        return 1

    print()
    print("Install complete.")
    print(f"  wine root          : {result.target_dir}")
    print(f"  winemac.so         : {result.winemac_so} (symbol OK)")
    print(f"  vendor dylibs      : {result.dylibs_copied} files + {result.dylib_symlinks_created} version-alias symlinks → {VENDOR_DIR}/")
    print()
    print("Verify with:")
    print(f"  python3 scripts/wine/resolve.py --list")
    print(f"  WINEPREFIX=$HOME/.wine-sika-test {result.target_dir}/bin/wine --version")
    return 0


if __name__ == "__main__":
    sys.exit(_cli())
