#!/usr/bin/env python3
"""Bind GPU-B snapshot budget receipts to the production PE ownership path.

Native budget and owned-payload specs own arithmetic and actual lifetime
behavior. This audit pins preallocation admission, exact allocation binding,
and receipt forwarding at the PE arm/bootstrap/JIT sites; it does not claim
to bound metadata, GPU allocations, CPU-A scratch, or oracle readback.
"""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


def source(name: str) -> str:
    return (ROOT / "src" / "d3d9" / name).read_text(encoding="utf-8")


def region(text: str, start: str, end: str) -> str:
    first = text.index(start)
    return text[first:text.index(end, first + len(start))]


class SnapshotBudgetPeSourceAudit(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = source("d3d9_pe_capture_state.hpp")
        cls.tape = source("d3d9_pe_device_tape.cpp")
        cls.registry = source("d3d9_pe_device_tape_registry.cpp")
        cls.arm = region(
            cls.tape,
            "bool D3D9DeviceImpl::snapshotRenderTapeResourcesAtArmBoundary()",
            "bool D3D9DeviceImpl::armRenderTapeCaptureAtPresentBoundary()",
        )
        cls.reservation = region(
            cls.arm,
            "const auto reserveSnapshotContent =",
            "    try {",
        )
        cls.bootstrap = region(
            cls.tape,
            "bool D3D9DeviceImpl::produceRenderTapeBootstrap(",
            "bool D3D9DeviceImpl::snapshotRenderTapeResourcesAtArmBoundary()",
        )
        cls.transfer = region(
            cls.header,
            "struct RenderTapeArmContentTransferSelection {",
            "enum class RenderTapeObjectRegistration",
        )
        cls.jit = region(
            cls.registry,
            "const auto incomingBytes = effectiveContent[subresource].size();",
            "    try {\n        peCaptureState_->renderTapeAdmittedIdentities.push_back(identity);",
        )

    def test_budget_initializes_once_from_normalized_session_limits(self) -> None:
        self.assertIn(
            "renderTapeSnapshotBudget.initialize(renderTapeCapture.limits().maxBlobBytes)",
            self.header,
        )
        self.assertEqual(self.header.count("renderTapeSnapshotBudget.initialize("), 1)
        self.assertNotIn("renderTapeSnapshotBudget.initialize(", self.tape + self.registry)
        self.assertNotRegex(self.header + self.tape + self.registry,
                            r"renderTapeSnapshotBudget\s*\.\s*reset\s*\(")
        self.assertLess(
            self.header.index("RenderTapeSnapshotBudget renderTapeSnapshotBudget{}"),
            self.header.index("RenderTapeCaptureSession renderTapeCapture;"),
        )
        self.assertIn("renderTapeSnapshotBudgetInitializationStatus", self.reservation)

    def test_snapshot_bytes_die_before_receipts(self) -> None:
        snapshot = region(self.header, "struct RenderTapeArmObjectSnapshot {",
                          "enum class RenderTapeArmContentTransferDisposition")
        self.assertLess(snapshot.index("retainedLeases{}"), snapshot.index("content{}"))
        self.assertIn("snapshot.retainedLeases.resize(exceptionContentCount)", self.arm)
        self.assertIn("snapshot.content.resize(exceptionContentCount)", self.arm)
        self.assertNotRegex(self.tape + self.registry,
                            r"(?:retainedLeases|retainedLease)\s*\.\s*clear\s*\(")

    def test_snapshot_is_move_only_and_populated_assignment_retires_whole_owner(self) -> None:
        snapshot = region(self.header, "struct RenderTapeArmObjectSnapshot {",
                          "enum class RenderTapeArmContentTransferDisposition")
        self.assertIn("RenderTapeArmObjectSnapshot(const RenderTapeArmObjectSnapshot&) = delete",
                      snapshot)
        self.assertIn("operator=(const RenderTapeArmObjectSnapshot&) = delete", snapshot)
        self.assertIn("RenderTapeArmObjectSnapshot(RenderTapeArmObjectSnapshot&&) noexcept = default",
                      snapshot)
        assignment = region(snapshot,
                            "operator=(RenderTapeArmObjectSnapshot&& other) noexcept {",
                            "  void swap(")
        self.assertIn("if (this != &other)", assignment)
        self.assertLess(assignment.index("RenderTapeArmObjectSnapshot retired;"),
                        assignment.index("retired.swap(*this)"))
        self.assertLess(assignment.index("retired.swap(*this)"),
                        assignment.index("swap(other)"))
        self.assertNotIn("retainedLeases =", assignment)
        self.assertNotIn("content =", assignment)

    def test_selection_requires_exact_current_owner_and_bound_receipt(self) -> None:
        for pin in (
            "snapshot->identity.kind != identity.kind",
            "snapshot->identity.generation != identity.generation",
            "snapshot->identity.objectId != identity.objectId",
            "snapshot->armOrdinal, activeOrdinal",
            "selected.descriptor.data() != snapshot->descriptor.data()",
            "selected.content.vectorData() != snapshot->content.data()",
            "snapshot->retainedLeases.size() != snapshot->content.size()",
            "lease.kind() == dxmt9::d3d9::RenderTapeSnapshotBudgetKind::Retained",
            "lease.resourceBytes() == bytes->size()",
            "lease.chargedBytes() == bytes->size()",
            "lease.boundPayloadMatches(bytes->data(), bytes->size())",
            "!snapshot->content[subresource].empty() && !transfer.retainedLease(subresource)",
        ):
            self.assertIn(pin, self.transfer)
        self.assertLess(self.transfer.index("snapshot->identity.kind != identity.kind"),
                        self.transfer.index("const RenderTapeArmContentTransferSelection transfer"))
        self.assertIn(".retainedLeases = snapshot->retainedLeases", self.transfer)
        self.assertNotIn("sha256", self.transfer)

    def test_reservations_charge_independent_retained_and_additional_staging(self) -> None:
        self.assertRegex(
            self.reservation,
            r"renderTapeSnapshotBudgetCheckedMultiply\(\s*bytes, 2u, stagingBytes\)",
        )
        self.assertIn("renderTapeSnapshotBudget.reserveRetained(bytes)", self.reservation)
        self.assertRegex(
            self.reservation,
            r"renderTapeSnapshotBudget.reserveTransient\(\s*bytes, stagingBytes\)",
        )
        self.assertIn("if (!retained)", self.reservation)
        self.assertIn("if (!transient)", self.reservation)
        self.assertIn("budget_status=%s", self.reservation)
        self.assertIn("renderTapeSnapshotBudgetStatusName(status)", self.reservation)
        self.assertIn("snapshot.retainedLeases[subresource] = std::move(retained.lease)",
                      self.reservation)
        self.assertIn("transientLease = std::move(transient.lease)", self.reservation)
        self.assertNotIn("sha256", self.reservation)
        self.assertNotIn(".resize(", self.reservation)

    def test_every_gpu_b_allocation_is_reserved_then_bound_before_provider(self) -> None:
        paths = (
            ("d24x8", "0", "byteCount"),
            ("d24s8", "0", "layout.byteCount"),
            ("color", "subresource", "expected.bytes"),
        )
        self.assertEqual(len(re.findall(r"snapshot\.content\[[^]]+\]\.resize\(",
                                        self.arm)), len(paths))
        self.assertEqual(self.arm.count("if (!reserveSnapshotContent("), len(paths))
        for kind, subresource, byte_count in paths:
            with self.subTest(kind=kind):
                path = region(self.arm, f'exceptionPhase = "{kind}_content_reserve";',
                              f"dxmt9c_device_capture_render_tape_{kind}_snapshot(")
                reserve = path.index("if (!reserveSnapshotContent(")
                allocate = path.index(f"snapshot.content[{subresource}].resize(")
                bind = path.index(f"snapshot.retainedLeases[{subresource}].bindRetainedPayload(")
                self.assertLess(reserve, allocate)
                self.assertLess(allocate, bind)
                self.assertRegex(
                    path,
                    rf"reserveSnapshotContent\(snapshot, {re.escape(subresource)}u?, "
                    rf"{re.escape(byte_count)},\s*transientLease\)\)\s*return false;",
                )
                self.assertIn(f"snapshot.content[{subresource}].data()", path)
                self.assertIn(f"snapshot.content[{subresource}].size()", path)
                self.assertRegex(path, r"bindRetainedPayload\([^;]+\)\)\s*return false;")

    def test_bootstrap_stages_receipt_before_no_fail_payload_transition(self) -> None:
        staging = region(self.bootstrap,
                         "if (auto *ownedBytes = contentTransfer.payload(subresource))",
                         "                    } else {")
        self.assertIn("contentTransfer.retainedLease(subresource)", staging)
        self.assertIn("if (!retainedLease)", staging)
        self.assertLess(staging.index("if (!retainedLease)"),
                        staging.index("seed.blobs.emplace_back()"))
        self.assertIn(".retainedLease = retainedLease", staging)
        self.assertNotIn("std::move(*ownedBytes)", staging)
        commit = region(self.bootstrap, "for (const auto &transfer : pendingContentTransfers)",
                        "logRenderTapePayloadBoundary(*peCaptureState_, \"bootstrap_complete\"")
        self.assertLess(commit.index(".bytes.swap(*transfer.source)"),
                        commit.index("std::move(*transfer.retainedLease)"))
        self.assertIn("seed.blobs[transfer.blobIndex].retainedLease", commit)

    def test_jit_passes_receipt_and_frees_residual_capacity_before_release(self) -> None:
        guard = self.jit.index("if (ownedBytes && !retainedLease)")
        consume = self.jit.index("resourceMutationOwnedBytes(")
        free = self.jit.index("std::vector<std::byte>().swap(*ownedBytes)")
        release = self.jit.index("retainedLease->reset()")
        reject = self.jit.index("if (status !=")
        self.assertLess(guard, consume)
        self.assertLess(consume, free)
        self.assertLess(free, release)
        self.assertLess(release, reject)
        self.assertRegex(
            self.jit,
            r"std::move\(\*ownedBytes\),\s*"
            r"dxmt9::d3d9::RenderTapeBufferMutationDisposition::Plain,\s*\*retainedLease\)",
        )
        self.assertNotIn("ownedBytes->clear()", self.jit)


if __name__ == "__main__":
    unittest.main()
