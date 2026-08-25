#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_X64_PE_BUILD_DIR = REPO_ROOT / "build-win32-x64" / "src"
DEFAULT_X86_PE_BUILD_DIR = REPO_ROOT / "build-win32-x86" / "src"
DEFAULT_UNIX_BUILD_DIR = REPO_ROOT / "build-x86_64-builtin" / "src"
DEFAULT_OUTPUT_DIR = REPO_ROOT / "dist" / "dxmt9-app-local"
DEFAULT_X64_MINGW_BIN_DIR = Path.home() / "llvm-mingw" / "x86_64-w64-mingw32" / "bin"
DEFAULT_X86_MINGW_BIN_DIR = Path.home() / "llvm-mingw" / "i686-w64-mingw32" / "bin"


@dataclass(frozen=True)
class PeLane:
    name: str
    pe_arch: str
    package_subdir: str
    build_dir: Path
    mingw_bin_dir: Path


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def binary_contains(path: Path, needle: bytes) -> bool:
    tail = b""
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            data = tail + chunk
            if needle in data:
                return True
            tail = data[-(len(needle) - 1):]
    return False


def copy_required_binary(output_dir: Path, source: Path, target: Path) -> dict[str, str]:
    if not source.is_file():
        raise FileNotFoundError(f"required artifact not found: {source}")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    return artifact_record(output_dir, target)


def copy_optional_dependency(output_dir: Path, source: Path, target: Path) -> tuple[str, dict[str, str]] | None:
    if not source.is_file():
        return None
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    return target.name, artifact_record(output_dir, target)


def bridge_abi_hash(candidates: list[Path]) -> str:
    missing = [path for path in candidates if not path.is_file()]
    if missing:
        formatted = "\n".join(f"  {path}" for path in missing)
        raise FileNotFoundError(f"missing generated bridge ABI headers:\n{formatted}")

    hashes = {path: sha256_file(path) for path in candidates}
    unique = set(hashes.values())
    if len(unique) != 1:
        details = "\n".join(f"  {digest}  {path}" for path, digest in hashes.items())
        raise RuntimeError(f"bridge ABI hash mismatch across build directories:\n{details}")
    return next(iter(unique))


def artifact_record(output_dir: Path, path: Path) -> dict[str, str]:
    return {
        "path": path.relative_to(output_dir).as_posix(),
        "sha256": sha256_file(path),
    }


def package_pe_lane(output_dir: Path, lane: PeLane) -> tuple[list[dict[str, str]], list[str]]:
    pe_dir = output_dir / "pe" / lane.package_subdir
    artifacts: list[dict[str, str]] = []

    artifacts.append(copy_required_binary(
        output_dir,
        lane.build_dir / "win32" / "d3d9.dll",
        pe_dir / "d3d9.dll",
    ))
    artifacts.append(copy_required_binary(
        output_dir,
        lane.build_dir / "winemetal" / "winemetal_dxmt9.dll",
        pe_dir / "winemetal_dxmt9.dll",
    ))

    pe_dependencies: list[str] = []
    for dep in ("libc++.dll", "libunwind.dll"):
        copied = copy_optional_dependency(output_dir, lane.mingw_bin_dir / dep, pe_dir / dep)
        if copied:
            name, record = copied
            pe_dependencies.append(name)
            artifacts.append(record)

    return artifacts, pe_dependencies


def package_unix_provider(output_dir: Path, unix_build_dir: Path, unix_arch: str) -> dict[str, str]:
    return copy_required_binary(
        output_dir,
        unix_build_dir / "winemetal" / "unix" / "winemetal_dxmt9.so",
        output_dir / "unix" / unix_arch / "winemetal_dxmt9.so",
    )


def variant_name(lane: PeLane, unix_arch: str) -> str:
    if lane.name == "x86" and unix_arch.startswith("x86_64-"):
        return f"x86-wow64-on-{unix_arch}"
    return f"{lane.package_subdir}-on-{unix_arch}"


def build_manifest(
    output_dir: Path,
    lanes: list[PeLane],
    unix_build_dir: Path,
    unix_arch: str,
) -> dict[str, Any]:
    abi_hash = bridge_abi_hash([
        lane.build_dir.parent / "dxmt9_bridge_ops.generated.h" for lane in lanes
    ] + [
        unix_build_dir.parent / "dxmt9_bridge_ops.generated.h",
    ])
    unix_artifact = package_unix_provider(output_dir, unix_build_dir, unix_arch)
    unix_provider_path = output_dir / unix_artifact["path"]

    variants: list[dict[str, Any]] = []
    for lane in lanes:
        pe_artifacts, pe_dependencies = package_pe_lane(output_dir, lane)
        variants.append({
            "name": variant_name(lane, unix_arch),
            "pe_arch": lane.pe_arch,
            "unix_arch": unix_arch,
            "artifacts": pe_artifacts + [unix_artifact],
            "pe_dependencies": pe_dependencies,
            "unix_dependencies": [
                {"name": "winemac.so", "source": "wine-runtime"},
                {"name": "ntdll.so", "source": "wine-runtime"},
            ],
        })

    return {
        "schema": 1,
        "mode": "app-local",
        "version": "0.1.0",
        "bridge_abi_hash": abi_hash,
        "provider_schema": "dxmt9-winemetal-v1",
        "has_wow64_unix_call_table": binary_contains(unix_provider_path, b"__wine_unix_call_wow64_funcs"),
        "min_wine_unixlib_feature": "MemoryWineLoadUnixLibByName",
        "variants": variants,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Package dxmt9 native app-local deployment artifacts.",
    )
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--x64-pe-build-dir", type=Path, default=DEFAULT_X64_PE_BUILD_DIR)
    parser.add_argument("--x86-pe-build-dir", type=Path, default=DEFAULT_X86_PE_BUILD_DIR)
    parser.add_argument("--unix-build-dir", type=Path, default=DEFAULT_UNIX_BUILD_DIR)
    parser.add_argument("--x64-mingw-bin-dir", type=Path, default=DEFAULT_X64_MINGW_BIN_DIR)
    parser.add_argument("--x86-mingw-bin-dir", type=Path, default=DEFAULT_X86_MINGW_BIN_DIR)
    parser.add_argument("--unix-arch", default="x86_64-unix")
    parser.add_argument(
        "--arch",
        choices=("x64", "x86", "both"),
        default="both",
        help="PE architecture variants to package.",
    )
    parser.add_argument("--clean", action="store_true", help="Remove output directory first.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    output_dir = args.output_dir.resolve()
    if args.clean and output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    lanes: list[PeLane] = []
    if args.arch in ("x64", "both"):
        lanes.append(PeLane(
            name="x64",
            pe_arch="x86_64-windows",
            package_subdir="x64",
            build_dir=args.x64_pe_build_dir.resolve(),
            mingw_bin_dir=args.x64_mingw_bin_dir.resolve(),
        ))
    if args.arch in ("x86", "both"):
        lanes.append(PeLane(
            name="x86",
            pe_arch="i386-windows",
            package_subdir="x86",
            build_dir=args.x86_pe_build_dir.resolve(),
            mingw_bin_dir=args.x86_mingw_bin_dir.resolve(),
        ))

    manifest = build_manifest(
        output_dir=output_dir,
        lanes=lanes,
        unix_build_dir=args.unix_build_dir.resolve(),
        unix_arch=args.unix_arch,
    )
    manifest_path = output_dir / "dxmt9-deploy.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
