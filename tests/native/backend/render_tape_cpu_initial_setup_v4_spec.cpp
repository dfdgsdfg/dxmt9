#include "render_tape_cpu_initial_setup_v4.hpp"
#include "render_tape_cpu_destroy_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace dxmt9::d3d9;
using dxmt9::test::CpuInitialSetupV4;
using EventResult = dxmt9::test::CpuInitialSetupV4EventResult;
using Object = render_tape_runtime::OwnedObject;
std::size_t checks = 0u;

void check(bool value, std::string_view message) {
  ++checks;
  if (!value) throw std::runtime_error(std::string(message));
}

template <class T>
std::vector<std::byte> bytesOf(const T& value) {
  const auto bytes = std::as_bytes(std::span(&value, 1u));
  return {bytes.begin(), bytes.end()};
}

struct Fixture {
  std::vector<std::byte> tape{};
  std::vector<std::vector<std::byte>> blobs{};
  render_tape_runtime::GeneralPreflightPlan general{};
  CaptureSourceReplayPlanV4 source{};
  RenderTapeInitialSetupV4Plan setup{};
  std::uint32_t prefix = 0u;
};

std::vector<std::byte> textureDescriptor(RenderTapeTextureDimension dimension,
    std::uint32_t mips, bool large = false, bool allocation = false) {
  const auto count = mips * (dimension == RenderTapeTextureDimension::Cube ? 6u : 1u);
  const RenderTapeTextureDescriptorV2 fixed{.dimension = static_cast<std::uint32_t>(dimension),
      .mipLevelCount = mips, .subresourceCount = count,
      .initialContentDisposition = static_cast<std::uint32_t>(allocation
          ? RenderTapeInitialContentDisposition::NullRtDescriptorOnly
          : RenderTapeInitialContentDisposition::CompleteSeed)};
  auto bytes = bytesOf(fixed);
  for (std::uint32_t index = 0u; index < count; ++index) {
    const auto mip = renderTapeTextureDescriptorMipLevel(dimension, mips, index);
    const D9CSurfaceDesc surface{
        .format = allocation ? kRenderTapeNullRtD3DFormat : large ? 113u : 21u,
        .resourceType = dimension == RenderTapeTextureDimension::Texture2D ? 3u
            : dimension == RenderTapeTextureDimension::Cube ? 5u : 4u,
        .usage = allocation || large ? 1u : 0u,
        .width = large ? 1280u : 4u >> mip, .height = large ? 720u : 4u >> mip,
        .depth = dimension == RenderTapeTextureDimension::Volume ? 4u >> mip : 1u};
    const auto level = bytesOf(surface);
    bytes.insert(bytes.end(), level.begin(), level.end());
  }
  return bytes;
}

// Synthetic input provenance is authored and authenticated independently of
// the host callbacks below. The helper never creates candidate topology rows.
Fixture makeFixture(bool large = false, bool empty = false) {
  auto original = test::makeGeneralCpuDestroyFixture(false, false);
  ImportedRenderTapeView tape;
  check(importPrevalidatedRenderTape(original.tape, tape), "original fixture imports");
  Fixture fixture;
  fixture.blobs = std::move(original.blobs);
  RenderTapeBuilder builder;
  std::uint32_t inserted = 0u;
  bool firstCommandSeen = false, ownersAppended = false;
  const auto appendAlias = [&](D9CWireObjectIdentity parent,
      const std::vector<std::byte>& descriptor, std::uint32_t subresource) {
    D9CSurfaceDesc surface{};
    check(renderTapeTextureSubresourceDescriptor(descriptor, subresource, surface),
        "late alias has an exact parent subresource");
    surface.resourceType = 1u;
    const RenderTapeSurfaceDescriptorV2 alias{
        .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::TextureSubresource),
        .initialContentDisposition = static_cast<std::uint32_t>(RenderTapeInitialContentDisposition::Unavailable),
        .subresource = subresource, .parentTexture = parent, .surface = surface};
    builder.appendObjectDefine({D9C_CHUNK_HANDLE_KIND_SURFACE, 11u, parent.objectId + 100u},
        static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface), bytesOf(alias), 0u, {});
    ++inserted;
  };
  const auto appendOwner = [&](D9CWireObjectIdentity identity,
      RenderTapeDescriptorKind kind, const std::vector<std::byte>& descriptor,
      bool allocation = false) {
    auto contract = renderTapeDeriveExpectedContentContract(identity.kind, descriptor);
    check(allocation
        ? contract.status == RenderTapeExpectedContentStatus::NotRequired &&
            !contract.bytes && !contract.count &&
            renderTapeNullRtDescriptorOnly(identity.kind, descriptor)
        : contract.status == RenderTapeExpectedContentStatus::Accepted,
        "late owner has its exact seeded or allocation-only content contract");
    std::vector<std::uint64_t> extents(contract.count);
    renderTapeDeriveExpectedContentContract(identity.kind, descriptor, extents);
    builder.appendObjectDefine(identity, static_cast<std::uint32_t>(kind), descriptor,
        0u, {}, contract.bytes, contract.count);
    ++inserted;
    for (std::uint32_t index = 0u; index < contract.count; ++index) {
      std::vector<std::byte> bytes(static_cast<std::size_t>(extents[index]),
          static_cast<std::byte>((identity.objectId + index) & 0xffu));
      const auto digest = RenderTapeCaptureSession::sha256(bytes);
      if (std::none_of(fixture.blobs.begin(), fixture.blobs.end(),
              [&](const auto& prior) { return prior == bytes; }))
        fixture.blobs.push_back(std::move(bytes));
      builder.appendResourceMutation(identity, RenderTapeMutationKind::Upload,
          index, 0u, extents[index], digest);
      ++inserted;
    }
    if (kind == RenderTapeDescriptorKind::Texture &&
        (allocation || identity.objectId == 1000u || identity.objectId == 1001u))
      appendAlias(identity, descriptor, identity.objectId == 1001u ? 7u : 0u);
  };
  for (std::size_t index = 0u; index < tape.events.size(); ++index) {
    const auto event = tape.event(index);
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    // Draw one is the arm boundary; all owners below remain logically late.
    if (firstCommandSeen && !ownersAppended && !empty) {
      appendOwner({D9C_CHUNK_HANDLE_KIND_TEXTURE, 11u, 1000u}, RenderTapeDescriptorKind::Texture,
          textureDescriptor(RenderTapeTextureDimension::Texture2D, large ? 1u : 2u, large));
      appendOwner({D9C_CHUNK_HANDLE_KIND_TEXTURE, 11u, 1001u}, RenderTapeDescriptorKind::Texture,
          textureDescriptor(RenderTapeTextureDimension::Cube, 2u));
      appendOwner({D9C_CHUNK_HANDLE_KIND_TEXTURE, 11u, 1002u}, RenderTapeDescriptorKind::Texture,
          textureDescriptor(RenderTapeTextureDimension::Volume, 1u));
      appendOwner({D9C_CHUNK_HANDLE_KIND_BUFFER, 11u, 1003u}, RenderTapeDescriptorKind::Buffer,
          bytesOf(D9CBufferDesc{.size = 32u, .usage = 0x200u}));
      appendOwner({D9C_CHUNK_HANDLE_KIND_BUFFER, 11u, 1004u}, RenderTapeDescriptorKind::Buffer,
          bytesOf(D9CBufferDesc{.size = 16u, .format = 101u}));
      appendOwner({D9C_CHUNK_HANDLE_KIND_TEXTURE, 11u, 1005u}, RenderTapeDescriptorKind::Texture,
          textureDescriptor(RenderTapeTextureDimension::Texture2D, 1u, false, true), true);
      ownersAppended = true;
    }
    auto payload = std::vector<std::byte>(event.payload.begin(), event.payload.end());
    if (type == RenderTapeEventType::PresentComplete) {
      RenderTapePresentCompleteHeader completion{};
      std::memcpy(&completion, payload.data(), sizeof(completion));
      completion.presentOrdinal += inserted;
      std::memcpy(payload.data(), &completion, sizeof(completion));
    }
    builder.appendRawEvent(type, payload);
    if (type == RenderTapeEventType::CommandChunk) firstCommandSeen = true;
  }
  fixture.tape = builder.seal();
  std::vector<RenderTapeProviderBlob> blobs;
  for (const auto& bytes : fixture.blobs)
    blobs.push_back({RenderTapeCaptureSession::sha256(bytes), bytes});
  const auto preflight = render_tape_runtime::buildGeneralReplayPlan(fixture.tape, blobs, fixture.general);
  check(preflight.complete(),
      std::string("late setup fixture passes the trusted General grammar: ") +
          frameTapeReplayStatusName(preflight.status) + " event=" +
          std::to_string(preflight.failedEventIndex));
  RenderTapeIdentityV4View identity;
  identity.header.coordinatorDomain = static_cast<std::uint32_t>(TapeCoordinatorAuthority::OrdinarySerial);
  identity.header.frameId = 9u;
  identity.header.captureToken = 100u;
  identity.header.captureEpoch = 101u;
  identity.header.presentOrdinal = fixture.general.presentOrdinal;
  identity.header.queue = 102u;
  identity.header.initializerEpoch = 103u;
  identity.header.baselineCompletionEdge = 1u;
  identity.header.baselineKind = static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::EmptyBaseline);
  RenderTapeExecutionTopologyV1View topology;
  topology.header.captureToken = identity.header.captureToken;
  topology.header.captureEpoch = identity.header.captureEpoch;
  topology.header.queue = identity.header.queue;
  std::uint64_t edge = 2u, generation = 1u;
  for (const auto& event : fixture.general.events) {
    if (event.type != RenderTapeEventType::CommandChunk) continue;
    const auto ordinal = fixture.general.tape.event(event.eventIndex).header.ordinal;
    const auto raw = 500u + ordinal;
    const auto& admitted = fixture.general.commandChunks[event.index];
    ImportedChunkView chunk;
    check(importPrevalidatedCommandChunk(admitted.bytes, admitted.envelope, chunk) &&
        chunk.records.size() == 1u &&
        (chunk.records[0].type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE ||
            chunk.records[0].type == D9C_COMMAND_RECORD_PRESENT),
        "trusted fixture raws contain one indexed draw or Present");
    const bool draw = chunk.records[0].type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
    const auto begin = edge++, effect = edge++;
    const auto source = static_cast<std::uint32_t>(identity.sources.size());
    const auto command = static_cast<std::uint32_t>(identity.commands.size());
    const auto publication = edge++, acceptance = edge++, submission = edge++, completion = edge++;
    const auto thisGeneration = generation++;
    identity.sources.push_back({.lifecycle = {.sourceOrdinal = source + 1u, .seqId = source + 1u,
        .writerCaptureId = source + 1u, .writerGeneration = 2u, .storageGeneration = 3u,
        .publicationEdge = publication, .submissionEdge = submission, .completionEdge = completion,
        .completedSequence = source + 1u, .cutEventOrdinal = ordinal, .commandCount = 1u,
        .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterEvent)},
        .rawOrdinal = raw, .publicationBuildGeneration = thisGeneration, .publicationControlIndex = source});
    identity.commands.push_back({.sourceIndex = source, .physicalCommandIndex = 0u,
        .kind = static_cast<std::uint32_t>(draw ? RenderTapeIdentityV3CommandKind::DrawRun
            : RenderTapeIdentityV3CommandKind::Present), .drawCount = draw ? 1u : 0u});
    identity.dispositions.push_back({.eventOrdinal = ordinal, .recordCount = 1u,
        .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::GpuCommand),
        .commandIndex = command, .subdrawCount = draw ? 1u : 0u});
    topology.placements.push_back({.planIndex = source, .commandIndex = command,
        .outcome = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Outcome::ExecutedWithoutDag),
        .executionOrdinal = 0u});
    topology.plans.push_back({.planId = source + 11u, .acceptanceEdge = acceptance,
        .route = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Route::NaturalOrder),
        .firstPlacement = command, .placementCount = 1u, .executedCount = 1u});
    const RenderTapeIdentityV4ReplaySourceIdentity projection{
        identity.header.frameId, 600u + ordinal, 600u + ordinal, raw};
    identity.attempts.push_back({.eventOrdinal = ordinal, .rawOrdinal = raw,
        .attemptGeneration = thisGeneration, .beginEdge = begin, .terminalEdge = edge++, .effectEdge = effect,
        .identity = projection, .receipt = {.identity = projection, .queueSequence = raw,
            .kind = static_cast<std::uint32_t>(RenderTapeIdentityV4ReceiptKind::Compatibility), .commandCount = 1u},
        .recordCount = admitted.envelope.recordCount,
        .outcome = static_cast<std::uint32_t>(RenderTapeIdentityV4AttemptOutcome::Committed), .stagedCommandCount = 1u});
    identity.settlements.push_back({ordinal, raw, edge++, admitted.envelope.recordCount});
  }
  RenderTapeIdentityV4Status identityStatus{};
  const auto identityBytes = buildRenderTapeIdentityV4(fixture.tape,
      fixture.general.catalogue, identity, &identityStatus);
  check(identityStatus == RenderTapeIdentityV4Status::Valid, "reference identity authenticates unchanged raw effects");
  RenderTapeExecutionTopologyV1Status topologyStatus{};
  const auto topologyBytes = buildRenderTapeExecutionTopologyV1(fixture.tape,
      fixture.general.catalogue, identityBytes, topology, &topologyStatus);
  check(topologyStatus == RenderTapeExecutionTopologyV1Status::Valid &&
      buildCaptureSourceReplayPlanV4(fixture.tape, fixture.general.catalogue,
          identityBytes, topologyBytes, fixture.source) == CaptureSourceReplayPlanV4Status::Valid,
      "input V4 identity/topology pair authenticates before callbacks");
  check(renderTapeInitialSetupV4SnapshotPrefix(fixture.general, fixture.prefix) && fixture.prefix == 8u &&
      buildRenderTapeInitialSetupV4(fixture.source, fixture.general, fixture.tape, fixture.prefix,
          fixture.setup) == RenderTapeInitialSetupV4Status::Valid,
      "pure setup plan selects late closures without changing original cuts");
  return fixture;
}

struct HostResource {
  std::vector<std::vector<std::byte>> subresources{};
};

struct Host {
  std::size_t created = 0u, released = 0u, staged = 0u;
  std::size_t failCreate = static_cast<std::size_t>(-1);
  std::size_t failStage = static_cast<std::size_t>(-1);
  bool throwStage = false;

  static void release(void* context, Object& object) noexcept {
    auto& host = *static_cast<Host*>(context);
    if (!object.value) return;
    delete static_cast<HostResource*>(object.value);
    object.value = nullptr;
    ++host.released;
  }

  auto sink() { return dxmt9::test::CpuInitialSetupV4ReleaseSink{this, release}; }

  bool prepare(CpuInitialSetupV4& helper, const Fixture& fixture) {
    return helper.prepare([&](const auto& entry, std::span<const Object>) -> void* {
      if (created == failCreate) return nullptr;
      auto resource = std::make_unique<HostResource>();
      resource->subresources.resize(entry.seedCount);
      ++created;
      return resource.release();
    }, [&](const auto&, const auto& seed, const Object& object) {
      if (staged == failStage) {
        if (throwStage) throw std::runtime_error("injected host seed failure");
        return false;
      }
      const auto blob = std::find_if(fixture.blobs.begin(), fixture.blobs.end(), [&](const auto& bytes) {
        return RenderTapeCaptureSession::sha256(bytes) == seed.mutation.digest;
      });
      if (blob == fixture.blobs.end() || blob->size() != seed.mutation.byteSize) return false;
      static_cast<HostResource*>(object.value)->subresources[seed.mutation.subresource] = *blob;
      ++staged;
      return true;
    });
  }

  void releaseActive(std::vector<Object>& objects) noexcept {
    for (auto& object : objects) release(this, object);
  }
};

void consumeAll(CpuInitialSetupV4& helper, const Fixture& fixture, std::vector<Object>& active) {
  for (const auto& event : fixture.general.events) {
    if (event.type == RenderTapeEventType::ObjectDefine) {
      const auto& definition = fixture.general.definitions[event.index];
      check(helper.activate(event.index, event.eventIndex, definition, active) != EventResult::Rejected,
          "each hidden owner activates at its original definition only");
      check(helper.qualifyAlias(event.index, event.eventIndex, definition, active) != EventResult::Rejected,
          "late alias requires its exact active parent generation");
    } else if (event.type == RenderTapeEventType::ResourceMutation) {
      const auto expected = event.eventIndex < fixture.prefix ? EventResult::NotManaged : EventResult::Accepted;
      check(helper.consumeSeed(event.index, event.eventIndex,
          fixture.general.mutations[event.index], active) == expected,
          "prefix Upload stays unmanaged; late seed consumes without running host stage twice");
    }
  }
}

void ownershipAndFamilies() {
  auto fixture = makeFixture();
  Host host;
  std::vector<Object> active;
  {
    CpuInitialSetupV4 helper(host.sink());
    check(helper.adopt(std::move(fixture.setup), fixture.source.digest()) && !fixture.setup.valid(),
        "adoption moves authenticated metadata rather than borrowing descriptors");
    check(helper.evidence().owners == 6u && helper.evidence().seeds == 17u &&
        helper.evidence().aliases == 3u && helper.evidence().seedBytes == 864u &&
        helper.evidence().allocationBytes == 64u, "five seeded families and NULLRT budgets remain separate");
    check(host.prepare(helper, fixture) && host.created == 6u && host.staged == 17u,
        "all canonical subresources stage once; AllocationOnly stages none");
    check(!helper.intervalComplete() && helper.joinHostCheckpoint([] { return true; }),
        "host setup join alone does not activate original transcript owners");
    consumeAll(helper, fixture, active);
    check(helper.intervalComplete() && helper.evidence().activated == 6u &&
        helper.evidence().consumedSeeds == 17u && host.staged == 17u,
        "all original logical seed events consume exactly once without extra transfers");
    check(helper.activate(999u, 999u, {}, active) == EventResult::NotManaged &&
        helper.consumeSeed(999u, 999u, {}, active) == EventResult::NotManaged,
        "interval writes and late completed EVENT baselines stay outside resource setup");
  }
  check(host.released == 0u, "hidden cleanup cannot release transferred active owners");
  host.releaseActive(active);
  check(host.released == host.created, "active cleanup retires every transferred acquisition once");

  auto empty = makeFixture(false, true);
  CpuInitialSetupV4 noOwners(host.sink());
  check(noOwners.adopt(std::move(empty.setup), empty.source.digest()) &&
      host.prepare(noOwners, empty) && noOwners.joinHostCheckpoint([] { return true; }) &&
      noOwners.intervalComplete(), "empty setup still requires an explicit host checkpoint join");
}

void boundsAndFailureCleanup() {
  for (std::uint32_t fault = 0u; fault < 4u; ++fault) {
    auto fixture = makeFixture();
    Host host;
    {
      CpuInitialSetupV4 helper(host.sink());
      auto digest = fixture.source.digest();
      digest[0] ^= std::byte{1};
      check(!helper.adopt(std::move(fixture.setup), digest) && fixture.setup.valid() && !host.created,
          "wrong source digest rejects transactionally before wrapper effects");
      auto limits = RenderTapeInitialSetupV4Limits{};
      limits.maxSeedBytes = 863u;
      check(!helper.adopt(std::move(fixture.setup), fixture.source.digest(), limits) && fixture.setup.valid(),
          "one-over aggregate host seed bytes cannot borrow allocation budget");
      check(helper.adopt(std::move(fixture.setup), fixture.source.digest()), "valid plan adopts after rejected admission");
      if (fault == 0u) host.failCreate = 2u;
      else if (fault < 3u) { host.failStage = 3u; host.throwStage = fault == 2u; }
      const auto prepared = host.prepare(helper, fixture);
      check(fault == 3u ? prepared && !helper.joinHostCheckpoint([] { return false; }) : !prepared,
          "creation/staging/exception/checkpoint failures reject their exact phase");
      check(helper.failed() && !host.prepare(helper, fixture), "failed preparation cannot retry or activate");
      helper.releaseHidden();
      check(host.released == host.created, "partial hidden setup releases all acquired wrappers");
      helper.releaseHidden();
      check(host.released == host.created, "repeated explicit cleanup is idempotent");
    }
    check(host.released == host.created, "destructor after explicit cleanup cannot double release");
  }
}

void eventNegatives() {
  for (std::uint32_t fault = 0u; fault < 9u; ++fault) {
    auto fixture = makeFixture();
    Host host;
    std::vector<Object> active;
    {
      CpuInitialSetupV4 helper(host.sink());
      check(helper.adopt(std::move(fixture.setup), fixture.source.digest()) && host.prepare(helper, fixture),
          "negative event case owns prepared hidden wrappers");
      const auto entry = helper.plan().entries()[0];
      auto definition = fixture.general.definitions[entry.definitionIndex];
      if (fault != 0u) check(helper.joinHostCheckpoint([] { return true; }), "negative case joins setup before original events");
      auto event = entry.definitionEventIndex;
      if (fault == 1u) ++event;
      if (fault == 2u) ++definition.fixed.identity.generation;
      if (fault <= 2u) {
        check(helper.activate(entry.definitionIndex, event, definition, active) == EventResult::Rejected,
            "activation rejects missing checkpoint, wrong original event and stale identity");
      } else if (fault == 3u) {
        const auto& seed = helper.plan().seeds()[0];
        check(helper.consumeSeed(seed.mutationIndex, seed.eventIndex, seed.mutation, active) == EventResult::Rejected,
            "seed consumption cannot precede original owner activation");
      } else {
        check(helper.activate(entry.definitionIndex, event, definition, active) == EventResult::Accepted,
            "exact owner activation transfers one acquisition");
        if (fault == 4u) {
          check(helper.activate(entry.definitionIndex, event, definition, active) == EventResult::Rejected,
              "duplicate activation cannot reacquire or transfer a second reference");
        } else {
          const auto& seed = helper.plan().seeds()[0];
          auto mutation = seed.mutation;
          auto seedEvent = seed.eventIndex;
          if (fault == 5u) ++seedEvent;
          if (fault == 6u) mutation.digest[0] ^= std::byte{1};
          if (fault == 7u) ++active[0].identity.generation;
          if (fault == 8u)
            check(helper.consumeSeed(seed.mutationIndex, seedEvent, mutation, active) == EventResult::Accepted,
                "first exact seed consumption succeeds");
          check(helper.consumeSeed(seed.mutationIndex, seedEvent, mutation, active) == EventResult::Rejected,
              "wrong seed event/digest/active generation and duplicate consumption fail closed");
        }
      }
      check(helper.failed() && !helper.intervalComplete(), "event mismatch prevents completion evidence");
    }
    host.releaseActive(active);
    check(host.released == host.created, "negative event cleanup conserves hidden and active ownership");
  }
}

void aliasNegativesAndArmPayload() {
  for (std::uint32_t fault = 0u; fault < 3u; ++fault) {
    auto fixture = makeFixture();
    Host host;
    std::vector<Object> active;
    {
      CpuInitialSetupV4 helper(host.sink());
      check(helper.adopt(std::move(fixture.setup), fixture.source.digest()) && host.prepare(helper, fixture) &&
          helper.joinHostCheckpoint([] { return true; }), "alias case joins bounded hidden setup");
      const auto& entry = helper.plan().entries()[0];
      const auto& alias = helper.plan().aliases()[0];
      if (fault != 0u)
        check(helper.activate(entry.definitionIndex, entry.definitionEventIndex,
            fixture.general.definitions[entry.definitionIndex], active) == EventResult::Accepted,
            "alias case activates exact parent");
      if (fault == 1u) ++active[0].identity.generation;
      const auto& definition = fixture.general.definitions[alias.definitionIndex];
      if (fault == 2u)
        check(helper.qualifyAlias(alias.definitionIndex, alias.eventIndex, definition, active) == EventResult::Accepted,
            "first alias qualification uses active parent and original subresource");
      check(helper.qualifyAlias(alias.definitionIndex, alias.eventIndex, definition, active) == EventResult::Rejected,
          "missing/stale parent and duplicate alias cannot forge activation");
    }
    host.releaseActive(active);
    check(host.created == host.released, "alias failure cleanup conserves all acquired references");
  }

  auto fixture = makeFixture(true);
  Host host;
  std::vector<Object> active;
  {
    CpuInitialSetupV4 helper(host.sink());
    check(helper.adopt(std::move(fixture.setup), fixture.source.digest()) && host.prepare(helper, fixture) &&
        helper.joinHostCheckpoint([] { return true; }), "capture-sized half-float host seed stages through bounded callback");
    consumeAll(helper, fixture, active);
    auto& bytes = static_cast<HostResource*>(active[0].value)->subresources[0];
    check(bytes.size() == 1280u * 720u * 8u && helper.intervalComplete(),
        "logical arm B supports actual capture29 extent rather than tiny observation cap");
    auto canonicalAPlusPatch = std::vector<std::byte>(bytes.size(), std::byte{0x37});
    canonicalAPlusPatch[17] = std::byte{0x91};
    auto mutation = helper.plan().seeds()[0].mutation;
    mutation.kind = static_cast<std::uint32_t>(RenderTapeMutationKind::CpuUnlock);
    mutation.digest = RenderTapeCaptureSession::sha256(canonicalAPlusPatch);
    check(helper.consumeSeed(999u, 999u, mutation, active) == EventResult::NotManaged,
        "later A-plus-patch mutation is not consumed as original arm B seed");
    bytes = canonicalAPlusPatch;
    check(bytes[16] == std::byte{0x37} && bytes[17] == std::byte{0x91} && host.staged == 16u,
        "whole authenticated later payload replaces host staging; B is never a partial-update base");
  }
  host.releaseActive(active);
  check(host.created == host.released, "capture-sized host receipt ownership retires exactly once");
}

} // namespace

int main() {
  try {
    ownershipAndFamilies();
    boundsAndFailureCleanup();
    eventNegatives();
    aliasNegativesAndArmPayload();
    std::cout << "cpu initial setup v4 checks: " << checks << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "cpu initial setup v4 failure: " << error.what() << '\n';
    return 1;
  }
}
