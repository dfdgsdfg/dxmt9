#include "dxmt9/dxmt9_draw_texture_dump_policy.hpp"
#include "dxmt9/dxmt9_color_attachment_dump_policy.hpp"
#include "dxmt9/dxmt9_color_attachment_capture_observer.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using dxmt9::encoders::DrawTextureDumpPassIdentity;
using dxmt9::encoders::drawTextureDumpRepeatPolicyValid;
using dxmt9::encoders::drawTextureDumpSlotValid;
using dxmt9::encoders::makeDrawTextureDumpPassIdentity;
using dxmt9::encoders::parseDrawTextureDumpU64;
using dxmt9::encoders::parseDrawTextureDumpU64List;

void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "draw_texture_dump_policy_spec: " << message << '\n';
    std::exit(1);
  }
}

}  // namespace

int main() {
  using dxmt9::encoders::colorAttachmentDepthPairPolicyValid;
  using dxmt9::encoders::colorAttachmentDepthPairHasStoredContents;
  for (bool enabled : {false, true})
    for (bool hashOnly : {false, true})
      for (bool afterDraw : {false, true})
        for (auto cap : {0u, 1u, 256u, 257u, 16384u})
          check(colorAttachmentDepthPairPolicyValid(enabled, hashOnly, afterDraw, cap) ==
                    (!enabled || (hashOnly && !afterDraw && cap >= 1u && cap <= 256u)),
                "paired depth requires bounded hash-only pass-end observation");
  for (const bool known : {false, true}) {
    for (const auto samples : {0u, 1u, 4u}) {
      for (const auto action : {WMTStoreActionDontCare, WMTStoreActionStore,
               WMTStoreActionMultisampleResolve, WMTStoreActionStoreAndMultisampleResolve,
               WMTStoreActionUnknown, WMTStoreActionCustomSampleDepthStore,
               static_cast<WMTStoreAction>(255u)}) {
        const bool expected = known && samples == 1u &&
            (action == WMTStoreActionStore || action == WMTStoreActionStoreAndMultisampleResolve);
        check(dxmt9::encoders::colorAttachmentDumpHasStoredContents(known, action, samples)
                  == expected,
              "discard/unknown/resolve-only/MSAA are not stored source-pixel evidence");
        for (bool plainDepth : {false, true})
          check(colorAttachmentDepthPairHasStoredContents(known, action, samples,
                    plainDepth, 16u, 16u, 16u, 16u) ==
                    (known && action == WMTStoreActionStore && samples == 1u && plainDepth),
                "depth pair rejects unresolved, discarded, MSAA and non-plain depth");
      }
    }
  }
  for (const auto extent : {0ull, 15ull, 17ull, 0x100000000ull, ~0ull}) {
    check(!colorAttachmentDepthPairHasStoredContents(true, WMTStoreActionStore,
              1u, true, extent, 16u, 16u, 16u) &&
          !colorAttachmentDepthPairHasStoredContents(true, WMTStoreActionStore,
              1u, true, 16u, extent, 16u, 16u),
          "depth pair requires full equal extents without narrowing or overflow");
  }
  check(!colorAttachmentDepthPairHasStoredContents(true, WMTStoreActionStore,
            1u, true, 65536u, 65536u, 65536u, 65536u),
        "even matching extents cannot overflow the bounded raw depth length");
  using namespace dxmt9;
  encoders::ColorAttachmentCaptureTable<2u> captureTable;
  core::CpuReadyPublicationTicket captureTicket{
      .id = {.index = 3u, .generation = 7u},
      .storage = {.firstPage = 5u, .pageCount = 2u, .generation = 9u},
      .rawOrdinal = 11u, .sourceOrdinal = 13u, .seqId = 17u,
      .buildGeneration = 19u};
  const core::CpuReadyTape::SourceRef captureRef{
      .id = captureTicket.id, .storage = captureTicket.storage};
  check(!captureTable.find(1u, 17u, captureRef),
        "unpublished source has no capture observation");
  captureTable.record(1u, captureTicket);
  check(captureTable.find(1u, 17u, captureRef)->ticket == captureTicket,
        "exact registered source preserves publication identity");
  for (unsigned field = 0; field != 5u; ++field) {
    auto stale = captureRef;
    switch (field) {
    case 0: ++stale.id.index; break;
    case 1: ++stale.id.generation; break;
    case 2: ++stale.storage.firstPage; break;
    case 3: ++stale.storage.pageCount; break;
    case 4: ++stale.storage.generation; break;
    }
    check(!captureTable.find(1u, 17u, stale),
          "every stale source/storage coordinate fails closed");
  }
  check(!captureTable.find(2u, 17u, captureRef) &&
        !captureTable.find(1u, 19u, captureRef),
        "owner and exact sequence are required independently of ring index");
  auto collision = captureTicket;
  collision.seqId = 19u;
  captureTable.record(1u, collision);
  check(!captureTable.find(1u, 17u, captureRef) &&
        captureTable.find(1u, 19u, captureRef).has_value(),
        "bounded overwrite is missing evidence, not stale evidence");
  collision.seqId = 20u;
  captureTable.record(2u, collision);
  captureTable.forget(1u);
  check(!captureTable.find(1u, 19u, captureRef) &&
        captureTable.find(2u, 20u, captureRef).has_value(),
        "teardown forgets only the exact pool owner");

  using dxmt9::encoders::ColorAttachmentDumpFrameCursor;
  using dxmt9::encoders::colorAttachmentDumpFramePolicyValid;
  check(colorAttachmentDumpFramePolicyValid(false, 0u, false, 0u),
        "unselected frame observer preserves existing behavior");
  check(colorAttachmentDumpFramePolicyValid(true, 201u, true, 256u),
        "frame observer requires a bounded readback count");
  check(!colorAttachmentDumpFramePolicyValid(true, 0u, true, 1u) &&
        !colorAttachmentDumpFramePolicyValid(true, 1u, false, 0u) &&
        !colorAttachmentDumpFramePolicyValid(true, 1u, true, 257u),
        "invalid frame/count selectors fail closed");
  check(colorAttachmentDumpFramePolicyValid(false, 0u, true, 16384u, true) &&
        colorAttachmentDumpFramePolicyValid(true, 1u, true, 257u, true),
        "hash-only metadata admits a larger explicit bounded count");
  check(!colorAttachmentDumpFramePolicyValid(false, 0u, false, 0u, true) &&
        !colorAttachmentDumpFramePolicyValid(false, 0u, true, 0u, true) &&
        !colorAttachmentDumpFramePolicyValid(false, 0u, true, 16385u, true) &&
        !colorAttachmentDumpFramePolicyValid(false, 0u, true, 257u),
        "hash-only never permits unbounded observation or increases the raw cap");
  ColorAttachmentDumpFrameCursor cursor;
  check(cursor.observe(1u, 10u) == 1u && cursor.observe(1u, 11u) == 1u,
        "source boundaries do not advance the encoded frame");
  cursor.present(1u, 11u);
  check(cursor.observe(1u, 11u) == 2u && cursor.observe(1u, 12u) == 2u,
        "Present advances once even for following work in the same source");
  check(cursor.observe(2u, 20u) == 1u,
        "new queue owner starts a fresh diagnostic frame");
  cursor.present(2u, 20u);
  check(cursor.observe(2u, 1u) == 1u,
        "source sequence restart discards stale diagnostic frame state");
  std::uint64_t parsed = 0u;
  check(parseDrawTextureDumpU64("0x2a", parsed) && parsed == 42u,
        "strict parser accepts hexadecimal values");
  check(parseDrawTextureDumpU64("42", parsed) && parsed == 42u,
        "strict parser accepts decimal values");
  check(!parseDrawTextureDumpU64(" -1", parsed),
        "strict parser rejects whitespace-prefixed negative values");
  check(!parseDrawTextureDumpU64("42junk", parsed),
        "strict parser rejects suffixes");
  std::array<std::uint64_t, 4u> parsedList{};
  std::size_t parsedCount = 0u;
  check(parseDrawTextureDumpU64List("0, 1;0x2", parsedList, parsedCount) &&
            parsedCount == 3u && parsedList[2] == 2u,
        "strict list parser accepts bounded separated values");
  check(!parseDrawTextureDumpU64List(",,,", parsedList, parsedCount),
        "strict list parser rejects delimiter-only lists");

  constexpr std::uint64_t kTextureStageCount = 20u;
  check(drawTextureDumpSlotValid(0u, kTextureStageCount),
        "slot zero is admitted");
  check(drawTextureDumpSlotValid(kTextureStageCount - 1u, kTextureStageCount),
        "last texture slot is admitted");
  check(!drawTextureDumpSlotValid(kTextureStageCount, kTextureStageCount),
        "slot equal to texture capacity is rejected");

  check(drawTextureDumpRepeatPolicyValid(false, false, 0u, false, 0u, false,
                                         0u),
        "legacy non-repeat mode remains valid without bounds");
  check(drawTextureDumpRepeatPolicyValid(true, true, 10u, true, 20u, true,
                                         1u),
        "bounded repeat mode is admitted");
  check(!drawTextureDumpRepeatPolicyValid(true, false, 0u, true, 20u, true,
                                          1u),
        "repeat mode requires a finite lower sequence bound");
  check(!drawTextureDumpRepeatPolicyValid(true, true, 20u, true, 10u, true,
                                          1u),
        "repeat mode rejects an inverted sequence range");
  check(!drawTextureDumpRepeatPolicyValid(true, true, 10u, true, 20u, true,
                                          0u),
        "repeat mode rejects an empty dump budget");
  check(!drawTextureDumpRepeatPolicyValid(true, true, 10u, true, 20u, true,
                                          257u),
        "repeat mode enforces the hard dump cap");

  constexpr auto legacy = makeDrawTextureDumpPassIdentity(
      0x12u, true, false, 10u, 4u, false);
  constexpr auto legacyDifferentPass = makeDrawTextureDumpPassIdentity(
      0x12u, true, false, 11u, 5u, false);
  check(legacy == legacyDifferentPass,
        "legacy mode preserves once-globally behavior");
  constexpr auto firstPass = makeDrawTextureDumpPassIdentity(
      0x12u, true, false, 10u, 4u, true);
  constexpr auto secondPass = makeDrawTextureDumpPassIdentity(
      0x12u, true, false, 11u, 5u, true);
  check(firstPass != secondPass,
        "repeat mode distinguishes exact physical pass identity");
  constexpr auto differentStage = makeDrawTextureDumpPassIdentity(
      0x12u, true, true, 10u, 4u, true);
  check(firstPass != differentStage,
        "repeat mode distinguishes vertex and fragment stages");
  constexpr DrawTextureDumpPassIdentity samePass = firstPass;
  check(firstPass == samePass, "same pass identity is stable");

  std::cout << "draw_texture_dump_policy_spec passed\n";
  return 0;
}
