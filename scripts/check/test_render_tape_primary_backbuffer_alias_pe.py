#!/usr/bin/env python3
"""Pin capture-only PE routing for authoritative primary-backbuffer aliases.

Native provider tests own the physical classification oracle. This source
audit binds that ABI to the PE arm/rewrap paths without loading a PE binary or
duplicating the provider's physical-identity decision in a test-only model.
"""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
ABI = "dxmt9c_device_classify_render_tape_primary_backbuffer"


def source(name: str) -> str:
    return (ROOT / "src" / "d3d9" / name).read_text(encoding="utf-8")


def region(text: str, start: str, end: str) -> str:
    first = text.index(start)
    return text[first:text.index(end, first + len(start))]


class PrimaryBackbufferAliasPeSourceAudit(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tape = source("d3d9_pe_device_tape.cpp")
        cls.child = source("d3d9_pe_device_tape_child.cpp")
        cls.registry = source("d3d9_pe_device_tape_registry.cpp")
        cls.arm = region(
            cls.tape,
            "bool D3D9DeviceImpl::armRenderTapeCaptureAtPresentBoundaryInterval()",
            "void D3D9DeviceImpl::finishRenderTapeCaptureAtPresentBoundary",
        )
        cls.classification = region(
            cls.arm,
            "// Recompute physical storage at every arm",
            "if (!snapshotRenderTapeResourcesAtArmBoundary())",
        )
        cls.rewrap = region(
            cls.child,
            "void D3D9DeviceImpl::NotifyRenderTapeStandaloneSurfaceForChild(",
            "bool D3D9DeviceImpl::retireRenderTapeObject(",
        )

    def test_queries_are_capture_only_and_pre_snapshot(self) -> None:
        self.assertLess(
            self.rewrap.index("if (!IsRenderTapeCaptureTrackingEnabledForChild())"),
            self.rewrap.index(ABI),
        )
        self.assertLess(
            self.rewrap.index("if (IsRenderTapeCaptureActiveForChild())"),
            self.rewrap.index(ABI),
        )
        self.assertLess(self.arm.index("renderTapeCapture.enabled()"),
                        self.arm.index(ABI))
        self.assertLess(self.arm.index("admitRenderTapePresentOutput()"),
                        self.arm.index(ABI))
        self.assertLess(self.arm.index(ABI),
                        self.arm.index("snapshotRenderTapeResourcesAtArmBoundary()"))
        self.assertLess(self.arm.index(ABI),
                        self.arm.index("produceRenderTapeBootstrap(seed)"))
        self.assertEqual((self.tape + self.child + self.registry).count(ABI), 2)

    def test_arm_uses_exact_live_identity_not_shape_as_provenance(self) -> None:
        self.assertIn("object.identity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE",
                      self.classification)
        self.assertIn("object.lifetime.textureAlias", self.classification)
        self.assertRegex(self.classification,
                         rf"{ABI}\(\s*dev_, &object.identity, &isPrimary\)")
        self.assertIn("FAILED(classificationHr) || isPrimary > 1u",
                      self.classification)
        self.assertIn("(output && isPrimary == 0u)", self.classification)
        self.assertNotRegex(self.classification,
                            r"object\.(identity|role)\s*=(?!=)")
        for heuristic in ("renderTapeOutputDesc", "memcmp", "expectedPixels",
                          "surface.surface.width ==", "surface.surface.height =="):
            self.assertNotIn(heuristic, self.classification)

    def test_rearm_recomputes_or_restores_ordinary_storage(self) -> None:
        self.assertRegex(
            self.classification,
            re.compile(r"if \(!output &&.*surface\.storage =.*isPrimary != 0u"
                       r".*SwapchainBackbuffer.*:.*Standalone", re.DOTALL),
            msg="nonprimary ordinary aliases cannot retain a stale backbuffer tag",
        )
        self.assertIn("RenderTapeInitialContentDisposition::CompleteSeed",
                      self.classification)
        self.assertIn("std::memcpy(object.descriptor.data(), &surface, sizeof(surface))",
                      self.classification)
        self.assertNotIn("renderTapeArmSnapshots", self.classification)

    def test_storage_does_not_claim_the_selected_present_role(self) -> None:
        registration = region(self.registry,
                              "case D9C_CHUNK_HANDLE_KIND_SURFACE: {",
                              "case D9C_CHUNK_HANDLE_KIND_TEXTURE: {")
        self.assertIn("const bool backbufferStorage", registration)
        self.assertIn("role != RenderTapeLiveObject::Role::PresentOutput",
                      registration)
        self.assertIn("RenderTapeInitialContentDisposition::CompleteSeed",
                      registration)
        snapshot = region(self.tape, "const bool swapchainColor =",
                          "if (standaloneD24) {")
        self.assertIn("object.role == RenderTapeLiveObject::Role::Ordinary", snapshot)
        self.assertIn("object.contentCount == 1u && object.content.size() == 1u", snapshot)
        self.assertIn("RenderTapeSurfaceStorage::SwapchainBackbuffer", snapshot)
        self.assertNotRegex(snapshot, r"object\.role\s*=(?!=)")

    def test_late_primary_alias_rejects_without_seed_or_drain(self) -> None:
        self.assertIn("renderTapeSameIdentity(existing->identity, surface.identity)",
                      self.rewrap)
        self.assertIn("existingDescriptor.surface.resourceType == descriptor.resourceType",
                      self.rewrap)
        self.assertIn("renderTapeSurfaceDescriptorsEqual", self.rewrap)
        self.assertIn("(isPrimary != 0u && !exactPrimaryRewrap)", self.rewrap)
        self.assertIn("(isPrimary == 0u && existingPrimaryStorage)", self.rewrap)
        self.assertIn('abortRenderTapeCapture("late_primary_backbuffer_alias")',
                      self.rewrap)
        self.assertRegex(self.rewrap,
                         re.compile(r"if \(exactPrimaryRewrap\).*notifyRenderTapeCreatedObject\("
                                    r".*existingDescriptor.*return;", re.DOTALL))
        for effect in ("snapshotRenderTapeResourcesAtArmBoundary",
                       "capture_render_tape_color_snapshot", "flushPendingCommandChunk",
                       "appendResourceMutation", "texture_upload", "surface_upload"):
            self.assertNotIn(effect, self.rewrap)


if __name__ == "__main__":
    unittest.main()
