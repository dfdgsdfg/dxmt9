#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_descriptors.hpp"
#include "d3d9_pe_capture_state.hpp"
#include "../backend/render_tape_completion_fixture.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace snapshot_transfer_probe {

using dxmt9::d3d9::RenderTapeSnapshotBudget;

struct Allocation {
  void* pointer = nullptr;
  RenderTapeSnapshotBudget* budget = nullptr;
  std::size_t bytes = 0u;
};

std::array<Allocation, 32u> allocations{};
RenderTapeSnapshotBudget* allocatingBudget = nullptr;
std::size_t allocatingBytes = 0u;
bool allocationWasCharged = true;
bool releaseWasCharged = true;
bool failEnabled = false;
std::size_t failAfter = 0u;
std::size_t watchedBytes = 0u;
std::size_t watchedAllocations = 0u;

std::uint64_t liveBytes(RenderTapeSnapshotBudget* budget) noexcept {
  std::uint64_t bytes = 0u;
  for (const auto& allocation : allocations)
    if (allocation.pointer && allocation.budget == budget) bytes += allocation.bytes;
  return bytes;
}

void record(void* pointer, RenderTapeSnapshotBudget* budget, std::size_t bytes) {
  for (auto& allocation : allocations) {
    if (!allocation.pointer) {
      allocation = {pointer, budget, bytes};
      return;
    }
  }
  throw std::runtime_error("snapshot allocation tracker exhausted");
}

} // namespace snapshot_transfer_probe

void* operator new(std::size_t size) {
  using namespace snapshot_transfer_probe;
  if (watchedBytes != 0u && size == watchedBytes) ++watchedAllocations;
  if (allocatingBudget && size == allocatingBytes) {
    allocationWasCharged = allocationWasCharged &&
        allocatingBudget->gauges().retainedCurrentBytes >=
            liveBytes(allocatingBudget) + size;
  }
  if (failEnabled) {
    if (failAfter == 0u) {
      failEnabled = false;
      throw std::bad_alloc();
    }
    --failAfter;
  }
  if (auto* pointer = std::malloc(size == 0u ? 1u : size)) return pointer;
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* pointer) noexcept {
  using namespace snapshot_transfer_probe;
  for (auto& allocation : allocations) {
    if (pointer && allocation.pointer == pointer) {
      releaseWasCharged = releaseWasCharged &&
          allocation.budget->gauges().retainedCurrentBytes >= liveBytes(allocation.budget);
      allocation = {};
      break;
    }
  }
  std::free(pointer);
}

void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { ::operator delete(pointer); }

namespace {

using namespace dxmt9::d3d9;
namespace fixture = dxmt9::d3d9::test;

static_assert(std::is_nothrow_move_constructible_v<RenderTapeCaptureBlob>);
static_assert(std::is_nothrow_move_assignable_v<RenderTapeCaptureBlob>);
static_assert(std::is_copy_constructible_v<RenderTapeCaptureBlob>);
static_assert(std::is_copy_assignable_v<RenderTapeCaptureBlob>);
static_assert(!std::is_aggregate_v<RenderTapeCaptureBlob>);
static_assert(std::is_nothrow_move_constructible_v<RenderTapeArmObjectSnapshot>);
static_assert(std::is_nothrow_move_assignable_v<RenderTapeArmObjectSnapshot>);
static_assert(!std::is_copy_constructible_v<RenderTapeArmObjectSnapshot>);
static_assert(!std::is_copy_assignable_v<RenderTapeArmObjectSnapshot>);

void check(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

void initialize(RenderTapeSnapshotBudget& budget, std::uint64_t bytes = 65536u) {
  check(budget.initialize(bytes) == RenderTapeSnapshotBudgetStatus::Complete,
        "snapshot budget initializes once from capture limit");
}

// This is the producer order, not a retrospective charge for an existing
// vector: reserve, allocate, bind, then transfer the receipt with the vector.
RenderTapeCaptureBlob snapshot(RenderTapeSnapshotBudget& budget, std::size_t bytes,
                               std::byte value = std::byte{0x35u}) {
  auto reservation = budget.reserveRetained(bytes);
  check(reservation.status == RenderTapeSnapshotBudgetStatus::Complete,
        "producer reserves retained B before allocating bytes");
  RenderTapeCaptureBlob result;
  result.retainedLease = std::move(reservation.lease);
  snapshot_transfer_probe::allocatingBudget = &budget;
  snapshot_transfer_probe::allocatingBytes = bytes;
  try {
    result.bytes.assign(bytes, value);
  } catch (...) {
    snapshot_transfer_probe::allocatingBudget = nullptr;
    snapshot_transfer_probe::allocatingBytes = 0u;
    throw;
  }
  snapshot_transfer_probe::allocatingBudget = nullptr;
  snapshot_transfer_probe::allocatingBytes = 0u;
  snapshot_transfer_probe::record(result.bytes.data(), &budget, bytes);
  check(result.retainedLease.bindRetainedPayload(result.bytes.data(), result.bytes.size()),
        "producer binds one retained receipt to its original allocation");
  return result;
}

RenderTapeArmObjectSnapshot armSnapshot(RenderTapeSnapshotBudget& budget,
                                       std::size_t bytes, std::uint64_t ordinal) {
  auto raw = snapshot(budget, bytes);
  RenderTapeArmObjectSnapshot result;
  result.armOrdinal = ordinal;
  result.identity = {D9C_CHUNK_HANDLE_KIND_BUFFER, 1u, ordinal};
  result.retainedLeases.push_back(std::move(raw.retainedLease));
  result.content.push_back(std::move(raw.bytes));
  return result;
}

void armEmpty(RenderTapeCaptureSession& session, const std::vector<std::byte>& bootstrap) {
  check(session.arm(bootstrap) == RenderTapeCaptureStatus::Accepted,
        "empty capture arms before runtime byte admission");
}

void defineBuffer(RenderTapeCaptureSession& session, std::size_t bytes) {
  const D9CWireObjectIdentity identity{D9C_CHUNK_HANDLE_KIND_BUFFER, 1u, 1u};
  const D9CBufferDesc descriptor{.size = static_cast<std::uint32_t>(bytes)};
  check(session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted &&
            session.objectDefine(identity,
                static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
                std::as_bytes(std::span(&descriptor, 1u)), 0u, {}, bytes, 1u) ==
                RenderTapeCaptureStatus::Accepted,
        "runtime fixture defines complete buffer content");
}

void seal(RenderTapeCaptureSession& session) {
  const D9CWireObjectIdentity output{D9C_CHUNK_HANDLE_KIND_SURFACE, 1u, 2u};
  const RenderTapeSurfaceDescriptorV2 descriptor{
      .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::SwapchainBackbuffer),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedPresentOutput),
      .surface = {.format = 21u, .resourceType = 1u, .usage = 1u,
                  .width = 4u, .height = 4u, .depth = 1u}};
  const RenderTapeOracleAttachment oracle{
      .identity = output,
      .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface)};
  if (session.state() == RenderTapeCaptureState::Armed)
    check(session.beginPresentInterval() == RenderTapeCaptureStatus::Accepted,
          "publication begins one Present interval");
  check(session.objectDefine(output,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            std::as_bytes(std::span(&descriptor, 1u)), 0u, {}) ==
            RenderTapeCaptureStatus::Accepted &&
            session.commandChunk({.recordCount = 1u}, fixture::presentChunk()) ==
                RenderTapeCaptureStatus::Accepted &&
            session.completePresent(session.eventCount(), 1u,
                RenderTapeDigestValidity::NotCaptured, {},
                std::as_bytes(std::span(&oracle, 1u))) == RenderTapeCaptureStatus::Complete,
        "retained payload publishes through the actual validated frame owner");
}

void immutableMovesAndFinalRelease() {
  RenderTapeSnapshotBudget budget;
  initialize(budget);
  auto raw = snapshot(budget, 8197u);
  const auto* pointer = raw.bytes.data();
  RenderTapeImmutablePayload first(std::move(raw.bytes), raw.retainedLease);
  raw.retainedLease.reset();
  check(raw.bytes.empty() && first.data() == pointer &&
            budget.gauges().retainedCurrentBytes == 8197u,
        "immutable adoption preserves pointer and does not reserve twice");
  auto second = first;
  auto moved = std::move(first);
  second = {};
  check(!first.hasBacking() && moved.data() == pointer &&
            budget.gauges().retainedCurrentBytes == 8197u,
        "copies and moves share one backing and one retained charge");
  moved = {};
  check(budget.gauges().retainedCurrentBytes == 0u &&
            budget.gauges().retainedPeakBytes == 8197u,
        "final immutable owner releases B only after raw allocation dies");

  RenderTapeImmutablePayload uncharged(std::vector<std::byte>(3u));
  RenderTapeCpuShadowContent shadow(std::move(uncharged));
  check(shadow.freeze().scratchBacked() && budget.gauges().retainedCurrentBytes == 0u,
        "durable CPU-A scratch remains outside retained GPU-B reservations");
}

void populatedBlobAssignmentsAndCheckedCopies() {
  RenderTapeSnapshotBudget budget;
  initialize(budget);
  auto destination = snapshot(budget, 8197u);
  auto source = snapshot(budget, 8201u);
  const auto* sourcePointer = source.bytes.data();
  destination = std::move(source);
  check(destination.bytes.data() == sourcePointer && source.bytes.empty() &&
            !source.retainedLease.valid() &&
            destination.retainedLease.boundPayloadMatches(sourcePointer, 8201u) &&
            budget.gauges().retainedCurrentBytes == 8201u &&
            snapshot_transfer_probe::liveBytes(&budget) == 8201u,
        "populated blob move assignment frees old allocation before returning old credit");
  auto* self = &destination;
  destination = *self;
  destination = std::move(*self);
  check(destination.bytes.data() == sourcePointer &&
            budget.gauges().retainedCurrentBytes == 8201u,
        "self copy/move assignment preserves the exact retained owner");

  auto preserved = snapshot(budget, 8197u);
  const auto* preservedPointer = preserved.bytes.data();
  snapshot_transfer_probe::watchedBytes = 8201u;
  snapshot_transfer_probe::watchedAllocations = 0u;
  bool copyRejected = false, assignmentRejected = false;
  try { RenderTapeCaptureBlob illegal(destination); }
  catch (const std::invalid_argument&) { copyRejected = true; }
  try { preserved = destination; }
  catch (const std::invalid_argument&) { assignmentRejected = true; }
  std::vector<RenderTapeCaptureBlob> relocations;
  relocations.push_back(std::move(destination));
  relocations.push_back(snapshot(budget, 8203u));
  const auto copiedAllocations = snapshot_transfer_probe::watchedAllocations;
  snapshot_transfer_probe::watchedBytes = 0u;
  check(copyRejected && assignmentRejected && copiedAllocations == 0u &&
            preserved.bytes.data() == preservedPointer &&
            relocations[0].bytes.data() == sourcePointer &&
            relocations[0].retainedLease.boundPayloadMatches(sourcePointer, 8201u),
        "leased copy rejects before payload allocation and vector relocation uses noexcept moves");

  RenderTapeCaptureBlob uncharged{std::vector<std::byte>(7u, std::byte{0x41u})};
  snapshot_transfer_probe::failAfter = 0u;
  snapshot_transfer_probe::failEnabled = true;
  bool failed = false;
  try { preserved = uncharged; }
  catch (const std::bad_alloc&) { failed = true; }
  snapshot_transfer_probe::failEnabled = false;
  check(failed && preserved.bytes.data() == preservedPointer &&
            preserved.retainedLease.boundPayloadMatches(preservedPointer, 8197u) &&
            budget.gauges().retainedCurrentBytes == 24601u,
        "copy assignment allocation failure preserves populated destination and its receipt");
  preserved = uncharged;
  check(!preserved.retainedLease.valid() && preserved.bytes == uncharged.bytes &&
            preserved.bytes.data() != uncharged.bytes.data() &&
            budget.gauges().retainedCurrentBytes == 16404u,
        "uncharged fixture copy compatibility safely retires a populated leased destination");
  relocations.clear();
  check(budget.gauges().retainedCurrentBytes == 0u,
        "relocated blob owners release exactly their original reservations");

  auto frozenRaw = snapshot(budget, 257u);
  RenderTapeCaptureBlob frozen{
      RenderTapeImmutablePayload(std::move(frozenRaw.bytes), frozenRaw.retainedLease)};
  frozenRaw.retainedLease.reset();
  auto shared = frozen;
  preserved = frozen;
  check(frozen.immutable.backingIdentity() == shared.immutable.backingIdentity() &&
            frozen.immutable.backingIdentity() == preserved.immutable.backingIdentity() &&
            budget.gauges().retainedCurrentBytes == 257u,
        "immutable CaptureBlob copies share backing without hidden retained raw copies");
}

void populatedArmSnapshotAssignments() {
  RenderTapeSnapshotBudget budget;
  initialize(budget);
  auto destination = armSnapshot(budget, 8197u, 1u);
  auto source = armSnapshot(budget, 8201u, 2u);
  const auto* pointer = source.content[0].data();
  destination = std::move(source);
  check(destination.armOrdinal == 2u && destination.identity.objectId == 2u &&
            destination.content[0].data() == pointer && source.content.empty() &&
            source.retainedLeases.empty() &&
            destination.retainedLeases[0].boundPayloadMatches(pointer, 8201u) &&
            budget.gauges().retainedCurrentBytes == 8201u &&
            snapshot_transfer_probe::liveBytes(&budget) == 8201u,
        "populated arm snapshot move assignment frees old vectors before receipt release");
  auto* self = &destination;
  destination = std::move(*self);
  auto moved = std::move(destination);
  check(moved.content[0].data() == pointer && destination.content.empty() &&
            budget.gauges().retainedCurrentBytes == 8201u,
        "move-only arm snapshots preserve bound allocation through self move and relocation");
  moved = RenderTapeArmObjectSnapshot{};
  check(budget.gauges().retainedCurrentBytes == 0u,
        "empty replacement retires final arm snapshot bytes before returning credit");
}

void rejectsUnboundWrongKindSizeAndAllocation() {
  RenderTapeSnapshotBudget budget;
  initialize(budget);
  auto source = snapshot(budget, 257u);
  const auto reject = [&](std::vector<std::byte>& bytes,
                          const RenderTapeSnapshotBudgetLease& lease) {
    const auto* pointer = bytes.data();
    const auto size = bytes.size();
    bool rejected = false;
    try { RenderTapeImmutablePayload bad(std::move(bytes), lease); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected && bytes.size() == size && bytes.data() == pointer && lease.valid(),
          "invalid immutable lease rejects before moving vector or releasing receipt");
  };
  auto transient = budget.reserveTransient(257u, 257u);
  auto wrongSize = budget.reserveRetained(258u);
  auto unbound = budget.reserveRetained(257u);
  reject(source.bytes, transient.lease);
  reject(source.bytes, wrongSize.lease);
  reject(source.bytes, unbound.lease);
  auto independent = source.bytes;
  check(!source.retainedLease.bindRetainedPayload(independent.data(), independent.size()),
        "a receipt cannot be rebound to a copied same-N allocation");
  reject(independent, source.retainedLease);
  check(budget.gauges().retainedCurrentBytes == 772u,
        "rejections do not mutate retained accounting");

  const auto bootstrap = fixture::fullStateChunk(false);
  RenderTapeCaptureSession borrowed(true);
  check(borrowed.armWithBlobs(bootstrap, std::span(&source, 1u)) ==
            RenderTapeCaptureStatus::InvalidInput && source.bytes.size() == 257u &&
            borrowed.ownedPublishedBlobs().empty(),
        "borrowed raw admission cannot reuse a reservation for a second buffer");
  RenderTapeImmutablePayload shared(std::move(source.bytes), source.retainedLease);
  source.retainedLease.reset();
  const RenderTapeCaptureBlob frozen{shared};
  check(borrowed.armWithBlobs(bootstrap, std::span(&frozen, 1u)) ==
            RenderTapeCaptureStatus::Accepted &&
            borrowed.ownedPublishedBlobs()[0].bytes.backingIdentity() == shared.backingIdentity(),
        "borrowed immutable admission shares an existing allocation legally");
}

void armDedupReleaseAbortAndRearm() {
  RenderTapeSnapshotBudget budget;
  initialize(budget);
  const auto bootstrap = fixture::fullStateChunk(false);
  RenderTapeCaptureSession session(true);
  std::vector<RenderTapeCaptureBlob> seed;
  seed.push_back(snapshot(budget, 4097u));
  seed.push_back(snapshot(budget, 4097u));
  const auto* first = seed[0].bytes.data();
  check(budget.gauges().retainedCurrentBytes == 8194u &&
            session.armWithOwnedBlobs(bootstrap, std::move(seed)) ==
                RenderTapeCaptureStatus::Accepted && seed.empty() &&
            session.ownedPublishedBlobs().size() == 1u &&
            session.ownedPublishedBlobs()[0].bytes.data() == first &&
            budget.gauges().retainedCurrentBytes == 4097u,
        "exact dedup frees redundant vector before releasing its independent charge");
  auto hold = session.ownedPublishedBlobs()[0].bytes;
  session.abort();
  check(budget.gauges().retainedCurrentBytes == 4097u,
        "abort cannot release a retained allocation still shared outside session");
  std::vector<RenderTapeCaptureBlob> next;
  next.push_back(snapshot(budget, 4099u, std::byte{0x72u}));
  check(session.armWithOwnedBlobs(bootstrap, std::move(next)) ==
            RenderTapeCaptureStatus::Accepted &&
            budget.gauges().retainedCurrentBytes == 8196u,
        "rearm reserves fresh B without erasing a prior live publication owner");
  hold = {};
  check(budget.gauges().retainedCurrentBytes == 4099u,
        "old receipt releases independently of a new capture epoch");
  session.abort();
  check(budget.gauges().retainedCurrentBytes == 0u,
        "abort releases the final capture-owned retained payload");
}

void runtimeDedupAndPublicationReferences() {
  RenderTapeSnapshotBudget budget;
  initialize(budget);
  const auto bootstrap = fixture::fullStateChunk(false);
  RenderTapeCaptureSession session(true);
  armEmpty(session, bootstrap);
  auto first = snapshot(budget, 4097u);
  const auto* pointer = first.bytes.data();
  check(session.registerOwnedBlobBytes(std::move(first.bytes), nullptr, first.retainedLease) ==
            RenderTapeCaptureStatus::Accepted && first.bytes.empty(),
        "runtime consuming registration adopts exact bound reservation");
  first.retainedLease.reset();
  auto duplicate = snapshot(budget, 4097u);
  check(session.registerOwnedBlobBytes(std::move(duplicate.bytes), nullptr,
            duplicate.retainedLease) == RenderTapeCaptureStatus::Accepted &&
            duplicate.bytes.empty() && budget.gauges().retainedCurrentBytes == 8194u,
        "const-ref handoff keeps caller receipt until redundant bytes have been freed");
  duplicate.retainedLease.reset();
  check(budget.gauges().retainedCurrentBytes == 4097u &&
            session.ownedPublishedBlobs().size() == 1u,
        "runtime exact dedup shares payload rather than duplicating reservation");
  seal(session);
  auto publication = session.publicationBundle();
  auto secondPublication = publication;
  session.abort();
  check(publication.blobs[0].bytes.data() == pointer &&
            budget.gauges().retainedCurrentBytes == 4097u,
        "publication bundle copies carry payload authority through session abort");
  publication = {};
  check(budget.gauges().retainedCurrentBytes == 4097u,
        "nonfinal publication release cannot return retained credit");
  secondPublication = {};
  check(budget.gauges().retainedCurrentBytes == 0u,
        "final publication copy frees bytes before returning retained credit");
}

void consumingRejectionsAreFailStop() {
  RenderTapeSnapshotBudget budget;
  initialize(budget);
  const auto bootstrap = fixture::fullStateChunk(false);
  for (const bool arm : {false, true}) {
    auto source = snapshot(budget, 257u);
    auto copy = source.bytes;
    RenderTapeCaptureSession session(true);
    if (arm) {
      std::vector<RenderTapeCaptureBlob> bad;
      bad.emplace_back(RenderTapeImmutablePayload::copyToScratch(source.readBytes(), {}));
      RenderTapeCaptureBlob invalidAllocation{std::move(copy)};
      invalidAllocation.retainedLease = source.retainedLease;
      bad.push_back(std::move(invalidAllocation));
      check(session.armWithOwnedBlobs(bootstrap, std::move(bad)) ==
                RenderTapeCaptureStatus::InvalidInput && bad.empty(),
            "owned arm rejects independently copied allocation with shared receipt");
    } else {
      armEmpty(session, bootstrap);
      check(session.registerBlobBytes(source.readBytes()) == RenderTapeCaptureStatus::Accepted,
            "copied-allocation rejection fixture already has exact matching content");
      check(session.registerOwnedBlobBytes(std::move(copy), nullptr, source.retainedLease) ==
                RenderTapeCaptureStatus::InvalidInput && copy.empty() &&
                session.state() == RenderTapeCaptureState::Aborted,
            "runtime wrong-allocation lease consumes incoming vector and fail-stops");
    }
    check(source.bytes.size() == 257u && budget.gauges().retainedCurrentBytes == 257u &&
              session.ownedPublishedBlobs().empty(),
          "rejection preserves original leased owner and publishes no copied bytes");
  }
  check(budget.gauges().retainedCurrentBytes == 0u,
        "rejected handoffs release original receipt at its actual final owner");
}

void allocationFaultsKeepChargeUntilBytesDie() {
  RenderTapeSnapshotBudget budget;
  initialize(budget);
  const auto bootstrap = fixture::fullStateChunk(false);
  const D9CWireObjectIdentity buffer{D9C_CHUNK_HANDLE_KIND_BUFFER, 1u, 1u};
  bool armFailed = false, armCompleted = false;
  bool mutationFailed = false, mutationCompleted = false;
  bool payloadOwnerFailed = false, publicationFailed = false, eventFailed = false;
  for (std::size_t allocation = 0u; allocation < 32u; ++allocation) {
    RenderTapeCaptureSession session(true);
    std::vector<RenderTapeCaptureBlob> seed;
    seed.push_back(snapshot(budget, 257u));
    snapshot_transfer_probe::failAfter = allocation;
    snapshot_transfer_probe::failEnabled = true;
    const auto status = session.armWithOwnedBlobs(bootstrap, std::move(seed));
    snapshot_transfer_probe::failEnabled = false;
    check(seed.empty(), "arm fault consumes every incoming raw allocation");
    if (status == RenderTapeCaptureStatus::Accepted) {
      armCompleted = true;
      check(budget.gauges().retainedCurrentBytes == 257u,
            "successful arm faults sweep retains exactly one charged backing");
    } else {
      armFailed = true;
      check(status == RenderTapeCaptureStatus::CapacityExceeded &&
                session.ownedPublishedBlobs().empty() &&
                budget.gauges().retainedCurrentBytes == 0u,
            "every arm allocation rejection frees raw vectors before their receipts");
    }
    session.abort();
    check(budget.gauges().retainedCurrentBytes == 0u,
          "fault fixture does not leak retained admission");
  }
  for (std::size_t allocation = 0u; allocation < 32u; ++allocation) {
    RenderTapeCaptureSession session(true);
    armEmpty(session, bootstrap);
    defineBuffer(session, 257u);
    auto input = snapshot(budget, 257u);
    snapshot_transfer_probe::failAfter = allocation;
    snapshot_transfer_probe::failEnabled = true;
    const auto status = session.resourceMutationOwnedBytes(buffer,
        RenderTapeMutationKind::Upload, 0u, 0u, std::move(input.bytes),
        RenderTapeBufferMutationDisposition::Plain, input.retainedLease);
    snapshot_transfer_probe::failEnabled = false;
    check(input.bytes.empty() && budget.gauges().retainedCurrentBytes == 257u,
          "caller receipt protects every consumed mutation failure until safe reset");
    input.retainedLease.reset();
    if (status == RenderTapeCaptureStatus::Accepted) {
      mutationCompleted = true;
      check(budget.gauges().retainedCurrentBytes == 257u,
            "successful mutation stores the original reservation without readmission");
    } else {
      mutationFailed = true;
      const auto& failure = session.mutationFailure();
      payloadOwnerFailed |= std::string_view(failure.phase) == "blob_payload_owner";
      publicationFailed |= std::string_view(failure.phase) == "blob_publication_append";
      eventFailed |= std::string_view(failure.layer) == "resource_mutation";
      check(status == RenderTapeCaptureStatus::CapacityExceeded &&
                session.state() == RenderTapeCaptureState::Aborted &&
                session.ownedPublishedBlobs().empty() &&
                budget.gauges().retainedCurrentBytes == 0u,
            "owner/publication/event failure aborts with no retained allocation leak");
    }
    session.abort();
    check(budget.gauges().retainedCurrentBytes == 0u,
          "mutation fault fixture releases any successful backing at abort");
  }
  check(armFailed && armCompleted && mutationFailed && mutationCompleted &&
            payloadOwnerFailed && publicationFailed && eventFailed,
        "fault sweep reaches both completed paths and payload/publication/event allocation failures");

  auto input = snapshot(budget, 257u);
  const auto* pointer = input.bytes.data();
  snapshot_transfer_probe::failAfter = 0u;
  snapshot_transfer_probe::failEnabled = true;
  bool rejected = false;
  try { RenderTapeImmutablePayload payload(std::move(input.bytes), input.retainedLease); }
  catch (const std::bad_alloc&) { rejected = true; }
  snapshot_transfer_probe::failEnabled = false;
  check(rejected && input.bytes.data() == pointer && input.bytes.size() == 257u &&
            budget.gauges().retainedCurrentBytes == 257u,
        "immutable metadata allocation failure precedes raw-vector move");
}

} // namespace

int main() {
  try {
    immutableMovesAndFinalRelease();
    populatedBlobAssignmentsAndCheckedCopies();
    populatedArmSnapshotAssignments();
    rejectsUnboundWrongKindSizeAndAllocation();
    armDedupReleaseAbortAndRearm();
    runtimeDedupAndPublicationReferences();
    consumingRejectionsAreFailStop();
    allocationFaultsKeepChargeUntilBytesDie();
    check(snapshot_transfer_probe::allocationWasCharged &&
              snapshot_transfer_probe::releaseWasCharged,
          "allocation hooks prove reservation-before-vector and free-before-credit-return");
    for (const auto& allocation : snapshot_transfer_probe::allocations)
      check(!allocation.pointer, "every tracked original snapshot allocation is freed");
    std::cout << "render_tape_snapshot_budget_transfer_spec passed\n";
    return 0;
  } catch (const std::exception& error) {
    snapshot_transfer_probe::failEnabled = false;
    std::cerr << "render_tape_snapshot_budget_transfer_spec failed: " << error.what() << '\n';
    return 1;
  }
}
