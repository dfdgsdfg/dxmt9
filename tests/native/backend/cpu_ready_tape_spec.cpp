#include "../../../src/dxmt9/dxmt9_cpu_ready_tape.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace dxmt9::core {

struct CpuReadyTapeTestAccess {
  static void markPageAllocated(CpuReadyTape& tape, std::size_t pageIndex) {
    tape.pages_[pageIndex].allocated = true;
  }

  static void forceReadyCount(CpuReadyTape& tape, std::size_t count) {
    tape.readyCount_ = count;
  }
};

}  // namespace dxmt9::core

namespace {

using dxmt9::core::ChunkSlot;
using dxmt9::core::ChunkSlotControl;
using dxmt9::core::CpuReadyAdmissionIdentity;
using dxmt9::core::CpuReadySourceId;
using dxmt9::core::CpuReadyStorageRef;
using dxmt9::core::CpuReadyTape;

static_assert(std::is_move_constructible_v<
              CpuReadyTape::DetachedArenaOwner>);
static_assert(!std::is_move_assignable_v<
              CpuReadyTape::DetachedArenaOwner>);
using dxmt9::core::CpuReadyTapeConfig;
using dxmt9::core::CpuReadyLayoutRegionRequest;
using dxmt9::core::ArenaSourcePayloadBuilder;
using dxmt9::core::SourcePayloadCapacity;
using dxmt9::core::SourcePayloadLayout;
using dxmt9::core::makeCpuReadyPayloadLayout;
using dxmt9::core::makeSourcePayloadLayout;
using dxmt9::core::makeArenaSourcePayloadLayout;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

CpuReadyTapeConfig makeConfig(std::size_t pageCount,
                              std::size_t sourceCount,
                              std::size_t compatibilityCount,
                              std::size_t maxPagesPerSource,
                              std::size_t highSources,
                              std::size_t lowSources,
                              std::size_t highPages,
                              std::size_t lowPages) {
  const auto config = CpuReadyTapeConfig::create({
      .pageSize = 64,
      .pageCount = pageCount,
      .sourceSlotCount = sourceCount,
      .readyFifoCount = compatibilityCount,
      .compatibilityPayloadCount = compatibilityCount,
      .maxPagesPerSource = maxPagesPerSource,
      .highWaterSources = highSources,
      .lowWaterSources = lowSources,
      .highWaterPages = highPages,
      .lowWaterPages = lowPages,
      .highWaterReady = compatibilityCount,
      .lowWaterReady = std::min(lowSources, compatibilityCount - 1),
  });
  check(config.has_value(), "test configuration must validate");
  return *config;
}

CpuReadyTapeConfig makeSeparatedPayloadConfig(
    std::size_t pageCount = 8,
    std::size_t sourceCount = 8,
    std::size_t compatibilityCount = 1) {
  const auto config = CpuReadyTapeConfig::create({
      .pageSize = 4096,
      .pageCount = pageCount,
      .sourceSlotCount = sourceCount,
      .readyFifoCount = sourceCount,
      .compatibilityPayloadCount = compatibilityCount,
      .maxPagesPerSource = pageCount,
      .highWaterSources = sourceCount,
      .lowWaterSources = 0,
      .highWaterPages = pageCount,
      .lowWaterPages = 0,
      .highWaterReady = sourceCount,
      .lowWaterReady = 0,
  });
  check(config.has_value(), "separated payload configuration validates");
  return *config;
}

SourcePayloadLayout makeMinimalArenaLayout() {
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1;
  capacity.surfaceCopyRecords = 1;
  const auto layout = makeSourcePayloadLayout(capacity, 4096, 8);
  check(layout.has_value(), "minimal arena layout validates");
  return *layout;
}

void publishMinimalArena(CpuReadyTape& tape,
                         const CpuReadyTape::Reservation& reservation,
                         const SourcePayloadLayout& layout) {
  check(reservation.payloadKind == CpuReadyTape::PayloadKind::Arena &&
            reservation.payload == nullptr &&
            reservation.arenaPayload != nullptr,
        "strict reservation owns only its source-index arena payload");
  auto memory = tape.writableStorage(reservation.ticket);
  check(memory.size() >= layout.usedBytes &&
            reinterpret_cast<std::uintptr_t>(memory.data()) %
                    layout.requiredBaseAlignment ==
                0,
        "arena page run satisfies the layout's absolute base alignment");
  ArenaSourcePayloadBuilder builder(
      *reservation.arenaPayload, layout, memory.first(layout.usedBytes));
  check(builder.tryAppendSurfaceCopyCommand({}) && builder.publish(),
        "minimal arena payload binds and publishes before Tape seal");
}

void abortArena(CpuReadyTape& tape,
                const CpuReadyTape::Reservation& reservation) {
  auto owner = tape.beginArenaAbort(reservation.ticket);
  check(owner.has_value(), "arena abort detaches its placement owner");
  owner->destroy();
  check(tape.finishArenaAbort(reservation.ticket, std::move(*owner)),
        "arena abort finishes only after out-of-lock destruction");
}

bool representPublished(CpuReadyTape& tape,
                        const CpuReadyTape::Reservation& source,
                        std::uint64_t ordinal,
                        std::size_t controlIndex = 0) {
  if (!tape.sealAndPublish(source.ticket, ordinal, ordinal, controlIndex)) {
    return false;
  }
  std::array<CpuReadyTape::ReadyEntry, 1> prefix{};
  return tape.copyReadyPrefix(prefix) == 1u &&
         prefix[0].source.id == source.id &&
         prefix[0].source.storage == source.storage &&
         tape.representReadyPrefix(prefix);
}

void configValidationRejectsUnsafeBounds() {
  check(!CpuReadyTapeConfig::create({}).has_value(),
        "zero-valued configuration is rejected");
  check(!CpuReadyTapeConfig::create({
             .pageSize = 64,
             .pageCount = 4,
             .sourceSlotCount = 4,
             .readyFifoCount = 2,
             .compatibilityPayloadCount = 4,
             .maxPagesPerSource = 1,
             .highWaterSources = 4,
             .lowWaterSources = 2,
             .highWaterPages = 4,
             .lowWaterPages = 2,
             .highWaterReady = 3,
             .lowWaterReady = 1,
         }).has_value(),
        "Ready high water cannot exceed hard FIFO capacity");
  check(!CpuReadyTapeConfig::create({
             .pageSize = 64,
             .pageCount = 4,
             .sourceSlotCount = 4,
             .readyFifoCount = 3,
             .compatibilityPayloadCount = 4,
             .maxPagesPerSource = 1,
             .highWaterSources = 4,
             .lowWaterSources = 2,
             .highWaterPages = 4,
             .lowWaterPages = 2,
             .highWaterReady = 2,
             .lowWaterReady = 2,
         }).has_value(),
        "Ready low water must remain below Ready high water");
  check(!CpuReadyTapeConfig::create({
             .pageSize = 64,
             .pageCount = 4,
             .sourceSlotCount = 2,
             .readyFifoCount = 2,
             .compatibilityPayloadCount = 2,
             .maxPagesPerSource = 5,
             .highWaterSources = 2,
             .lowWaterSources = 1,
             .highWaterPages = 4,
             .lowWaterPages = 1,
             .highWaterReady = 2,
             .lowWaterReady = 1,
         }).has_value(),
        "per-source page bound cannot exceed the arena");
  check(!CpuReadyTapeConfig::create({
             .pageSize = 64,
             .pageCount = 6,
             .sourceSlotCount = 4,
             .readyFifoCount = 4,
             .compatibilityPayloadCount = 4,
             .maxPagesPerSource = 4,
             .highWaterSources = 4,
             .lowWaterSources = 0,
             .highWaterPages = 3,
             .lowWaterPages = 0,
             .highWaterReady = 4,
             .lowWaterReady = 0,
         }).has_value(),
        "per-source page bound cannot exceed the page high-water");
  check(CpuReadyTapeConfig::create({
             .pageSize = 64,
             .pageCount = 4,
             .sourceSlotCount = 2,
             .readyFifoCount = 1,
             .compatibilityPayloadCount = 1,
             .maxPagesPerSource = 2,
             .highWaterSources = 2,
             .lowWaterSources = 1,
             .highWaterPages = 4,
             .lowWaterPages = 1,
             .highWaterReady = 1,
             .lowWaterReady = 0,
         }).has_value(),
        "source capacity is independent of the compatibility payload lane");
  check(!CpuReadyTapeConfig::create({
             .pageSize = dxmt9::core::kCpuReadyPageArenaAlignment + 1,
             .pageCount = 4,
             .sourceSlotCount = 2,
             .readyFifoCount = 2,
             .compatibilityPayloadCount = 1,
             .maxPagesPerSource = 1,
             .highWaterSources = 2,
             .lowWaterSources = 1,
             .highWaterPages = 4,
             .lowWaterPages = 1,
             .highWaterReady = 2,
             .lowWaterReady = 1,
         }).has_value(),
        "page stride must preserve every source payload base alignment");

  if (std::numeric_limits<std::size_t>::max() >
      std::numeric_limits<std::uint32_t>::max()) {
    const auto beyondLocator =
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) +
        1u;
    check(!CpuReadyTapeConfig::create({
               .pageSize = 1,
               .pageCount = beyondLocator,
               .sourceSlotCount = 1,
               .readyFifoCount = 1,
               .compatibilityPayloadCount = 1,
               .maxPagesPerSource = 1,
               .highWaterSources = 1,
               .lowWaterSources = 0,
               .highWaterPages = 1,
               .lowWaterPages = 0,
               .highWaterReady = 1,
               .lowWaterReady = 0,
           }).has_value(),
          "page count must fit the storage locator index");
    check(!CpuReadyTapeConfig::create({
               .pageSize = 1,
               .pageCount = 1,
               .sourceSlotCount = beyondLocator,
               .readyFifoCount = 1,
               .compatibilityPayloadCount = 1,
               .maxPagesPerSource = 1,
               .highWaterSources = 1,
               .lowWaterSources = 0,
               .highWaterPages = 1,
               .lowWaterPages = 0,
               .highWaterReady = 1,
               .lowWaterReady = 0,
           }).has_value(),
          "source slot count must fit the source locator index");
  }
}

void productionProfilesSeparateSessionPageAndSourceHeadroom() {
  const auto compatibility = CpuReadyTapeConfig::queueCompatibility(32);
  const auto streaming = CpuReadyTapeConfig::queueSessionStreaming(32);
  const auto& legacy = compatibility.values();
  const auto& session = streaming.values();

  check(legacy.pageSize == 4096u && session.pageSize == legacy.pageSize,
        "production profiles share the fixed page size");
  check(legacy.pageCount == 64u && legacy.highWaterPages == 64u &&
            legacy.lowWaterPages == 32u,
        "compatibility profile preserves the 256 KiB page arena");
  check(session.pageCount == 512u && session.highWaterPages == 512u &&
            session.lowWaterPages == 256u &&
            session.maxPagesPerSource == 512u,
        "session profile lets one segmented source use bounded total page "
        "headroom");
  check(session.sourceSlotCount == legacy.sourceSlotCount &&
            session.readyFifoCount == legacy.readyFifoCount &&
            session.compatibilityPayloadCount ==
                legacy.compatibilityPayloadCount &&
            session.highWaterSources == legacy.highWaterSources &&
            session.lowWaterSources == legacy.lowWaterSources &&
            session.highWaterReady == legacy.highWaterReady &&
            session.lowWaterReady == legacy.lowWaterReady,
        "session streaming preserves non-page queue capacity policy");
  check(legacy.maxPagesPerSource == 64u,
        "default-off compatibility profile preserves its 64-page source "
        "bound");

  bool overflowRejected = false;
  try {
    (void)CpuReadyTapeConfig::queueSessionStreaming(
        std::numeric_limits<std::size_t>::max() / 16u + 1u);
  } catch (const std::invalid_argument&) {
    overflowRejected = true;
  }
  check(overflowRejected,
        "session page-capacity multiplication rejects overflow");
}

void readyAdmissionDistinguishesHardHighAndLowWater() {
  const auto config = CpuReadyTapeConfig::create({
      .pageSize = 64,
      .pageCount = 4,
      .sourceSlotCount = 4,
      .readyFifoCount = 3,
      .compatibilityPayloadCount = 4,
      .maxPagesPerSource = 1,
      .highWaterSources = 4,
      .lowWaterSources = 0,
      .highWaterPages = 4,
      .lowWaterPages = 0,
      .highWaterReady = 2,
      .lowWaterReady = 0,
  });
  check(config.has_value(), "Ready admission configuration validates");
  CpuReadyTape tape{*config};
  const auto first = tape.reserve();
  const auto second = tape.reserve();
  check(first && second && tape.stats().readyPublicationReservations == 2u,
        "Writing sources reserve Ready publication tickets");
  check(tape.config().values().readyFifoCount == 3u &&
            tape.stats().admissionClosed,
        "Ready high water closes before the distinct hard capacity");
  check(tape.probeReserve() == CpuReadyTape::ReserveProbe::TemporaryPressure,
        "Ready ticket pressure is reported as temporary admission pressure");
  check(tape.sealAndPublish(first->ticket, 1, 1, 0) &&
            tape.sealAndPublish(second->ticket, 2, 2, 1),
        "reserved tickets guarantee publication below hard capacity");
  std::array<CpuReadyTape::ReadyEntry, 2> ready{};
  check(tape.copyReadyPrefix(ready) == 2u,
        "published Ready occupancy is observable independently of tickets");
  check(tape.representReadyPrefix(
            std::span<const CpuReadyTape::ReadyEntry>(ready.data(), 1u)),
        "representing one source releases one Ready ticket");
  check(tape.stats().admissionClosed,
        "Ready admission stays latched above its low watermark");
  std::array<CpuReadyTape::ReadyEntry, 1> suffix{};
  check(tape.copyReadyPrefix(suffix) == 1u &&
            tape.representReadyPrefix(suffix),
        "remaining Ready suffix represents in FIFO order");
  check(!tape.stats().admissionClosed &&
            tape.stats().admissionReopens == 1u,
        "Ready admission reopens only at its configured low watermark");
}

void sealAndPublishFailureLeavesWritingAndCursorsUnchanged() {
  CpuReadyTape tape{makeConfig(4, 2, 2, 1, 2, 0, 4, 0)};
  const auto source = tape.reserve();
  check(source.has_value(), "fixture reserves a Writing publication ticket");
  const auto before = tape.stats();
  dxmt9::core::CpuReadyTapeTestAccess::forceReadyCount(
      tape, tape.config().values().readyFifoCount);
  check(!tape.sealAndPublish(source->ticket, 1, 1, 0),
        "corrupt full FIFO rejects publication");
  check(tape.state(source->id, source->storage) == CpuReadyTape::State::Writing &&
            tape.resolveForWrite(source->ticket) == source->payload &&
            tape.stats().residentSources == before.residentSources &&
            tape.stats().residentPages == before.residentPages &&
            tape.stats().readyPublicationReservations ==
                before.readyPublicationReservations,
        "failed publication leaves Writing state and every reservation cursor unchanged");
}

void representPrefixIsAtomicAndPreservesReadySuffix() {
  CpuReadyTape tape{makeConfig(4, 4, 4, 1, 4, 0, 4, 0)};
  std::array<CpuReadyTape::Reservation, 3> sources{};
  for (std::size_t i = 0; i < sources.size(); ++i) {
    const auto source = tape.reserve();
    check(source.has_value(), "fixture reserves prefix source");
    sources[i] = *source;
    check(tape.sealAndPublish(source->ticket, i + 1u, i + 1u, i),
          "fixture publishes prefix source");
  }
  std::array<CpuReadyTape::ReadyEntry, 3> ready{};
  check(tape.copyReadyPrefix(ready) == 3u,
        "complete Ready candidate prefix is copied without mutation");

  auto reordered = ready;
  std::swap(reordered[0], reordered[1]);
  check(!tape.representReadyPrefix(reordered),
        "out-of-order selected prefix is rejected atomically");
  auto stale = ready;
  ++stale[1].source.storage.generation;
  check(!tape.representReadyPrefix(stale),
        "stale FIFO locator rejects the whole selected prefix");
  for (const auto& source : sources) {
    check(tape.state(source.id, source.storage) == CpuReadyTape::State::Ready,
          "failed prefix preflight leaves every source Ready");
  }

  check(tape.representReadyPrefix(
            std::span<const CpuReadyTape::ReadyEntry>(ready.data(), 2u)),
        "valid selected prefix represents in one transaction");
  check(tape.state(sources[0].id, sources[0].storage) ==
            CpuReadyTape::State::Represented &&
            tape.state(sources[1].id, sources[1].storage) ==
                CpuReadyTape::State::Represented &&
            tape.state(sources[2].id, sources[2].storage) ==
                CpuReadyTape::State::Ready,
        "selected prefix transitions together while suffix stays Ready");
  std::array<CpuReadyTape::ReadyEntry, 2> suffix{};
  check(tape.copyReadyPrefix(suffix) == 1u &&
            suffix[0] == ready[2],
        "Ready suffix retains its original FIFO locator and order");
}

void payloadLayoutIsAlignedBoundedAndOverflowSafe() {
  const std::array requests{
      CpuReadyLayoutRegionRequest{.byteCount = 3, .alignment = 1},
      CpuReadyLayoutRegionRequest{.byteCount = 16, .alignment = 16},
      CpuReadyLayoutRegionRequest{.byteCount = 9, .alignment = 8},
  };
  const auto layout = makeCpuReadyPayloadLayout(requests, 32, 2);
  check(layout.has_value(), "bounded source layout is accepted");
  check(layout->regions[0].offset == 0u &&
            layout->regions[1].offset == 16u &&
            layout->regions[2].offset == 32u,
        "layout aligns every region without overlap");
  check(layout->usedBytes == 41u && layout->pageCount == 2u,
        "layout computes exact used bytes and page run size");

  const std::array badAlignment{
      CpuReadyLayoutRegionRequest{.byteCount = 8, .alignment = 3},
  };
  check(!makeCpuReadyPayloadLayout(badAlignment, 32, 2).has_value(),
        "non-power-of-two alignment is rejected");
  const std::array overflow{
      CpuReadyLayoutRegionRequest{
          .byteCount = std::numeric_limits<std::size_t>::max(),
          .alignment = 8,
      },
  };
  check(!makeCpuReadyPayloadLayout(overflow, 32, 2).has_value(),
        "layout arithmetic overflow is rejected");
}

void payloadIsInvisibleUntilAtomicSeal() {
  CpuReadyTape tape{makeConfig(4, 2, 2, 2, 2, 0, 4, 0)};
  const auto reservation = tape.reserve(2);
  check(reservation.has_value(), "source page run reserves atomically");
  check(reservation->storage.firstPage == 0u &&
            reservation->storage.pageCount == 2u,
        "first reservation is one non-wrapping contiguous page run");

  auto writable = tape.writableStorage(reservation->ticket);
  check(writable.size() == 128u, "writer sees the complete reserved run");
  writable[0] = std::byte{0x5a};
  reservation->payload->appendClear({});

  check(tape.resolve(reservation->id, reservation->storage,
                     CpuReadyTape::State::Writing) == nullptr,
        "scheduler cannot resolve compatibility payload while Writing");
  check(tape.resolveStorage(reservation->id, reservation->storage,
                            CpuReadyTape::State::Ready).empty(),
        "page bytes are not visible before Ready publication");
  check(tape.sealAndPublish(reservation->ticket, 1, 1, 0, 1),
        "seal publishes identities, payload, and storage together");
  check(tape.resolve(reservation->id, reservation->storage,
                     CpuReadyTape::State::Ready) == reservation->payload,
        "sealed compatibility payload resolves through both generations");
  const auto bytes = tape.resolveStorage(
      reservation->id, reservation->storage, CpuReadyTape::State::Ready);
  check(bytes.size() == 1u && bytes[0] == std::byte{0x5a},
        "sealed page extent exposes only used bytes");
}

void abortRollsBackWholeNewestReservation() {
  CpuReadyTape tape{makeConfig(6, 3, 3, 3, 3, 0, 6, 0)};
  const auto first = tape.reserve(2);
  const auto second = tape.reserve(3);
  check(first && second, "test reserves two page runs");
  const auto beforeAbort = tape.stats();
  check(tape.abort(second->ticket), "newest Writing source aborts");
  check(tape.stats().residentSources + 1 == beforeAbort.residentSources &&
            tape.stats().residentPages + 3 == beforeAbort.residentPages,
        "abort returns descriptor, pages, and compatibility payload together");
  check(tape.resolveForWrite(second->ticket) == nullptr,
        "aborted publication ticket is stale");
  const auto retried = tape.reserve(3);
  check(retried && retried->storage.firstPage == second->storage.firstPage,
        "rollback restores the exact allocator tail");
  check(retried->id.index == second->id.index &&
            retried->id.generation != second->id.generation,
        "retry advances descriptor generation");
}

void strictAdmissionIdentitySurvivesAbort() {
  CpuReadyTape tape{makeSeparatedPayloadConfig()};
  const auto layout = makeMinimalArenaLayout();
  const CpuReadyAdmissionIdentity firstIdentity{
      .rawOrdinal = 10,
      .sourceOrdinal = 20,
      .seqId = 30,
      .buildGeneration = 40,
  };
  check(!tape.reserve(layout.pageCount + 1, layout.usedBytes,
                      firstIdentity).has_value() &&
            tape.residentCount() == 0,
        "strict sizing must name the exact reserved page count");

  const auto first =
      tape.reserve(layout.pageCount, layout.usedBytes, firstIdentity);
  check(first.has_value() && first->ticket.rawOrdinal == 10 &&
            first->ticket.sourceOrdinal == 20 &&
            first->ticket.seqId == 30 &&
            first->ticket.buildGeneration == 40,
        "strict reserve fixes all admission identities in the ticket");
  check(!tape.reserve().has_value() &&
            !tape.reserve(layout.pageCount, layout.usedBytes,
                          CpuReadyAdmissionIdentity{
                              .rawOrdinal = 11,
                              .sourceOrdinal = 21,
                              .seqId = 31,
                              .buildGeneration = 41,
                          }).has_value(),
        "an open strict writer excludes every later reservation");
  check(!tape.sealAndPublish(first->ticket, 0) &&
            !tape.sealAndPublish(first->ticket, 0,
                                 layout.usedBytes - 1) &&
            !tape.sealAndPublish(first->ticket, 0,
                                 layout.usedBytes) &&
            tape.state(first->id, first->storage) ==
                CpuReadyTape::State::Writing,
        "strict seal requires the complete planned layout extent");
  auto tampered = first->ticket;
  ++tampered.seqId;
  check(tape.writableStorage(tampered).empty() &&
            !tape.sealAndPublish(tampered, 0, layout.usedBytes) &&
            !tape.beginArenaAbort(tampered).has_value(),
        "strict write, seal, and abort validate the complete identity");
  auto detached = tape.beginArenaAbort(first->ticket);
  check(detached.has_value(), "strict abort detaches the arena owner");
  detached->destroy();
  const auto detachedGeneration =
      tape.sourceGenerationAt(first->id.index);
  check(!tape.finishArenaReclaim(first->id, first->storage,
                                 std::move(*detached)) &&
            tape.residentCount() == 1 &&
            tape.sourceGenerationAt(first->id.index) ==
                detachedGeneration &&
            tape.state(first->id, first->storage) ==
                CpuReadyTape::State::Reclaiming,
        "abort capability cannot finish through the reclaim transaction");
  check(!tape.reserve().has_value() &&
            !tape.reserve(layout.pageCount, layout.usedBytes,
                          CpuReadyAdmissionIdentity{
                              .rawOrdinal = 11,
                              .sourceOrdinal = 21,
                              .seqId = 31,
                              .buildGeneration = 41,
                          }).has_value(),
        "detached abort retains the strict guard until generation advance");
  check(tape.finishArenaAbort(first->ticket, std::move(*detached)),
        "destroyed raw owner address validates without laundering dead storage");
  check(!tape.reserve(layout.pageCount, layout.usedBytes,
                      firstIdentity).has_value(),
        "strict abort must not make consumed identities reusable");
  check(!tape.reserve(layout.pageCount, layout.usedBytes,
                      CpuReadyAdmissionIdentity{
                          .rawOrdinal = 11,
                          .sourceOrdinal = 21,
                          .seqId = 30,
                          .buildGeneration = 41,
                      }).has_value(),
        "raw/source/seq high-water must advance after abort");

  const auto later = tape.reserve(
      layout.pageCount, layout.usedBytes, CpuReadyAdmissionIdentity{
          .rawOrdinal = 11,
          .sourceOrdinal = 21,
          .seqId = 31,
          .buildGeneration = 40,
      });
  check(later.has_value(), "later strict identity reserves arena storage");
  publishMinimalArena(tape, *later, layout);
  check(tape.sealAndPublish(later->ticket, 0, layout.usedBytes),
        "strict construction advances identities while buildGeneration remains an exact stamp");
}

void strictSealRequiresExactTapeOwnedBinding() {
  CpuReadyTape tape{makeSeparatedPayloadConfig()};
  const auto layout = makeMinimalArenaLayout();
  const auto reservation = tape.reserve(
      layout.pageCount, layout.usedBytes, CpuReadyAdmissionIdentity{
          .rawOrdinal = 1,
          .sourceOrdinal = 1,
          .seqId = 1,
          .buildGeneration = 1,
      });
  check(reservation.has_value(), "external-binding fixture reserves");

  std::vector<std::max_align_t> external(
      (layout.usedBytes + sizeof(std::max_align_t) - 1) /
      sizeof(std::max_align_t));
  auto externalBytes = std::span<std::byte>(
      reinterpret_cast<std::byte*>(external.data()), layout.usedBytes);
  ArenaSourcePayloadBuilder builder(
      *reservation->arenaPayload, layout, externalBytes);
  check(builder.tryAppendSurfaceCopyCommand({}) && builder.publish(),
        "arena block can independently publish against aligned external memory");
  check(!tape.sealAndPublish(reservation->ticket, 0, layout.usedBytes) &&
            tape.state(reservation->id, reservation->storage) ==
                CpuReadyTape::State::Writing,
        "strict seal rejects a block not bound to its ticket page run");
  abortArena(tape, *reservation);
}

void strictArenaDoesNotConsumeCompatibilityCapacity() {
  CpuReadyTape tape{makeSeparatedPayloadConfig(6, 6, 1)};
  const auto layout = makeMinimalArenaLayout();
  const auto legacy = tape.reserve();
  check(legacy.has_value() &&
            tape.sealAndPublish(legacy->ticket, 10, 20, 0) &&
            tape.stats().compatibilityPayloads == 1,
        "fixture fills the single compatibility payload slot");

  const auto arena = tape.reserve(
      layout.pageCount, layout.usedBytes, CpuReadyAdmissionIdentity{
          .rawOrdinal = 1,
          .sourceOrdinal = 11,
          .seqId = 21,
          .buildGeneration = 1,
      });
  check(arena.has_value() && tape.stats().compatibilityPayloads == 1,
        "strict arena admission ignores compatibility-only pressure");
  publishMinimalArena(tape, *arena, layout);
  check(tape.sealAndPublish(arena->ticket, 1, layout.usedBytes) &&
            tape.stats().compatibilityPayloads == 1,
        "arena publication never increments compatibility residency");
  check(!tape.reserve().has_value(),
        "the filled compatibility lane remains independently bounded");
}

void segmentedArenaPublishesAndReclaimsAsOneSource() {
  CpuReadyTape tape{makeSeparatedPayloadConfig()};
  const auto segment = makeMinimalArenaLayout();
  const std::array segmentLayouts{segment, segment};
  const auto layout = makeArenaSourcePayloadLayout(segmentLayouts, 4096, 8);
  check(layout.has_value(), "segmented arena layout validates");
  auto overlapping = *layout;
  overlapping.segments[1].byteOffset = 0;
  check(!tape.reserve(overlapping, CpuReadyAdmissionIdentity{
                                       .rawOrdinal = 1,
                                       .sourceOrdinal = 1,
                                       .seqId = 1,
                                       .buildGeneration = 1,
                                   }),
        "overlapping segment extents reject without consuming identity");
  const auto reservation = tape.reserve(
      *layout, CpuReadyAdmissionIdentity{
                   .rawOrdinal = 1,
                   .sourceOrdinal = 1,
                   .seqId = 1,
                   .buildGeneration = 1,
               });
  check(reservation.has_value() && reservation->arenaPayloadCount == 2 &&
            reservation->arenaPayloads[0] &&
            reservation->arenaPayloads[1],
        "one strict ticket constructs all packed segment owners");
  for (std::size_t i = 0; i < reservation->arenaPayloadCount; ++i) {
    auto memory = tape.writableArenaSegment(reservation->ticket, i);
    ArenaSourcePayloadBuilder builder(
        *reservation->arenaPayloads[i], layout->segments[i].layout, memory);
    dxmt9::core::SurfaceCopyDesc copy{};
    copy.source = dxmt9::core::Handle{i + 1};
    check(builder.tryAppendSurfaceCopyCommand(copy) && builder.publish(),
          "each owner publishes only against its packed extent");
  }
  check(tape.sealAndPublish(reservation->ticket, 0,
                            layout->usedBytes),
        "all packed owners become Ready atomically");
  const auto payload = tape.resolveSourcePayload(
      reservation->id, reservation->storage, CpuReadyTape::State::Ready);
  check(payload.valid() && payload.arenaSegmentCount() == 2 &&
            payload.commandCount() == 2 &&
            payload.commandAt(0).command.surfaceCopy->source ==
                dxmt9::core::Handle{1} &&
            payload.commandAt(1).command.surfaceCopy->source ==
                dxmt9::core::Handle{2},
        "one Ready source exposes a logical command space across segments");

  std::array<CpuReadyTape::ReadyEntry, 1> ready{};
  check(tape.copyReadyPrefix(ready) == 1 &&
            tape.representReadyPrefix(ready) &&
            tape.transition(reservation->id, reservation->storage,
                            CpuReadyTape::State::Represented,
                            CpuReadyTape::State::Submitted) &&
            tape.complete(reservation->id, reservation->storage) &&
            tape.beginReclaim(reservation->id, reservation->storage),
        "segmented source retains one lifecycle identity");
  auto owner = tape.detachReclaimingArenaOwner(
      reservation->id, reservation->storage);
  check(owner.has_value(), "reclaim detaches the complete owner chain");
  owner->destroy();
  check(tape.finishArenaReclaim(reservation->id, reservation->storage,
                                std::move(*owner)) &&
            tape.residentCount() == 0,
        "all owners destroy before one atomic page-run reclaim");
}

void arenaOwnerReclaimIsTwoPhaseAndGenerationScoped() {
  CpuReadyTape tape{makeSeparatedPayloadConfig(2, 1, 1)};
  const auto layout = makeMinimalArenaLayout();
  const auto first = tape.reserve(
      layout.pageCount, layout.usedBytes, CpuReadyAdmissionIdentity{
          .rawOrdinal = 1,
          .sourceOrdinal = 1,
          .seqId = 1,
          .buildGeneration = 7,
      });
  check(first.has_value(), "first arena generation reserves");
  publishMinimalArena(tape, *first, layout);
  check(tape.sealAndPublish(first->ticket, 0, layout.usedBytes),
        "first arena generation seals");
  std::array<CpuReadyTape::ReadyEntry, 1> ready{};
  check(tape.copyReadyPrefix(ready) == 1 &&
            tape.representReadyPrefix(ready) &&
            tape.transition(first->id, first->storage,
                            CpuReadyTape::State::Represented,
                            CpuReadyTape::State::Submitted) &&
            tape.complete(first->id, first->storage) &&
            tape.beginReclaim(first->id, first->storage),
        "published arena reaches the reclaim transaction");
  auto owner = tape.detachReclaimingArenaOwner(first->id, first->storage);
  check(owner.has_value() &&
            tape.resolveArena(first->id, first->storage,
                              CpuReadyTape::State::Ready) == nullptr,
        "detached arena owner is no longer resolvable");
  owner->destroy();
  const auto reclaimGeneration =
      tape.sourceGenerationAt(first->id.index);
  check(!tape.finishArenaAbort(first->ticket, std::move(*owner)) &&
            tape.residentCount() == 1 &&
            tape.sourceGenerationAt(first->id.index) ==
                reclaimGeneration &&
            tape.state(first->id, first->storage) ==
                CpuReadyTape::State::Reclaiming,
        "reclaim capability cannot finish through the abort transaction");
  check(tape.finishArenaReclaim(first->id, first->storage,
                                std::move(*owner)),
        "generation advances only after detached destruction finishes");

  const auto second = tape.reserve(
      layout.pageCount, layout.usedBytes, CpuReadyAdmissionIdentity{
          .rawOrdinal = 2,
          .sourceOrdinal = 2,
          .seqId = 2,
          .buildGeneration = 7,
      });
  check(second.has_value() && second->id.index == first->id.index &&
            second->id.generation != first->id.generation &&
            second->arenaPayload == first->arenaPayload,
        "source slot reconstructs its arena owner at a new generation");
  abortArena(tape, *second);
}

void rawLegacyAndStrictAdmissionShareRawHighWater() {
  CpuReadyTape tape{makeSeparatedPayloadConfig(6, 6, 2)};
  const auto layout = makeMinimalArenaLayout();
  const auto legacy = tape.reserve();
  check(legacy.has_value() &&
            tape.sealAndPublish(legacy->ticket, 10, 20, 0, 0, 50),
        "raw-backed Legacy publication consumes its replay ordinal");
  const auto strict = [&](std::uint64_t rawOrdinal) {
    return tape.reserve(
        layout.pageCount, layout.usedBytes, CpuReadyAdmissionIdentity{
            .rawOrdinal = rawOrdinal,
            .sourceOrdinal = 11,
            .seqId = 21,
            .buildGeneration = 1,
        });
  };
  check(!strict(49).has_value() && !strict(50).has_value(),
        "strict admission rejects raw ordinals at or below Legacy high-water");
  const auto next = strict(51);
  check(next.has_value(),
        "strict admission accepts the next mixed-FIFO raw ordinal");
  abortArena(tape, *next);
}

void compatibilityAndStrictIdentityShareHighWater() {
  CpuReadyTape tape{makeSeparatedPayloadConfig(8, 8, 4)};
  const auto layout = makeMinimalArenaLayout();
  const auto compatibility = tape.reserve();
  check(compatibility.has_value() &&
            !compatibility->ticket.hasAdmissionIdentity() &&
            tape.sealAndPublish(compatibility->ticket, 50, 60, 0),
        "the compatibility writer retains seal-assigned identity behavior");

  check(!tape.reserve(layout.pageCount, layout.usedBytes,
                      CpuReadyAdmissionIdentity{
            .rawOrdinal = 1,
            .sourceOrdinal = 50,
            .seqId = 61,
            .buildGeneration = 1,
          }).has_value() &&
            !tape.reserve(layout.pageCount, layout.usedBytes,
                          CpuReadyAdmissionIdentity{
              .rawOrdinal = 1,
              .sourceOrdinal = 51,
              .seqId = 60,
              .buildGeneration = 1,
            }).has_value(),
        "strict admission cannot collide with compatibility high-water");

  const auto strict = tape.reserve(
      layout.pageCount, layout.usedBytes, CpuReadyAdmissionIdentity{
          .rawOrdinal = 1,
          .sourceOrdinal = 51,
          .seqId = 61,
          .buildGeneration = 1,
  });
  check(strict.has_value(), "strict source reserves after compatibility seal");
  abortArena(tape, *strict);

  const auto collidingCompatibility = tape.reserve();
  check(collidingCompatibility.has_value() &&
            !tape.sealAndPublish(
                collidingCompatibility->ticket, 51, 61, 1) &&
            tape.abort(collidingCompatibility->ticket),
        "compatibility seal cannot reuse a strict admission identity");
  const auto laterCompatibility = tape.reserve();
  check(laterCompatibility.has_value() &&
            tape.sealAndPublish(
                laterCompatibility->ticket, 52, 62, 1),
        "compatibility seal advances beyond the shared global high-water");
}

void reservationRejectsCorruptTargetPageWithoutMutation() {
  CpuReadyTape tape{makeConfig(4, 2, 2, 2, 2, 0, 4, 0)};
  dxmt9::core::CpuReadyTapeTestAccess::markPageAllocated(tape, 1);
  const auto before = tape.stats();
  check(!tape.canReserve(2),
        "allocator rejects a run containing an unexpectedly occupied page");
  check(tape.probeReserve(2) == CpuReadyTape::ReserveProbe::Corrupt,
        "admission probe distinguishes allocator corruption from pressure");
  check(!tape.reserve(2).has_value(),
        "corrupt allocator state remains fail-closed");
  check(tape.stats().residentSources == before.residentSources &&
            tape.stats().residentPages == before.residentPages &&
            tape.stats().compatibilityPayloads ==
                before.compatibilityPayloads,
        "collision rejection leaves every allocation cursor and count intact");
  check(tape.stats().admissionClosed,
        "allocator corruption stops admission instead of pressure waiting");
}

void wrapPaddingAndOrderedReclaimAreDeterministic() {
  CpuReadyTape tape{makeConfig(6, 4, 4, 3, 4, 0, 6, 0)};
  const auto first = tape.reserve(3);
  const auto second = tape.reserve(2);
  check(first && second, "test fills arena tail");
  check(representPublished(tape, *first, 1) &&
            tape.transition(first->id, first->storage,
                            CpuReadyTape::State::Represented,
                            CpuReadyTape::State::Submitted),
        "oldest source reaches submitted state");
  check(representPublished(tape, *second, 2, 1) &&
            tape.transition(second->id, second->storage,
                            CpuReadyTape::State::Represented,
                            CpuReadyTape::State::Submitted),
        "second source reaches submitted state");
  check(!tape.beginReclaim(first->id, first->storage),
        "submitted work cannot reclaim before explicit completion");
  check(tape.complete(first->id, first->storage) &&
            tape.complete(second->id, second->storage),
        "Metal completion makes both submitted sources reclaimable");
  check(!tape.reclaim(second->id, second->storage),
        "younger completed source cannot reclaim ahead of FIFO head");
  check(tape.reclaim(first->id, first->storage),
        "oldest completed source reclaims");

  const auto wrapped = tape.reserve(2);
  check(wrapped && wrapped->storage.firstPage == 0u,
        "allocator inserts padding and wraps to page zero");
  check(tape.stats().wrapPaddingPages == 1u,
        "wrap padding is explicit bounded residency");
  check(wrapped->storage.firstPage + wrapped->storage.pageCount <=
            tape.config().values().pageCount,
        "wrapped source run never crosses the backing end");
  check(representPublished(tape, *wrapped, 3, 2) &&
            tape.transition(wrapped->id, wrapped->storage,
                            CpuReadyTape::State::Represented,
                            CpuReadyTape::State::Submitted) &&
            tape.complete(wrapped->id, wrapped->storage),
        "wrapped source reaches completed state");
  check(tape.reclaim(second->id, second->storage) &&
            tape.reclaim(wrapped->id, wrapped->storage),
        "FIFO reclaim releases the older source and wrapped allocation");
  check(tape.stats().residentSources == 0u &&
            tape.stats().residentPages == 0u,
        "wrapped payload and its padding both return to zero residency");
}

void emptyTapeCanonicalizesWrapTailForLargestCandidate() {
  CpuReadyTape tape{makeConfig(6, 4, 4, 3, 4, 0, 4, 0)};
  const auto first = tape.reserve(2);
  const auto second = tape.reserve(2);
  check(first && second && second->storage.firstPage == 2,
        "fixture advances the page tail to the high-water boundary");
  const auto complete = [&](const CpuReadyTape::Reservation& source,
                            std::uint64_t ordinal) {
    check(representPublished(tape, source, ordinal) &&
              tape.transition(source.id, source.storage,
                              CpuReadyTape::State::Represented,
                              CpuReadyTape::State::Submitted) &&
              tape.complete(source.id, source.storage),
          "wrap-tail fixture completes a source");
  };
  complete(*first, 1);
  complete(*second, 2);
  check(tape.reclaim(first->id, first->storage) &&
            tape.reclaim(second->id, second->storage) &&
            tape.residentCount() == 0,
        "all resident sources reclaim before the largest candidate");

  check(tape.probeReserve(3) == CpuReadyTape::ReserveProbe::Ready,
        "empty Tape canonicalization removes stale wrap-padding pressure");
  const auto largest = tape.reserve(3);
  check(largest.has_value() && largest->storage.firstPage == 0,
        "largest admissible source restarts at canonical page zero");
}

void sourceAndPageGenerationsRejectAba() {
  CpuReadyTape tape{1};
  const auto original = tape.reserve();
  check(original.has_value(), "test reserves original source");
  const CpuReadySourceId staleId = original->id;
  const CpuReadyStorageRef staleStorage = original->storage;
  ChunkSlot* const payload = original->payload;
  check(tape.abort(original->ticket),
        "writing reservation may abort before publication");

  const auto reused = tape.reserve();
  check(reused.has_value(), "aborted descriptor can be reused");
  check(reused->payload == payload,
        "fixed compatibility lane reuses storage without allocation");
  check(reused->id.index == staleId.index &&
            reused->id.generation != staleId.generation &&
            reused->storage.generation != staleStorage.generation,
        "source and page generations both advance on reuse");
  auto staleTicket = original->ticket;
  staleTicket.id = staleId;
  staleTicket.storage = staleStorage;
  check(tape.resolveForWrite(staleTicket) ==
            nullptr,
        "stale source/storage locator fails before payload dereference");
}

void admissionUsesHighLowHysteresis() {
  CpuReadyTape tape{makeConfig(4, 4, 4, 1, 3, 1, 3, 1)};
  const auto first = tape.reserve();
  const auto second = tape.reserve();
  const auto third = tape.reserve();
  check(first && second && third, "high watermark itself is admissible");
  check(tape.stats().admissionClosed &&
            tape.stats().admissionCloses == 1u,
        "successful reserve closes admission exactly at high water");
  check(!tape.canReserve(),
        "producer observes the sticky latch before another reserve call");

  const auto submit = [&](const CpuReadyTape::Reservation& source,
                          std::uint64_t ordinal) {
    check(representPublished(tape, source, ordinal),
          "source becomes represented");
    check(tape.transition(source.id, source.storage,
                          CpuReadyTape::State::Represented,
                          CpuReadyTape::State::Submitted),
          "source becomes submitted");
  };
  submit(*first, 1);
  submit(*second, 2);
  submit(*third, 3);
  check(tape.complete(first->id, first->storage) &&
            tape.complete(second->id, second->storage) &&
            tape.complete(third->id, third->storage),
        "submitted sources receive explicit completion");
  check(tape.reclaim(first->id, first->storage), "first source reclaims");
  check(!tape.canReserve(),
        "admission stays closed above the low watermark");
  check(tape.reclaim(second->id, second->storage),
        "second source reclaims");
  check(tape.canReserve() && tape.stats().admissionReopens == 1u,
        "ordered reclaim at low water reopens admission exactly once");
}

void pageOnlyPressureUsesIndependentHysteresis() {
  CpuReadyTape tape{makeConfig(6, 6, 6, 3, 6, 0, 4, 1)};
  const auto first = tape.reserve(2);
  const auto second = tape.reserve(2);
  check(first && second && tape.stats().residentSources == 2u &&
            tape.stats().residentPages == 4u &&
            tape.stats().readyPublicationReservations == 2u,
        "page-only fixture stays below source and Ready high water");
  check(tape.stats().admissionClosed && !tape.canReserve(1),
        "page high water independently closes admission");

  const auto complete = [&](const CpuReadyTape::Reservation& source,
                            std::uint64_t seqId) {
    check(representPublished(tape, source, seqId) &&
              tape.transition(source.id, source.storage,
                              CpuReadyTape::State::Represented,
                              CpuReadyTape::State::Submitted) &&
              tape.complete(source.id, source.storage),
          "page-only fixture completes source before reclaim");
  };
  complete(*first, 1);
  complete(*second, 2);
  check(tape.reclaim(first->id, first->storage) &&
            tape.stats().residentPages == 2u && !tape.canReserve(1),
        "page latch remains closed above its low watermark");
  check(tape.reclaim(second->id, second->storage) &&
            tape.stats().residentPages == 0u && tape.canReserve(1) &&
            tape.stats().admissionReopens == 1u,
        "page reclaim at low water reopens admission exactly once");
}

void invalidPageRequestsDoNotCloseAdmission() {
  CpuReadyTape tape{makeConfig(4, 2, 2, 1, 2, 0, 4, 0)};
  check(tape.probeReserve(0) == CpuReadyTape::ReserveProbe::InvalidRequest &&
            tape.probeReserve(2) == CpuReadyTape::ReserveProbe::InvalidRequest,
        "admission probe classifies invalid and oversize requests explicitly");
  check(!tape.reserve(0).has_value(), "zero-page request is invalid");
  check(!tape.reserve(2).has_value(),
        "request beyond the per-source maximum is invalid");
  check(!tape.stats().admissionClosed &&
            tape.stats().admissionCloses == 0u &&
            tape.stats().residentSources == 0u &&
            tape.stats().residentPages == 0u,
        "invalid requests neither latch pressure nor consume residency");
  check(tape.reserve(1).has_value(),
        "a valid request remains admissible after invalid requests");
}

void headCandidatePressureStaysLatchedUntilAllLowWaterBounds() {
  CpuReadyTape tape{makeConfig(6, 4, 4, 3, 4, 0, 4, 0)};
  const auto first = tape.reserve(1);
  const auto second = tape.reserve(1);
  check(first && second && !tape.stats().admissionClosed,
        "below-high residency begins with admission open");
  check(tape.probeReserve(3) ==
            CpuReadyTape::ReserveProbe::TemporaryPressure,
        "head candidate crossing page high-water latches pressure");
  check(tape.stats().admissionClosed &&
            tape.stats().admissionCloses == 1u,
        "head-candidate pressure closes admission before producer wait");

  const auto complete = [&](const CpuReadyTape::Reservation& source,
                            std::uint64_t seqId) {
    check(representPublished(tape, source, seqId) &&
              tape.transition(source.id, source.storage,
                              CpuReadyTape::State::Represented,
                              CpuReadyTape::State::Submitted) &&
              tape.complete(source.id, source.storage),
          "pressure fixture completes source before reclaim");
  };
  complete(*first, 1);
  complete(*second, 2);
  check(tape.reclaim(first->id, first->storage),
        "partial reclaim releases the oldest page");
  check(!tape.canReserve(3) && tape.stats().admissionClosed,
        "candidate stays blocked above every low-water bound");
  check(tape.reclaim(second->id, second->storage),
        "final reclaim reaches all low-water bounds");
  check(tape.canReserve(3) && !tape.stats().admissionClosed &&
            tape.stats().admissionReopens == 1u,
        "all-axis low water reopens the candidate exactly once");
}

void shutdownStopsAdmissionButAllowsRelease() {
  CpuReadyTape tape{2};
  const auto source = tape.reserve();
  check(source.has_value(), "test reserves pre-shutdown source");
  tape.stopAdmission();
  check(!tape.reserve().has_value(), "shutdown rejects new admission");
  check(tape.abort(source->ticket),
        "shutdown still permits rollback needed to release capacity");
  check(tape.residentCount() == 0u,
        "shutdown rollback leaves no resident source");
}

void controlShellDoesNotOwnPayload() {
  CpuReadyTape tape{makeConfig(2, 4, 2, 1, 2, 0, 2, 0)};
  const auto reservation = tape.reserve();
  check(reservation.has_value(), "test reserves compatibility payload");

  ChunkSlotControl control;
  control.state = ChunkSlot::State::Writing;
  control.sourceId = reservation->id;
  control.storage = reservation->storage;
  control.payload = reservation->payload;

  check(tape.capacity() == 4u,
        "source descriptor capacity is independent of payload lane capacity");
  check(sizeof(ChunkSlotControl) < sizeof(ChunkSlot),
        "queue control shell stays materially smaller than payload storage");
}

}  // namespace

int main() {
  try {
    configValidationRejectsUnsafeBounds();
    productionProfilesSeparateSessionPageAndSourceHeadroom();
    readyAdmissionDistinguishesHardHighAndLowWater();
    sealAndPublishFailureLeavesWritingAndCursorsUnchanged();
    representPrefixIsAtomicAndPreservesReadySuffix();
    payloadLayoutIsAlignedBoundedAndOverflowSafe();
    payloadIsInvisibleUntilAtomicSeal();
    abortRollsBackWholeNewestReservation();
    strictAdmissionIdentitySurvivesAbort();
    strictSealRequiresExactTapeOwnedBinding();
    strictArenaDoesNotConsumeCompatibilityCapacity();
    segmentedArenaPublishesAndReclaimsAsOneSource();
    arenaOwnerReclaimIsTwoPhaseAndGenerationScoped();
    rawLegacyAndStrictAdmissionShareRawHighWater();
    compatibilityAndStrictIdentityShareHighWater();
    reservationRejectsCorruptTargetPageWithoutMutation();
    wrapPaddingAndOrderedReclaimAreDeterministic();
    emptyTapeCanonicalizesWrapTailForLargestCandidate();
    sourceAndPageGenerationsRejectAba();
    admissionUsesHighLowHysteresis();
    pageOnlyPressureUsesIndependentHysteresis();
    invalidPageRequestsDoNotCloseAdmission();
    headCandidatePressureStaysLatchedUntilAllLowWaterBounds();
    shutdownStopsAdmissionButAllowsRelease();
    controlShellDoesNotOwnPayload();
  } catch (const std::exception& error) {
    std::cerr << "cpu_ready_tape_spec failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
