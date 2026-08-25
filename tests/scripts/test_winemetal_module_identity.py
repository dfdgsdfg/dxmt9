"""Executable deployment checks for dxmt9's private Wine module namespace."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.tools import package_app_local


QUALIFIED_PE = "winemetal_dxmt9.dll"
QUALIFIED_UNIX = "winemetal_dxmt9.so"


class ModuleIdentityTests(unittest.TestCase):
    def test_generated_client_resolves_only_qualified_sibling(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            command = [
                sys.executable,
                str(REPO_ROOT / "scripts/codegen/gen_wine_bridge.py"),
                "--ops-header", str(out / "ops.h"),
                "--client-cpp", str(out / "client.cpp"),
                "--server-cpp", str(out / "server.cpp"),
                "--server-entries", str(out / "entries.inc"),
            ]
            subprocess.run(command, check=True, cwd=REPO_ROOT)
            client = (out / "client.cpp").read_text(encoding="utf-8")
            self.assertIn('L"winemetal_dxmt9.dll"', client)
            self.assertNotIn('L"winemetal.dll"', client)

    def test_build_graph_declares_qualified_module_outputs(self):
        pe_meson = (REPO_ROOT / "src/winemetal/meson.build").read_text()
        unix_meson = (REPO_ROOT / "src/winemetal/unix/meson.build").read_text()
        module_def = (REPO_ROOT / "src/winemetal/winemetal.def").read_text()
        self.assertIn("shared_library(\n    'winemetal_dxmt9'", pe_meson)
        self.assertIn("shared_library(\n  'winemetal_dxmt9'", unix_meson)
        self.assertIn("@rpath/winemetal_dxmt9.so", unix_meson)
        self.assertIn("LIBRARY winemetal_dxmt9.dll", module_def)
        self.assertNotIn("LIBRARY winemetal.dll", module_def)

    def test_app_local_package_contains_no_unqualified_modules(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            output = root / "package"
            lanes = []
            for name, pe_arch in (("x64", "x86_64-windows"),
                                  ("x86", "i386-windows")):
                build = root / name / "src"
                (build / "win32").mkdir(parents=True)
                (build / "winemetal").mkdir(parents=True)
                (build / "win32/d3d9.dll").write_bytes(b"d3d9-" + name.encode())
                (build / f"winemetal/{QUALIFIED_PE}").write_bytes(
                    b"bridge-" + name.encode())
                (build.parent / "dxmt9_bridge_ops.generated.h").write_bytes(b"same-abi")
                lanes.append(package_app_local.PeLane(
                    name=name,
                    pe_arch=pe_arch,
                    package_subdir=name,
                    build_dir=build,
                    mingw_bin_dir=root / f"missing-{name}",
                ))

            unix_build = root / "unix/src"
            (unix_build / "winemetal/unix").mkdir(parents=True)
            (unix_build / f"winemetal/unix/{QUALIFIED_UNIX}").write_bytes(
                b"provider __wine_unix_call_wow64_funcs")
            (unix_build.parent / "dxmt9_bridge_ops.generated.h").write_bytes(b"same-abi")

            manifest = package_app_local.build_manifest(
                output_dir=output,
                lanes=lanes,
                unix_build_dir=unix_build,
                unix_arch="x86_64-unix",
            )
            (output / "dxmt9-deploy.json").write_text(
                json.dumps(manifest), encoding="utf-8")

            artifact_paths = {
                entry["path"]
                for variant in manifest["variants"]
                for entry in variant["artifacts"]
            }
            self.assertIn(f"pe/x64/{QUALIFIED_PE}", artifact_paths)
            self.assertIn(f"pe/x86/{QUALIFIED_PE}", artifact_paths)
            self.assertIn(f"unix/x86_64-unix/{QUALIFIED_UNIX}", artifact_paths)
            self.assertFalse(any(
                Path(path).name in {"winemetal.dll", "winemetal.so"}
                for path in artifact_paths
            ))
            self.assertFalse((output / "pe/x64/winemetal.dll").exists())
            self.assertFalse((output / "unix/x86_64-unix/winemetal.so").exists())

    def test_active_installers_never_stage_upstream_owned_basenames(self):
        installer = (REPO_ROOT / "scripts/install/install_heroic_wine.sh").read_text()
        conformance = (REPO_ROOT / "scripts/tools/run_d3d9_conformance.py").read_text()
        experiment = (REPO_ROOT / "scripts/run_apps/run_experiment.py").read_text()
        launcher = (REPO_ROOT / "experiments/launchers/common.sh").read_text()
        app_launcher = (
            REPO_ROOT / "experiments/launchers/app-d3d9-3dmark05.sh"
        ).read_text()
        self.assertNotIn('install_file "$runtime_pe_build_dir/winemetal.dll"', installer)
        self.assertNotIn('"$unix_runtime_dir/winemetal.so"', installer)
        self.assertIn('"$windows_runtime_dir/winemetal_dxmt9.dll"', installer)
        self.assertIn('"$unix_runtime_dir/winemetal_dxmt9.so"', installer)
        self.assertIn('so_dst = unix_dir / "winemetal_dxmt9.so"', conformance)
        self.assertNotIn('so_dst = unix_dir / "winemetal.so"', conformance)
        self.assertIn('lib/wine/x86_64-windows/winemetal_dxmt9.dll', experiment)
        self.assertIn('lib/wine/x86_64-unix/winemetal_dxmt9.so', experiment)
        self.assertNotIn('"lib/wine/x86_64-windows/winemetal.dll"', experiment)
        self.assertNotIn('"lib/wine/x86_64-unix/winemetal.so"', experiment)
        self.assertIn('d3d9,winemetal_dxmt9=n,b', launcher)
        self.assertNotIn('d3d9,winemetal=n,b', launcher)
        self.assertIn('d3d9,winemetal_dxmt9=n,b', app_launcher)
        self.assertNotIn('d3d9,winemetal=n,b', app_launcher)


if __name__ == "__main__":
    unittest.main()
