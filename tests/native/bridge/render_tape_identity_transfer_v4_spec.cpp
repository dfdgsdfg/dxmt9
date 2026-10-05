#include "device_c_render_tape_identity_transfer_v4.hpp"
#include "device_c_render_tape_identity_transfer_v3.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace transfer_v4_allocation_probe {
bool enabled = false;
std::size_t failAfter = 0u;
} // namespace transfer_v4_allocation_probe

void* operator new(std::size_t size) {
  if (transfer_v4_allocation_probe::enabled) {
    if (!transfer_v4_allocation_probe::failAfter) {
      transfer_v4_allocation_probe::enabled = false;
      throw std::bad_alloc();
    }
    --transfer_v4_allocation_probe::failAfter;
  }
  if (auto* value = std::malloc(size)) return value;
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete(void* value, std::size_t) noexcept { ::operator delete(value); }
void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }

namespace {

using namespace dxmt9::d3d9;
using Status = RenderTapeIdentityV4TransferStatus;
constexpr RenderTapeIdentityV4CaptureKey kCapture{7u, 8u};
constexpr std::uint64_t kQueue = 9u;

static_assert(kRenderTapeIdentityV4TransferMagic != kRenderTapeIdentityV3TransferMagic);
static_assert(kRenderTapeIdentityV4TransferVersion != kRenderTapeIdentityV3TransferVersion);
static_assert(kRenderTapeIdentityV4TransferMagic == 0x33564e5354523944ull);
static_assert(kRenderTapeIdentityV4TransferLegacyMagic == 0x32564e5354523944ull);

RenderTapeActionWitnessV1 knownWitness() {
  return {.evidence = static_cast<std::uint32_t>(RenderTapeActionWitnessEvidenceV1::QueueObserved),
      .constructionProfile = 1u, .armProfile = 1u, .freezeProfile = 1u,
      .captureToken = kCapture.token, .captureEpoch = kCapture.epoch, .queue = kQueue};
}

void check(bool value, std::string_view message) {
  if (!value) throw std::runtime_error(std::string(message));
}

struct SnapshotPair {
  RenderTapeIdentityV4View identity{};
  RenderTapeExecutionTopologyV1View topology{};
};

SnapshotPair snapshotFixture() {
  SnapshotPair pair;
  auto& i = pair.identity;
  i.header.frameId = 9007199254740993ull;
  i.header.presentOrdinal = 4u;
  i.header.captureToken = kCapture.token;
  i.header.captureEpoch = kCapture.epoch;
  i.header.queue = kQueue;
  i.header.initializerEpoch = 3u;
  i.header.baselineCompletionEdge = 1u;
  i.header.baselineKind = static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::EmptyBaseline);
  i.header.sources = i.header.initializers = i.header.commands = i.header.dispositions =
      i.header.coordinators = i.header.settlements = i.header.attempts = {99u, 99u, 99u};
  i.sources = {{.lifecycle = {.sourceOrdinal = 9007199254740995ull, .seqId = 1u,
      .writerCaptureId = 2u, .writerGeneration = 3u, .storageGeneration = 4u,
      .publicationEdge = 10u, .submissionEdge = 30u, .completionEdge = 40u,
      .completedSequence = 1u, .cutEventOrdinal = 3u, .commandCount = 2u,
      .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterEvent)},
      .rawOrdinal = 30u, .publicationBuildGeneration = 11u, .publicationControlIndex = 7u}};
  i.initializers = {{.sequence = 5u, .epoch = 3u, .publicationEdge = 6u,
      .submissionEdge = 7u, .completionEdge = 8u, .completedSequence = 5u,
      .cutEventOrdinal = 3u,
      .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterRecord),
      .cutRecord = 1u}};
  i.commands = {
      {.sourceIndex = 0u, .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::DrawRun),
       .drawCount = 2u},
      {.sourceIndex = 0u, .physicalCommandIndex = 1u,
       .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::Present)},
  };
  i.dispositions = {
      {.eventOrdinal = 2u, .recordCount = 1u,
       .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::StateOnly)},
      {.eventOrdinal = 3u, .recordCount = 1u,
       .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::Coordinator),
       .coordinatorIndex = 0u},
      {.eventOrdinal = 3u, .firstRecord = 1u, .recordCount = 2u,
       .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::GpuCommand),
       .commandIndex = 0u, .subdrawCount = 2u},
      {.eventOrdinal = 3u, .firstRecord = 3u, .recordCount = 1u,
       .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::GpuCommand),
       .commandIndex = 1u},
  };
  i.coordinators = {{.provenance = {.eventOrdinal = 3u, .rawOrdinal = 30u, .buildGeneration = 11u,
      .dispatchEdge = 9u, .releaseOrdinal = 2u, .releaseGeneration = 3u,
      .queryIdentity = {D9C_CHUNK_HANDLE_KIND_QUERY, 2u, 44u},
      .issueCycle = 5u, .issueOrdinal = 6u, .deviceEpoch = 7u,
      .prefixInitializer = 5u, .prefixInitializerEpoch = 3u,
      .recordType = D9C_COMMAND_RECORD_QUERY_ISSUE, .flags = 1u, .queryType = 8u,
      .prefixKind = static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::EmptyBaseline),
      .initializerIndex = 0u,
      .observation = static_cast<std::uint32_t>(RenderTapeIdentityV3Observation::Completed)}}};
  i.settlements = {{2u, 20u, 4u, 1u}, {3u, 30u, 18u, 4u}};
  i.attempts = {{.eventOrdinal = 3u, .rawOrdinal = 30u, .attemptGeneration = 11u,
      .beginEdge = 2u, .terminalEdge = 17u, .effectEdge = 16u,
      .identity = {9007199254740993ull, 30u, 30u, 11u},
      .receipt = {.identity = {9007199254740993ull, 30u, 30u, 11u},
          .queueSequence = 1u, .buildGeneration = 11u, .sourceGeneration = 3u,
          .storageGeneration = 4u,
          .kind = static_cast<std::uint32_t>(RenderTapeIdentityV4ReceiptKind::DirectChunkSlot),
          .controlIndex = 7u, .commandCount = 2u},
      .recordCount = 4u,
      .outcome = static_cast<std::uint32_t>(RenderTapeIdentityV4AttemptOutcome::Committed),
      .stagedCommandCount = 2u}};
  auto& t = pair.topology;
  t.header.captureToken = kCapture.token;
  t.header.captureEpoch = kCapture.epoch;
  t.header.queue = kQueue;
  t.header.plans = t.header.placements = {99u, 99u, 99u};
  t.plans = {{.planId = 19u, .acceptanceEdge = 20u,
      .route = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Route::Optimized),
      .placementCount = 2u, .executedCount = 1u}};
  t.placements = {
      {.planIndex = 0u, .commandIndex = 0u,
       .outcome = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Outcome::ProvenOmitted),
       .omissionReason = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1OmissionReason::DeadPass)},
      {.planIndex = 0u, .commandIndex = 1u,
       .outcome = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Outcome::ExecutedWithoutDag),
       .executionOrdinal = 0u},
  };
  return pair;
}

template <typename T>
bool sameRows(const std::vector<T>& a, const std::vector<T>& b) {
  return a.size() == b.size() &&
      (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

bool emptyPair(const SnapshotPair& pair) {
  const auto& i = pair.identity;
  const auto& t = pair.topology;
  return !i.header.captureToken && !i.header.captureEpoch && !i.header.queue &&
      i.sources.empty() && i.initializers.empty() && i.commands.empty() &&
      i.dispositions.empty() && i.coordinators.empty() && i.settlements.empty() && i.attempts.empty() &&
      !t.header.captureToken && !t.header.captureEpoch && !t.header.queue &&
      t.plans.empty() && t.placements.empty();
}

void rejectedDecode(std::span<const std::byte> bytes, Status expected,
                    RenderTapeIdentityV4CaptureKey capture = kCapture,
                    std::uint64_t queue = kQueue) {
  auto out = snapshotFixture();
  auto witness = knownWitness();
  const auto status = decodeRenderTapeIdentityV4SnapshotTransfer(
      bytes, capture, queue, out.identity, out.topology, &witness);
  check(status == expected && emptyPair(out) && witness == RenderTapeActionWitnessV1{},
        "failed structural decode clears prior/partial pair and action witness values");
}

template <typename Header>
std::array<RenderTapeIdentityV3Table*, 9> tables(Header& h) {
  return {&h.identity.sources, &h.identity.initializers, &h.identity.commands,
      &h.identity.dispositions, &h.identity.coordinators, &h.identity.settlements,
      &h.identity.attempts, &h.topology.plans, &h.topology.placements};
}

template <typename T>
void canonicalTable(RenderTapeIdentityV3Table table, std::uint64_t& cursor,
                    const std::vector<T>& rows) {
  check(table.offset == cursor && table.count == rows.size() && table.entrySize == sizeof(T),
        "owned vectors determine exact canonical table offsets, counts and native POD entry sizes");
  cursor += rows.size() * std::uint64_t{sizeof(T)};
}

void pairedRoundTripAndAuthoritySeparation() {
  const auto snapshot = snapshotFixture();
  std::vector<std::byte> bytes;
  check(encodeRenderTapeIdentityV4SnapshotTransfer(snapshot.identity, snapshot.topology, bytes) == Status::Valid,
        "one paired neutral snapshot uses a distinct bounded transport grammar");
  RenderTapeIdentityV4TransferHeader h{};
  std::memcpy(&h, bytes.data(), sizeof(h));
  check(h.magic == kRenderTapeIdentityV4TransferMagic && h.version == 3u &&
            h.headerSize == 512u && h.totalBytes == bytes.size() &&
            h.actionWitness == RenderTapeActionWitnessV1{} &&
            !h.identity.eventsBytes && h.identity.eventsDigest == RenderTapeDigest{} &&
            !h.topology.identityBytes && !h.topology.eventsBytes &&
            h.topology.identityDigest == RenderTapeDigest{} && h.topology.eventsDigest == RenderTapeDigest{},
        "snapshot transfer preserves zero provisional Tape and identity byte/digest authority");
  std::uint64_t cursor = sizeof(h);
  canonicalTable(h.identity.sources, cursor, snapshot.identity.sources);
  canonicalTable(h.identity.initializers, cursor, snapshot.identity.initializers);
  canonicalTable(h.identity.commands, cursor, snapshot.identity.commands);
  canonicalTable(h.identity.dispositions, cursor, snapshot.identity.dispositions);
  canonicalTable(h.identity.coordinators, cursor, snapshot.identity.coordinators);
  canonicalTable(h.identity.settlements, cursor, snapshot.identity.settlements);
  canonicalTable(h.identity.attempts, cursor, snapshot.identity.attempts);
  canonicalTable(h.topology.plans, cursor, snapshot.topology.plans);
  canonicalTable(h.topology.placements, cursor, snapshot.topology.placements);
  check(cursor == bytes.size(), "all nine tables exactly fill the transfer with no trailing bytes");
  SnapshotPair decoded;
  auto witness = knownWitness();
  check(decodeRenderTapeIdentityV4SnapshotTransfer(bytes, kCapture, kQueue,
            decoded.identity, decoded.topology, &witness) == Status::Valid &&
            witness == RenderTapeActionWitnessV1{} &&
            !renderTapeActionWitnessV1StableExact(witness) &&
            sameRows(snapshot.identity.sources, decoded.identity.sources) &&
            sameRows(snapshot.identity.initializers, decoded.identity.initializers) &&
            sameRows(snapshot.identity.commands, decoded.identity.commands) &&
            sameRows(snapshot.identity.dispositions, decoded.identity.dispositions) &&
            sameRows(snapshot.identity.coordinators, decoded.identity.coordinators) &&
            sameRows(snapshot.identity.settlements, decoded.identity.settlements) &&
            sameRows(snapshot.identity.attempts, decoded.identity.attempts) &&
            sameRows(snapshot.topology.plans, decoded.topology.plans) &&
            sameRows(snapshot.topology.placements, decoded.topology.placements),
        "trusted pair joins all nine tables while the old encode API carries only LegacyUnknown");
  check(std::memcmp(&decoded.identity.header, &h.identity, sizeof(h.identity)) == 0 &&
            std::memcmp(&decoded.topology.header, &h.topology, sizeof(h.topology)) == 0 &&
            decoded.identity.commands[0].drawCount == 2u &&
            decoded.topology.placements[0].outcome == static_cast<std::uint32_t>(
                RenderTapeExecutionTopologyV1Outcome::ProvenOmitted),
        "neutral physical DrawRun and explicit omission remain separate, never fabricated v3 Pass membership");
  std::vector<std::byte> repeated;
  check(encodeRenderTapeIdentityV4SnapshotTransfer(decoded.identity, decoded.topology, repeated) == Status::Valid &&
            repeated == bytes && snapshot.identity.header.attempts.offset == 99u &&
            snapshot.topology.header.placements.count == 99u,
        "transfer deterministically rebuilds only descriptors without modifying caller-owned metadata");
  bytes.assign(bytes.size(), std::byte{});
  check(decoded.identity.sources[0].lifecycle.sourceOrdinal == 9007199254740995ull &&
            decoded.identity.attempts[0].receipt.controlIndex == 7u && decoded.topology.plans[0].planId == 19u,
        "decoded values do not retain borrowed transfer storage");

  auto incomplete = snapshot;
  incomplete.identity.sources[0].lifecycle.completionEdge = 0u;
  incomplete.identity.sources[0].lifecycle.completedSequence = 0u;
  incomplete.identity.attempts[0].outcome = UINT32_MAX;
  incomplete.topology.placements[0].outcome = UINT32_MAX;
  check(encodeRenderTapeIdentityV4SnapshotTransfer(incomplete.identity, incomplete.topology, bytes) == Status::Valid &&
            decodeRenderTapeIdentityV4SnapshotTransfer(bytes, kCapture, kQueue,
                decoded.identity, decoded.topology) == Status::Valid &&
            !decoded.identity.sources[0].lifecycle.completionEdge &&
            decoded.identity.attempts[0].outcome == UINT32_MAX &&
            decoded.topology.placements[0].outcome == UINT32_MAX,
        "structural copying does not invent completion or replace required final provenance/topology validation");
  auto empty = snapshot;
  empty.identity.sources.clear(); empty.identity.initializers.clear(); empty.identity.commands.clear();
  empty.identity.dispositions.clear(); empty.identity.coordinators.clear(); empty.identity.settlements.clear();
  empty.identity.attempts.clear(); empty.topology.plans.clear(); empty.topology.placements.clear();
  check(encodeRenderTapeIdentityV4SnapshotTransfer(empty.identity, empty.topology, bytes) == Status::Valid &&
            bytes.size() == sizeof(h) && decodeRenderTapeIdentityV4SnapshotTransfer(bytes,
                kCapture, kQueue, decoded.identity, decoded.topology) == Status::Valid &&
            decoded.identity.attempts.empty() && decoded.topology.placements.empty(),
        "zero-row snapshot remains structural evidence only with explicit capture join and typed descriptors");
}

void actionWitnessValidityAndRoundTrip() {
  const RenderTapeActionWitnessV1 unknown;
  check(renderTapeActionWitnessV1Valid(unknown) &&
      !renderTapeActionWitnessV1MatchesBinding(unknown, kCapture.token, kCapture.epoch, kQueue) &&
      !renderTapeActionWitnessV1StableExact(unknown),
      "canonical LegacyUnknown is valid absence without binding or stable-policy authority");
  for (std::uint32_t field = 0u; field != 9u; ++field) {
    auto invalid = unknown;
    if (field == 0u) invalid.constructionProfile = 1u;
    if (field == 1u) invalid.armProfile = 1u;
    if (field == 2u) invalid.freezeProfile = 1u;
    if (field == 3u) invalid.reserved = 1u;
    if (field == 4u) invalid.captureToken = kCapture.token;
    if (field == 5u) invalid.captureEpoch = kCapture.epoch;
    if (field == 6u) invalid.queue = kQueue;
    if (field == 7u) invalid.armRevision = 2u;
    if (field == 8u) invalid.freezeRevision = 2u;
    check(!renderTapeActionWitnessV1Valid(invalid) &&
        !renderTapeActionWitnessV1StableExact(invalid),
        "unknown evidence cannot carry a profile, revision or borrowed capture credentials");
  }
  const auto snapshot = snapshotFixture();
  for (std::uint32_t variant = 0u; variant != 7u; ++variant) {
    auto witness = knownWitness();
    if (variant == 1u)
      witness.constructionProfile = witness.armProfile = witness.freezeProfile = 0u;
    if (variant == 2u) witness.armRevision = witness.freezeRevision = 2u;
    if (variant == 3u) witness.freezeRevision = 2u;
    if (variant == 4u) { witness.freezeProfile = 0u; witness.freezeRevision = 1u; }
    if (variant == 5u) {
      witness.constructionProfile = 0u;
      witness.armRevision = witness.freezeRevision = 1u;
    }
    if (variant == 6u) {
      witness.constructionProfile = witness.armProfile = 0u;
      witness.freezeRevision = 1u;
    }
    check(renderTapeActionWitnessV1Valid(witness) &&
        renderTapeActionWitnessV1MatchesBinding(witness, kCapture.token, kCapture.epoch, kQueue) &&
        renderTapeActionWitnessV1StableExact(witness) == (variant == 0u),
        "shared predicate separates stable construction-through-freeze exact profile from normal/change/ABA history");
    std::vector<std::byte> bytes;
    check(encodeRenderTapeIdentityV4SnapshotTransfer(snapshot.identity, snapshot.topology,
        witness, bytes) == Status::Valid, "V3 export preserves valid action history without promoting stability");
    RenderTapeIdentityV4TransferHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    check(header.magic == kRenderTapeIdentityV4TransferMagic && header.version == 3u &&
        header.headerSize == 512u && header.identity.sources.offset == 512u &&
        header.actionWitness == witness,
        "V3 explicitly appends the 64-byte witness before its canonical tables");
    SnapshotPair decoded;
    RenderTapeActionWitnessV1 observed;
    check(decodeRenderTapeIdentityV4SnapshotTransfer(bytes, kCapture, kQueue,
        decoded.identity, decoded.topology, &observed) == Status::Valid && observed == witness &&
        sameRows(decoded.identity.attempts, snapshot.identity.attempts) &&
        sameRows(decoded.topology.placements, snapshot.topology.placements),
        "complete decode commits the independently bound witness alongside unchanged pair rows");
    check(decodeRenderTapeIdentityV4SnapshotTransfer(bytes, kCapture, kQueue,
        decoded.identity, decoded.topology) == Status::Valid,
        "callers omitting the witness output still validate its complete grammar and binding");
    check(!renderTapeActionWitnessV1MatchesBinding(witness, kCapture.token + 1u, kCapture.epoch, kQueue) &&
        !renderTapeActionWitnessV1MatchesBinding(witness, kCapture.token, kCapture.epoch + 1u, kQueue) &&
        !renderTapeActionWitnessV1MatchesBinding(witness, kCapture.token, kCapture.epoch, kQueue + 1u) &&
        !renderTapeActionWitnessV1MatchesBinding(witness, 0u, kCapture.epoch, kQueue),
        "an observed profile never authorizes a replacement or zero trusted binding");
  }

  const auto known = knownWitness();
  std::vector<std::byte> original;
  check(encodeRenderTapeIdentityV4SnapshotTransfer(snapshot.identity, snapshot.topology,
      known, original) == Status::Valid, "witness negatives begin with a bound stable observation");
  for (std::uint32_t mutation = 0u; mutation != 19u; ++mutation) {
    auto witness = known;
    auto expected = Status::InvalidHeader;
    if (mutation == 0u) witness.version = 0u;
    if (mutation == 1u) witness.evidence = 2u;
    if (mutation == 2u) witness.constructionProfile = 2u;
    if (mutation == 3u) witness.armProfile = 2u;
    if (mutation == 4u) witness.freezeProfile = 2u;
    if (mutation == 5u) witness.reserved = 1u;
    if (mutation == 6u) witness.captureToken = 0u;
    if (mutation == 7u) witness.captureEpoch = 0u;
    if (mutation == 8u) witness.queue = 0u;
    if (mutation == 9u) witness.armRevision = 2u;
    if (mutation == 10u) witness.freezeProfile = 0u;
    if (mutation == 11u) witness.constructionProfile = 0u;
    if (mutation == 12u) { ++witness.captureToken; expected = Status::CaptureMismatch; }
    if (mutation == 13u) { ++witness.captureEpoch; expected = Status::CaptureMismatch; }
    if (mutation == 14u) { ++witness.queue; expected = Status::CaptureMismatch; }
    if (mutation == 15u) witness.evidence = 0u;
    // Revision parity reflects real boolean transitions, not arbitrary labels.
    if (mutation == 16u) witness.armRevision = witness.freezeRevision = 1u;
    if (mutation == 17u) witness.freezeRevision = 1u;
    if (mutation == 18u) { witness.freezeProfile = 0u; witness.freezeRevision = 2u; }
    std::vector<std::byte> out{std::byte{1u}};
    check(encodeRenderTapeIdentityV4SnapshotTransfer(snapshot.identity, snapshot.topology,
        witness, out) == expected && out.empty(),
        "malformed/impossible/misbound witness export exposes no transfer");
    auto bytes = original;
    RenderTapeIdentityV4TransferHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    header.actionWitness = witness;
    std::memcpy(bytes.data(), &header, sizeof(header));
    rejectedDecode(bytes, expected);
    SnapshotPair decoded;
    check(decodeRenderTapeIdentityV4SnapshotTransfer(bytes, kCapture, kQueue,
        decoded.identity, decoded.topology) == expected && emptyPair(decoded),
        "omitting witness output never skips rejection of malformed or mismatched witness input");
  }
  for (std::size_t extent = 448u; extent < 512u; ++extent)
    rejectedDecode(std::span<const std::byte>(original).first(extent), Status::InvalidHeader);
}

void legacyVersionIsExplicitUnknown() {
  auto snapshot = snapshotFixture();
  for (const bool empty : {false, true}) {
    if (empty) {
      snapshot.identity.sources.clear(); snapshot.identity.initializers.clear();
      snapshot.identity.commands.clear(); snapshot.identity.dispositions.clear();
      snapshot.identity.coordinators.clear(); snapshot.identity.settlements.clear();
      snapshot.identity.attempts.clear(); snapshot.topology.plans.clear();
      snapshot.topology.placements.clear();
    }
    std::vector<std::byte> current;
    check(encodeRenderTapeIdentityV4SnapshotTransfer(snapshot.identity, snapshot.topology,
        knownWitness(), current) == Status::Valid, "legacy fixture starts with unchanged owned rows");
    RenderTapeIdentityV4TransferHeader header{};
    std::memcpy(&header, current.data(), sizeof(header));
    RenderTapeIdentityV4TransferLegacyHeader legacy;
    legacy.identity = header.identity;
    legacy.topology = header.topology;
    constexpr auto addedBytes = sizeof(header) - sizeof(legacy);
    legacy.totalBytes = current.size() - addedBytes;
    for (auto* table : tables(legacy)) table->offset -= addedBytes;
    std::vector<std::byte> bytes(static_cast<std::size_t>(legacy.totalBytes));
    std::memcpy(bytes.data(), &legacy, sizeof(legacy));
    if (current.size() > sizeof(header))
      std::memcpy(bytes.data() + sizeof(legacy), current.data() + sizeof(header),
          current.size() - sizeof(header));
    SnapshotPair decoded;
    auto witness = knownWitness();
    check(decodeRenderTapeIdentityV4SnapshotTransfer(bytes, kCapture, kQueue,
        decoded.identity, decoded.topology, &witness) == Status::Valid &&
        witness == RenderTapeActionWitnessV1{} && !renderTapeActionWitnessV1StableExact(witness) &&
        decoded.identity.header.sources.offset == 448u &&
        sameRows(decoded.identity.sources, snapshot.identity.sources) &&
        sameRows(decoded.identity.attempts, snapshot.identity.attempts) &&
        sameRows(decoded.topology.placements, snapshot.topology.placements),
        "explicit D9RTSNV2 retains 448-byte table origin and cannot infer action evidence from rows");
    for (std::uint32_t mutation = 0u; mutation != 6u; ++mutation) {
      auto invalid = bytes;
      auto broken = legacy;
      auto expected = Status::InvalidHeader;
      if (mutation == 0u) broken.headerSize = 512u;
      if (mutation == 1u) broken.magic = kRenderTapeIdentityV4TransferMagic;
      if (mutation == 2u) broken.version = kRenderTapeIdentityV4TransferVersion;
      if (mutation == 3u) { broken.identity.sources.offset = 512u; expected = Status::InvalidLayout; }
      if (mutation == 4u) broken.reserved0 = 1u;
      if (mutation == 5u) broken.reserved1 = 1u;
      std::memcpy(invalid.data(), &broken, sizeof(broken));
      rejectedDecode(invalid, expected);
    }
    rejectedDecode(std::span<const std::byte>(bytes).first(447u), Status::InvalidHeader);
  }
}

void malformedHeadersAndTrustedPair() {
  const auto snapshot = snapshotFixture();
  std::vector<std::byte> original;
  check(encodeRenderTapeIdentityV4SnapshotTransfer(snapshot.identity, snapshot.topology, original) == Status::Valid,
        "header negatives begin with a bounded paired transport");
  for (std::uint32_t mutation = 0u; mutation < 38u; ++mutation) {
    auto bytes = original;
    RenderTapeIdentityV4TransferHeader h{};
    std::memcpy(&h, bytes.data(), sizeof(h));
    auto expected = Status::InvalidHeader;
    switch (mutation) {
    case 0u: h.magic = kRenderTapeIdentityMagic; break;
    case 1u: h.magic = kRenderTapeExecutionTopologyV1Magic; break;
    case 2u: h.magic = kRenderTapeIdentityV3TransferMagic; break;
    case 3u: h.version = kRenderTapeIdentityV3TransferVersion; break;
    case 4u: --h.headerSize; break;
    case 5u: h.reserved0 = 1u; break;
    case 6u: h.reserved1 = 1u; break;
    case 7u: h.identity.magic = kRenderTapeExecutionTopologyV1Magic; break;
    case 8u: h.identity.version = kRenderTapeIdentityV3Version; break;
    case 9u: h.identity.headerSize = 240u; break;
    case 10u: h.identity.requiredExecutionTopologyVersion = 0u; break;
    case 11u: h.identity.requiredExecutionTopologyVersion = 2u; break;
    case 12u: h.identity.authority = static_cast<std::uint32_t>(RenderTapeIdentityAuthority::ProviderReplay); break;
    case 13u: h.identity.eventsBytes = 1u; break;
    case 14u: h.identity.eventsDigest[0] = std::byte{1}; break;
    case 15u: h.identity.reserved0 = 1u; break;
    case 16u: h.identity.reserved1 = 1u; break;
    case 17u: h.identity.reserved2 = 1u; break;
    case 18u: h.identity.reserved3 = 1u; break;
    case 19u: h.topology.magic = kRenderTapeIdentityMagic; break;
    case 20u: h.topology.version = 2u; break;
    case 21u: --h.topology.headerSize; break;
    case 22u: h.topology.identityBytes = 1u; break;
    case 23u: h.topology.eventsBytes = 1u; break;
    case 24u: h.topology.identityDigest[0] = std::byte{1}; break;
    case 25u: h.topology.eventsDigest[0] = std::byte{1}; break;
    case 26u: h.topology.reserved0 = 1u; break;
    case 27u: h.topology.reserved1 = 1u; break;
    case 28u: h.identity.captureToken = 0u; expected = Status::CaptureMismatch; break;
    case 29u: h.identity.captureEpoch = 0u; expected = Status::CaptureMismatch; break;
    case 30u: h.identity.queue = 0u; expected = Status::CaptureMismatch; break;
    case 31u: h.topology.captureToken = 0u; expected = Status::CaptureMismatch; break;
    case 32u: h.topology.captureEpoch = 0u; expected = Status::CaptureMismatch; break;
    case 33u: h.topology.queue = 0u; expected = Status::CaptureMismatch; break;
    case 34u: ++h.topology.captureToken; expected = Status::CaptureMismatch; break;
    case 35u: ++h.topology.captureEpoch; expected = Status::CaptureMismatch; break;
    case 36u: ++h.topology.queue; expected = Status::CaptureMismatch; break;
    case 37u: h.identity.authority = 0u; break;
    }
    std::memcpy(bytes.data(), &h, sizeof(h));
    rejectedDecode(bytes, expected);
    if (mutation >= 7u) {
      auto invalid = snapshot;
      invalid.identity.header = h.identity;
      invalid.topology.header = h.topology;
      std::vector<std::byte> out{std::byte{1}};
      check(encodeRenderTapeIdentityV4SnapshotTransfer(invalid.identity, invalid.topology, out) == expected && out.empty(),
            "export rejects malformed grammar, prebound authority and unmatched capture pairs before allocation");
    }
  }
  rejectedDecode(original, Status::CaptureMismatch, {kCapture.token + 1u, kCapture.epoch});
  rejectedDecode(original, Status::CaptureMismatch, {kCapture.token, kCapture.epoch + 1u});
  rejectedDecode(original, Status::CaptureMismatch, {0u, kCapture.epoch});
  rejectedDecode(original, Status::CaptureMismatch, {kCapture.token, 0u});
  rejectedDecode(original, Status::CaptureMismatch, kCapture, kQueue + 1u);
  rejectedDecode(original, Status::CaptureMismatch, kCapture, 0u);
  for (std::uint32_t key = 0u; key < 3u; ++key) {
    auto bytes = original;
    RenderTapeIdentityV4TransferHeader h{};
    std::memcpy(&h, bytes.data(), sizeof(h));
    if (key == 0u) { ++h.identity.captureToken; ++h.topology.captureToken; }
    if (key == 1u) { ++h.identity.captureEpoch; ++h.topology.captureEpoch; }
    if (key == 2u) { ++h.identity.queue; ++h.topology.queue; }
    std::memcpy(bytes.data(), &h, sizeof(h));
    rejectedDecode(bytes, Status::CaptureMismatch);
  }
}

void malformedCanonicalLayoutAndCapacity() {
  const auto snapshot = snapshotFixture();
  std::vector<std::byte> original;
  check(encodeRenderTapeIdentityV4SnapshotTransfer(snapshot.identity, snapshot.topology, original) == Status::Valid,
        "layout negatives begin with every table nonempty");
  for (std::size_t index = 0u; index < 9u; ++index) {
    for (std::uint32_t mutation = 0u; mutation < 8u; ++mutation) {
      auto bytes = original;
      RenderTapeIdentityV4TransferHeader h{};
      std::memcpy(&h, bytes.data(), sizeof(h));
      auto& table = *tables(h)[index];
      auto expected = Status::InvalidLayout;
      if (mutation == 0u) ++table.offset;
      if (mutation == 1u) --table.offset;
      if (mutation == 2u) table.offset = h.identity.sources.offset;
      if (mutation == 2u && index == 0u) table.offset = sizeof(h) - 1u;
      if (mutation == 3u) ++table.entrySize;
      if (mutation == 4u) table.count = 0u;
      if (mutation == 5u) table.offset = UINT32_MAX;
      if (mutation == 6u) { table.count = kRenderTapeIdentityV4MaxRows + 1u; expected = Status::CapacityExceeded; }
      if (mutation == 7u) { table.count = UINT32_MAX; expected = Status::CapacityExceeded; }
      std::memcpy(bytes.data(), &h, sizeof(h));
      rejectedDecode(bytes, expected);
    }
  }
  for (std::uint32_t mutation = 0u; mutation < 7u; ++mutation) {
    auto bytes = original;
    RenderTapeIdentityV4TransferHeader h{};
    std::memcpy(&h, bytes.data(), sizeof(h));
    auto expected = Status::InvalidLayout;
    if (mutation == 0u) ++h.totalBytes;
    if (mutation == 1u) bytes.pop_back();
    if (mutation == 2u) bytes.push_back(std::byte{});
    if (mutation == 3u) { bytes.pop_back(); h.totalBytes = bytes.size(); }
    if (mutation == 4u) { bytes.push_back(std::byte{}); h.totalBytes = bytes.size(); }
    if (mutation == 5u) { h.totalBytes = std::uint64_t{UINT32_MAX} + 1u; expected = Status::CapacityExceeded; }
    if (mutation == 6u) { h.totalBytes = UINT64_MAX; expected = Status::CapacityExceeded; }
    std::memcpy(bytes.data(), &h, sizeof(h));
    rejectedDecode(bytes, expected);
  }
  rejectedDecode(std::span<const std::byte>(original).first(sizeof(RenderTapeIdentityV4TransferHeader) - 1u),
                 Status::InvalidHeader);
  rejectedDecode({}, Status::InvalidHeader);
  auto excessive = snapshot;
  excessive.identity.settlements.resize(kRenderTapeIdentityV4MaxRows + 1u);
  std::vector<std::byte> bytes{std::byte{1}};
  check(encodeRenderTapeIdentityV4SnapshotTransfer(excessive.identity, excessive.topology, bytes) ==
            Status::CapacityExceeded && bytes.empty(),
        "provenance table owned count exceeding the schema bound rejects before transfer allocation");
  excessive = snapshot;
  excessive.topology.placements.resize(kRenderTapeIdentityV4MaxRows + 1u);
  bytes = {std::byte{1}};
  check(encodeRenderTapeIdentityV4SnapshotTransfer(excessive.identity, excessive.topology, bytes) ==
            Status::CapacityExceeded && bytes.empty(),
        "topology table owned count exceeding the shared schema bound publishes no transport");
}

void allocationFailureAndIndependentGrammars() {
  const auto snapshot = snapshotFixture();
  std::vector<std::byte> bytes{std::byte{1}};
  transfer_v4_allocation_probe::failAfter = 0u;
  transfer_v4_allocation_probe::enabled = true;
  const auto exportFailure = encodeRenderTapeIdentityV4SnapshotTransfer(snapshot.identity, snapshot.topology, bytes);
  transfer_v4_allocation_probe::enabled = false;
  check(exportFailure == Status::AllocationFailed && bytes.empty(), "failed allocation exposes no encoded pair");
  check(encodeRenderTapeIdentityV4SnapshotTransfer(snapshot.identity, snapshot.topology, bytes) == Status::Valid,
        "decode allocation negatives start with every owned table present");
  for (std::size_t allocation = 0u; allocation < 9u; ++allocation) {
    auto decoded = snapshot;
    auto witness = knownWitness();
    transfer_v4_allocation_probe::failAfter = allocation;
    transfer_v4_allocation_probe::enabled = true;
    const auto status = decodeRenderTapeIdentityV4SnapshotTransfer(bytes, kCapture, kQueue,
        decoded.identity, decoded.topology, &witness);
    transfer_v4_allocation_probe::enabled = false;
    check(status == Status::AllocationFailed && emptyPair(decoded) && witness == RenderTapeActionWitnessV1{},
          "failure in any provenance/topology table copy cannot commit a partial pair or action witness");
  }
  std::vector<std::byte> sidecar(sizeof(RenderTapeIdentityV4TransferHeader));
  RenderTapeIdentityV4Header identity{};
  std::memcpy(sidecar.data(), &identity, sizeof(identity));
  rejectedDecode(sidecar, Status::InvalidHeader);
  RenderTapeExecutionTopologyV1Header topology{};
  std::memcpy(sidecar.data(), &topology, sizeof(topology));
  rejectedDecode(sidecar, Status::InvalidHeader);
  RenderTapeIdentityV3TransferHeader oldTransfer{};
  std::memcpy(sidecar.data(), &oldTransfer, sizeof(oldTransfer));
  rejectedDecode(sidecar, Status::InvalidHeader);
  auto oldOut = RenderTapeIdentityV3View{};
  check(decodeRenderTapeIdentityV3SnapshotTransfer(bytes, kCapture, oldOut) ==
            RenderTapeIdentityV3TransferStatus::InvalidHeader && oldOut.sources.empty(),
        "paired transport cannot silently attach through the v3-only snapshot reader");
  for (const auto status : {Status::Valid, Status::InvalidHeader, Status::InvalidLayout,
           Status::CaptureMismatch, Status::CapacityExceeded, Status::AllocationFailed})
    check(std::string_view(renderTapeIdentityV4TransferStatusName(status)) != "unknown",
          "all bounded transport outcomes retain stable diagnostic text");
}

} // namespace

int main() {
  try {
    pairedRoundTripAndAuthoritySeparation();
    actionWitnessValidityAndRoundTrip();
    legacyVersionIsExplicitUnknown();
    malformedHeadersAndTrustedPair();
    malformedCanonicalLayoutAndCapacity();
    allocationFailureAndIndependentGrammars();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render tape v4 paired identity transfer spec failed: " << error.what() << '\n';
    return 1;
  }
}
