"""Tests for `run_experiment.extract_wsi_layer_acquisition`.

Covers R-WMB-15.2 / `agents/rules/test_wild.rules.md`: the wild-experiment
runner must surface which WSI layer acquisition path the presenter
selected, so triage can distinguish ExtEscape, the exact-qualified legacy
aggregate-table path, and fail-closed unavailability.

Run:
    python3 -m unittest tests.scripts.test_wsi_layer_acquisition_extract

Or via meson:
    meson test -C build dxmt9-run-experiment-wsi-acquisition
"""

import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.run_apps import run_experiment  # noqa: E402


class WsiLayerAcquisitionExtractTests(unittest.TestCase):
    def _log_with(self, body: str) -> Path:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        path = Path(tmp.name) / "dxmt9.log"
        path.write_text(body, encoding="utf-8")
        return path

    def test_missing_log_reports_unavailable(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        missing = Path(tmp.name) / "absent.log"
        self.assertFalse(missing.exists())
        self.assertEqual(
            run_experiment.extract_wsi_layer_acquisition(missing),
            "unavailable",
        )

    def test_log_without_marker_reports_unavailable(self):
        log = self._log_with(
            "[dxmt9-device] info: something else\n"
            "[dxmt9-perf] frames=120\n"
        )
        self.assertEqual(
            run_experiment.extract_wsi_layer_acquisition(log),
            "unavailable",
        )

    def test_extescape_path_is_recognised(self):
        log = self._log_with(
            "[dxmt9-device] info: dxmt9-device capabilities: ...\n"
            "[dxmt9-wsi] info: layer_acquisition=extescape-v1 hwnd=0x12345\n"
            "[dxmt9-perf] frames=120\n"
        )
        self.assertEqual(
            run_experiment.extract_wsi_layer_acquisition(log),
            "extescape-v1",
        )

    def test_legacy_macdrv_symbols_is_recognised(self):
        log = self._log_with(
            "[dxmt9-wsi] info: layer_acquisition=legacy-macdrv-symbols hwnd=0x10\n"
        )
        self.assertEqual(
            run_experiment.extract_wsi_layer_acquisition(log),
            "legacy-macdrv-symbols",
        )

    def test_first_emission_wins_when_log_repeats(self):
        # Reset/rebind can emit more than once; preserve the first successful
        # creation path so later lifecycle failures do not rewrite the result.
        log = self._log_with(
            "[dxmt9-wsi] info: layer_acquisition=extescape-v1 hwnd=0x1\n"
            "[dxmt9-wsi] error: layer_acquisition=unavailable hwnd=0x2\n"
        )
        self.assertEqual(
            run_experiment.extract_wsi_layer_acquisition(log),
            "extescape-v1",
        )

    def test_unknown_token_passes_through_for_diagnostics(self):
        # If the presenter ever grows a new path, the extractor should not
        # silently downgrade to "unavailable" — the reviewer needs to see the
        # unknown token so they can extend VALID_WSI_LAYER_ACQUISITION_PATHS.
        log = self._log_with(
            "[dxmt9-wsi] info: layer_acquisition=future_path hwnd=0x1\n"
        )
        self.assertEqual(
            run_experiment.extract_wsi_layer_acquisition(log),
            "future_path",
        )

    def test_valid_path_set_matches_presenter_branches(self):
        # Mirror of the two supported paths plus the "unavailable" sentinel.
        self.assertEqual(
            run_experiment.VALID_WSI_LAYER_ACQUISITION_PATHS,
            (
                "extescape-v1",
                "legacy-macdrv-symbols",
                "unavailable",
            ),
        )

    def test_runner_clears_inherited_process_identity(self):
        source = (
            REPO_ROOT / "scripts" / "run_apps" / "run_experiment.py"
        ).read_text(encoding="utf-8")
        run_body = source[source.index("def run_experiment(") :]
        self.assertIn(
            'env.pop("DXMT9_WINE_METAL_SURFACE_PROTOCOL", None)',
            run_body,
        )
        self.assertIn('env.pop("DXMT9_WINE_MANIFEST_ID", None)', run_body)

    def test_conformance_runner_declares_the_wsi_identity(self):
        # The PE gate gives a legacy runtime a layer only when the harness
        # declares `legacy-macdrv-symbols:<id>` AND the matching id. The D3D9
        # conformance runner used to declare neither, so every
        # CreateDevice/CreateDeviceEx fail-closed with D3DERR_NOTAVAILABLE --
        # visible as only 11 failures, because most cases skip a missing
        # device and still report PASS.
        source = (
            REPO_ROOT / "scripts" / "tools" / "run_d3d9_conformance.py"
        ).read_text(encoding="utf-8")
        env_body = source[source.index("def build_env(") : source.index(
            "def run_chunk(")]
        self.assertIn(
            'env.pop("DXMT9_WINE_METAL_SURFACE_PROTOCOL", None)', env_body)
        self.assertIn('env.pop("DXMT9_WINE_MANIFEST_ID", None)', env_body)
        self.assertIn(
            'env["DXMT9_WINE_METAL_SURFACE_PROTOCOL"]', env_body)
        self.assertIn('env["DXMT9_WINE_MANIFEST_ID"]', env_body)

    def test_conformance_runner_resolves_the_default_wine_root(self):
        import importlib.util

        path = REPO_ROOT / "scripts" / "tools" / "run_d3d9_conformance.py"
        spec = importlib.util.spec_from_file_location(
            "run_d3d9_conformance_under_test", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)

        class _Args:
            wine = module.DEFAULT_WINE
            wine_manifest = module.DEFAULT_WINE_MANIFEST
            wine_id = None

        entry = module.resolve_wine_identity(_Args())
        # The default runtime is a legacy-qualified entry; its declaration must
        # be exactly `legacy-macdrv-symbols:<id>` or the PE gate rejects it.
        self.assertEqual(
            entry.metal_surface_protocol,
            f"legacy-macdrv-symbols:{entry.id}",
        )

    def test_pe_only_hdc_capability_stays_out_of_wire(self):
        source = (
            REPO_ROOT / "src" / "d3d9" / "d3d9_pe_wsi.cpp"
        ).read_text(encoding="utf-8")
        acquire = source[
            source.index("D3D9PeWsiBinding dxmt9PeAcquireWsiBinding") :
            source.index("HRESULT dxmt9PeAdoptWsiBinding")
        ]
        retained = acquire.index("binding.releaseCapability.hdc =")
        successful_return = acquire.index("return binding;", retained)
        self.assertNotIn("ReleaseDC", acquire[retained:successful_return])

        adopt = source[
            source.index("HRESULT dxmt9PeAdoptWsiBinding") :
            source.index("HRESULT dxmt9PeAdoptDeviceWsiBinding")
        ]
        wire = adopt[
            adopt.index("const D9CWsiSurfaceBinding wire") :
            adopt.index("};", adopt.index("const D9CWsiSurfaceBinding wire"))
        ]
        self.assertNotIn("releaseCapability", wire)

    def test_candidate_order_exception_closure_and_registry_lifetime(self):
        core = (
            REPO_ROOT / "src" / "d3d9" / "core_resources.cpp"
        ).read_text(encoding="utf-8")
        install = core[
            core.index("bool SwapChain::installPresentOutput") :
            core.index("void SwapChain::restoreWindowPresenter")
        ]
        self.assertLess(
            install.index("beginWsiQuiescence()"),
            install.index("registerPresenter(candidate.get())"),
        )
        self.assertLess(
            install.index("beginWsiQuiescence()"),
            install.index("unregisterPresenter();"),
        )
        self.assertLess(
            install.index("unregisterPresenter();"),
            install.index("presentId_ = candidateId"),
        )

        unregister = core[
            core.index("void SwapChain::unregisterPresenter") :
            core.index("std::shared_ptr<dxmt9::Device> SwapChain::lockUpperDevice")
        ]
        self.assertLess(
            unregister.index("queue().unregisterPresenter"),
            unregister.index("presenter_.reset()"),
        )
        self.assertIn("upperDevice_.lock()", core)
        self.assertIn("std::lock_guard wsiLock(wsiMutex_)", install)

        snapshots = (
            REPO_ROOT / "include" / "dxmt9" / "core_snapshots.hpp"
        ).read_text(encoding="utf-8")
        self.assertNotIn("Presenter* presenter()", snapshots)
        self.assertIn("PresentId snapshotPresentId()", snapshots)
        self.assertIn("bool reservePresentMirror(", snapshots)
        self.assertIn("void cancelPresentMirror(", snapshots)

        snapshot_id = core[
            core.index("PresentId SwapChain::snapshotPresentId") :
            core.index("bool SwapChain::reservePresentMirror")
        ]
        reserve_mirror = core[
            core.index("bool SwapChain::reservePresentMirror") :
            core.index("void SwapChain::cancelPresentMirror")
        ]
        cancel_mirror = core[
            core.index("void SwapChain::cancelPresentMirror") :
            core.index("std::unique_ptr<dxmt9::Presenter>")
        ]
        self.assertIn("std::lock_guard wsiLock(wsiMutex_)", snapshot_id)
        self.assertIn("std::lock_guard wsiLock(wsiMutex_)", reserve_mirror)
        self.assertIn("std::lock_guard wsiLock(wsiMutex_)", cancel_mirror)

        legacy = (
            REPO_ROOT / "src" / "dxmt9" / "dxmt9_presenter_macdrv.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("LegacyHostViewClaimRegistry", legacy)
        self.assertIn("releaseLegacyHostViewClaim", legacy)
        self.assertLess(
            legacy.index("legacyHostViewClaims().release"),
            legacy.index("WMT::MacdrvMetalView{acquisition.metalViewHandle}.release"),
        )

        provider = (
            REPO_ROOT / "src" / "winemetal" / "unix" / "winemetal_private_api.mm"
        ).read_text(encoding="utf-8")
        create = provider[
            provider.index('extern "C" obj_handle_t CreateMetalViewFromHWND') :
            provider.index('extern "C" obj_handle_t CreateMetalViewFromCocoaView')
        ]
        self.assertLess(
            create.index("if (!win_data->client_cocoa_view)"),
            create.index("pfn_create_metal_view(win_data->client_cocoa_view"),
        )

        boundary = (
            REPO_ROOT
            / "src"
            / "d3d9"
            / "device_c_swapchain_query_stateblock.cpp"
        ).read_text(encoding="utf-8")
        adopt = boundary[
            boundary.index("dxmt9c_swapchain_adopt_wsi_surface") :
            boundary.index("dxmt9c_swapchain_teardown_wsi_surface")
        ]
        self.assertIn("catch (const std::bad_alloc&)", adopt)
        self.assertIn("catch (...)", adopt)

        queue = (
            REPO_ROOT / "src" / "dxmt9" / "dxmt9_command_queue.cpp"
        ).read_text(encoding="utf-8")
        registration = queue[
            queue.index("CommandQueue::registerPresenter") :
            queue.index("CommandQueue::unregisterPresenter")
        ]
        self.assertIn("presenterSlots_.emplace_back()", registration)
        self.assertIn("catch (...)", registration)


if __name__ == "__main__":
    unittest.main()
