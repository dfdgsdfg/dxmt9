#include "render_tape_cpu_v4_admission.hpp"

#include "render_tape_completion_fixture.hpp"
#include "render_tape_cpu_policy.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {

using namespace dxmt9::d3d9;
using namespace dxmt9::d3d9::render_tape_runtime;
namespace cpu = dxmt9::test;
namespace fixture = dxmt9::d3d9::test;

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

static_assert(GeneralCpuAdmissionCaps{}.maxRecordsPerChunk == 64u);
static_assert(generalCpuExtendedInputCaps().maxRecordsPerChunk == 64u);
static_assert(cpu::kCpuTapeMaxRecordsPerRaw == 64u && cpu::kCpuTapeMaxPendingPerRaw == 31u);
static_assert(cpu::cpuTapeCaptureSourcePolicyBounded(
    {.compatibility = true, .drawCommandLimit = 3u}));
static_assert(cpu::cpuTapeConservativeMaxPendingPerRaw(256u, 3u) == 0u);
static_assert(!std::is_convertible_v<cpu::ControlledCpuV4InputAdmission,
                                  GeneralCpuAdmissionResult>);
static_assert(!std::is_default_constructible_v<cpu::ControlledCpuV4InputAdmission>);

std::vector<std::byte> recordChunk(std::size_t count, bool terminal) {
  check(count != 0u && count <= 257u, "test raw count is bounded");
  const auto state = fixture::fullStateChunk(false);
  const auto draw = fixture::fullStateChunk(true);
  const auto present = fixture::presentChunk();
  const std::array<std::span<const std::byte>, 3u> sourceBytes{state, draw, present};
  std::array<ImportedChunkView, 3u> sources;
  for (std::size_t index = 0u; index < sources.size(); ++index)
    check(importPrevalidatedCommandChunk(sourceBytes[index], {.recordCount = 1u}, sources[index]),
          "canonical source record imports");
  const auto recordOffset = sizeof(D9CCommandChunkWireHeader);
  const auto arenaOffset = fixture::alignUp(
      recordOffset + count * sizeof(D9CCommandChunkWireRecordHeader), alignof(std::uint32_t));
  std::vector<std::byte> bytes(arenaOffset);
  for (std::size_t index = 0u; index < count; ++index) {
    const auto& source = sources[terminal ? (index + 1u == count ? 2u : 1u) : 0u];
    auto record = source.records.front();
    const auto payload = source.record(0u).payload;
    record.payloadOffset = static_cast<std::uint32_t>(bytes.size() - arenaOffset);
    std::memcpy(bytes.data() + recordOffset + index * sizeof(record), &record, sizeof(record));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
  }
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordOffset),
      .recordCount = static_cast<std::uint32_t>(count),
      .handleTableOffset = static_cast<std::uint32_t>(arenaOffset),
      .payloadArenaOffset = static_cast<std::uint32_t>(arenaOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(bytes.size() - arenaOffset)};
  std::memcpy(bytes.data(), &header, sizeof(header));
  return bytes;
}

// Synthetic already-preflighted metadata exercises only the shared narrowing
// grammar. It is not original-bundle authentication or execution evidence.
struct MiniPlan {
  GeneralPreflightPlan plan{};
  std::vector<std::byte> bootstrapBytes;
  std::vector<std::byte> bodyBytes;
  RenderTapeSurfaceDescriptorV2 outputDescriptor{};

  explicit MiniPlan(std::size_t bodyRecords = 1u, std::size_t bootstrapRecords = 1u)
      : bootstrapBytes(recordChunk(bootstrapRecords, false)),
        bodyBytes(recordChunk(bodyRecords, true)) {
    const D9CWireObjectIdentity output{
        .kind = D9C_CHUNK_HANDLE_KIND_SURFACE, .generation = 1u, .objectId = 1u};
    outputDescriptor = {
        .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
        .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::SwapchainBackbuffer),
        .initialContentDisposition = static_cast<std::uint32_t>(
            RenderTapeInitialContentDisposition::ProducedPresentOutput),
        .surface = {.format = 21u, .resourceType = 1u, .usage = 1u,
                    .width = 4u, .height = 4u, .depth = 1u}};
    plan.outputIdentity = output;
    plan.outputDesc = outputDescriptor.surface;
    plan.bootstrapGamma.emplace_back();
    plan.bootstrapChunks.push_back({
        .envelope = {.recordCount = static_cast<std::uint32_t>(bootstrapRecords)},
        .bytes = bootstrapBytes});
    plan.commandChunks.push_back({
        .envelope = {.recordCount = static_cast<std::uint32_t>(bodyRecords)},
        .bytes = bodyBytes});
    plan.definitions.push_back({
        .fixed = {.identity = output,
                  .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
                  .descriptorBytes = sizeof(outputDescriptor)},
        .descriptor = std::as_bytes(std::span(&outputDescriptor, 1u))});
    plan.events = {
        {.type = RenderTapeEventType::BootstrapState, .eventIndex = 0u, .count = 1u},
        {.type = RenderTapeEventType::ObjectDefine, .eventIndex = 1u},
        {.type = RenderTapeEventType::CommandChunk, .eventIndex = 2u},
        {.type = RenderTapeEventType::PresentComplete, .eventIndex = 3u}};
  }
};

void legacyAndV4RawBoundaries() {
  {
    const MiniPlan input(64u);
    const auto legacy = admitGeneralCpuPlan(input.plan);
    const auto extended = admitGeneralCpuPlan(input.plan, generalCpuExtendedInputCaps());
    const auto v4 = cpu::admitControlledCpuV4Input(input.plan);
    check(legacy.accepted() && extended.accepted() && v4.accepted() &&
              v4.counts() == legacy.counts && extended.counts == legacy.counts,
          "raw64 preserves both legacy profiles and canonical counts");
  }
  for (std::size_t records = 65u; records <= 256u; ++records) {
    const MiniPlan input(records);
    const auto legacy = admitGeneralCpuPlan(input.plan);
    const auto extended = admitGeneralCpuPlan(input.plan, generalCpuExtendedInputCaps());
    const auto v4 = cpu::admitControlledCpuV4Input(input.plan);
    check(legacy.reason == GeneralCpuAdmissionReason::PerChunkRecordLimit &&
              extended.reason == GeneralCpuAdmissionReason::PerChunkRecordLimit && v4.accepted(),
          "each raw65 through raw256 rejects legacy64 and admits only typed V4 input");
    check(v4.counts().records == records && v4.counts().draws == records - 1u &&
              v4.counts().chunks == 1u && v4.counts().bootstrapRecords == 1u &&
              v4.counts().presents == 1u && v4.inputCaps().maxRecordsPerChunk == 256u &&
              v4.sourceProgressRequirement() ==
                  cpu::ControlledCpuV4SourceProgressRequirement::RequiredUnproven,
          "accepted raw256 input reports exact counts but cannot grant source progress");
  }
  const MiniPlan over(257u);
  const auto v4 = cpu::admitControlledCpuV4Input(over.plan);
  check(v4.reason() == GeneralCpuAdmissionReason::PerChunkRecordLimit &&
            v4.eventIndex() == 2u && v4.chunkIndex() == 0u &&
            admitGeneralCpuPlan(over.plan).reason == GeneralCpuAdmissionReason::PerChunkRecordLimit,
        "raw257 fails closed with its original locator");
}

void legacyCallerCapsAndV4BootstrapStaySeparate() {
  auto caps = generalCpuExtendedInputCaps();
  caps.maxRecordsPerChunk = 256u;
  const MiniPlan customBody(256u);
  check(admitGeneralCpuPlan(customBody.plan, caps).accepted(),
        "legacy caller-supplied per-raw caps retain their previous input behavior");
  const MiniPlan bootstrap64(1u, 64u);
  const auto v4 = cpu::admitControlledCpuV4Input(bootstrap64.plan);
  check(v4.accepted() && v4.counts().bootstrapRecords == 64u &&
            v4.bootstrapRecordsPerChunkLimit() == 64u,
        "typed V4 retains the state-only bootstrap64 boundary");
  const MiniPlan bootstrap65(1u, 65u);
  caps.maxRecordsPerChunk = 65u;
  check(admitGeneralCpuPlan(bootstrap65.plan, caps).accepted() &&
            cpu::admitControlledCpuV4Input(bootstrap65.plan).reason() ==
                GeneralCpuAdmissionReason::PerChunkRecordLimit,
        "V4 bootstrap64 does not newly restrict legacy caller-supplied caps");
  MiniPlan drawBootstrap;
  drawBootstrap.bootstrapBytes = fixture::fullStateChunk(true);
  drawBootstrap.plan.bootstrapChunks[0].bytes = drawBootstrap.bootstrapBytes;
  check(cpu::admitControlledCpuV4Input(drawBootstrap.plan).reason() ==
            GeneralCpuAdmissionReason::UnsupportedCommandRecord &&
            admitGeneralCpuPlan(drawBootstrap.plan).reason ==
                GeneralCpuAdmissionReason::UnsupportedCommandRecord,
        "neither policy permits publishing draw work in a bootstrap overlay");
}

void completeGammaIsPreservedAndMalformedGammaRejects() {
  MiniPlan input;
  auto& gamma = input.plan.bootstrapGamma[0];
  gamma.resize(kRenderTapeGammaRampBytes);
  for (std::size_t index = 0u; index < 768u; ++index) {
    const auto value = static_cast<std::uint16_t>((index % 256u) * 257u);
    std::memcpy(gamma.data() + index * sizeof(value), &value, sizeof(value));
  }
  const auto originalGamma = gamma;
  check(cpu::admitControlledCpuV4Input(input.plan).accepted() &&
            admitGeneralCpuPlan(input.plan).reason == GeneralCpuAdmissionReason::GammaUnsupported &&
            gamma == originalGamma,
        "complete i*257 gamma admits V4 without changing original non-identity bytes");
  for (std::size_t index = 0u; index < gamma.size(); ++index)
    gamma[index] = static_cast<std::byte>((index * 37u + 113u) & 255u);
  check(cpu::admitControlledCpuV4Input(input.plan).accepted() &&
            admitGeneralCpuPlan(input.plan).reason == GeneralCpuAdmissionReason::GammaUnsupported,
        "V4 accepts arbitrary authenticated complete ramp values, not one rewritten identity");
  for (std::size_t index = 0u; index < 768u; ++index) {
    const auto value = static_cast<std::uint16_t>((index % 256u) << 8u);
    std::memcpy(gamma.data() + index * sizeof(value), &value, sizeof(value));
  }
  check(cpu::admitControlledCpuV4Input(input.plan).accepted() && admitGeneralCpuPlan(input.plan).accepted(),
        "legacy identity gamma remains accepted by both policies");
  for (const auto bytes : {1u, kRenderTapeGammaRampBytes - 1u, kRenderTapeGammaRampBytes + 1u}) {
    gamma.resize(bytes);
    check(cpu::admitControlledCpuV4Input(input.plan).reason() ==
              GeneralCpuAdmissionReason::GammaUnsupported &&
              admitGeneralCpuPlan(input.plan).reason == GeneralCpuAdmissionReason::GammaUnsupported,
          "truncated, odd, and oversized gamma reject before effects under both policies");
  }
  gamma.clear();
  check(cpu::admitControlledCpuV4Input(input.plan).accepted() && admitGeneralCpuPlan(input.plan).accepted(),
        "an omitted gamma ramp retains its prior state-only meaning");
}

void originalBytesAndCountsStayUnchanged() {
  const MiniPlan input(256u, 6u);
  const auto body = input.bodyBytes;
  const auto bootstrap = input.bootstrapBytes;
  const auto events = input.plan.events;
  const auto bodyEnvelope = input.plan.commandChunks[0].envelope;
  auto caps = generalCpuExtendedInputCaps();
  caps.maxRecordsPerChunk = 256u;
  const auto reference = admitGeneralCpuPlan(input.plan, caps);
  const auto v4 = cpu::admitControlledCpuV4Input(input.plan);
  const auto repeated = cpu::admitControlledCpuV4Input(input.plan);
  check(reference.accepted() && v4.accepted() && v4.counts() == reference.counts &&
            repeated.counts() == v4.counts() && input.bodyBytes == body &&
            input.bootstrapBytes == bootstrap && input.plan.events.size() == events.size() &&
            input.plan.commandChunks[0].bytes.data() == input.bodyBytes.data() &&
            input.plan.commandChunks[0].bytes.size() == body.size() &&
            input.plan.commandChunks[0].envelope.version == bodyEnvelope.version &&
            input.plan.commandChunks[0].envelope.recordCount == bodyEnvelope.recordCount &&
            input.plan.commandChunks[0].envelope.handleCount == bodyEnvelope.handleCount,
        "typed admission neither repartitions original raw bytes nor changes canonical counts");
  for (std::size_t index = 0u; index < events.size(); ++index) {
    const auto& current = input.plan.events[index];
    const auto& original = events[index];
    check(current.type == original.type && current.eventIndex == original.eventIndex &&
              current.index == original.index && current.count == original.count && current.aux == original.aux,
          "typed admission leaves every original event reference unchanged");
  }
}

void rejectBoth(const MiniPlan& input, GeneralCpuAdmissionReason reason) {
  check(admitGeneralCpuPlan(input.plan).reason == reason &&
            cpu::admitControlledCpuV4Input(input.plan).reason() == reason,
        "shared immutable general narrowing fails closed under both policies");
}

void missingAndMalformedGeneralInputStillRejects() {
  {
    MiniPlan input;
    input.plan.commandChunks[0].bytes = {};
    rejectBoth(input, GeneralCpuAdmissionReason::CommandChunkUnavailable);
  }
  {
    MiniPlan input;
    input.bodyBytes.pop_back();
    input.plan.commandChunks[0].bytes = input.bodyBytes;
    rejectBoth(input, GeneralCpuAdmissionReason::CommandChunkUnavailable);
  }
  {
    MiniPlan input;
    input.plan.commandChunks[0].envelope.recordCount = 2u;
    rejectBoth(input, GeneralCpuAdmissionReason::CommandChunkUnavailable);
  }
  {
    MiniPlan input;
    input.plan.events[2].index = 99u;
    rejectBoth(input, GeneralCpuAdmissionReason::CommandChunkUnavailable);
  }
  {
    MiniPlan input;
    input.plan.events[1].eventIndex = 99u;
    rejectBoth(input, GeneralCpuAdmissionReason::InvalidEventReference);
  }
  {
    MiniPlan input;
    input.plan.controls.push_back({});
    rejectBoth(input, GeneralCpuAdmissionReason::OrderedControlUnsupported);
  }
  {
    MiniPlan input;
    input.plan.outputDesc.width = 0u;
    rejectBoth(input, GeneralCpuAdmissionReason::OutputDescriptorMissing);
  }
  {
    MiniPlan input;
    const D9CCommandChunkWireReadback readback{};
    input.bodyBytes = fixture::singleRecordChunk(D9C_COMMAND_RECORD_READBACK,
        std::as_bytes(std::span(&readback, 1u)));
    input.plan.commandChunks[0].bytes = input.bodyBytes;
    rejectBoth(input, GeneralCpuAdmissionReason::UnsupportedCommandRecord);
  }
  const MiniPlan input;
  const auto invalid = cpu::admitControlledCpuV4Input(
      input.plan, static_cast<cpu::ControlledCpuV4InputProfile>(255u));
  check(!invalid.accepted() && invalid.reason() == GeneralCpuAdmissionReason::InvalidBudget &&
            invalid.sourceProgressRequirement() ==
                cpu::ControlledCpuV4SourceProgressRequirement::RequiredUnproven,
        "unknown typed profiles reject without authenticating input or granting progress");
}

} // namespace

int main() {
  try {
    legacyAndV4RawBoundaries();
    legacyCallerCapsAndV4BootstrapStaySeparate();
    completeGammaIsPreservedAndMalformedGammaRejects();
    originalBytesAndCountsStayUnchanged();
    missingAndMalformedGeneralInputStillRejects();
    std::cout << "controlled CPU V4 input admission spec passed\n";
  } catch (const std::exception& error) {
    std::cerr << "controlled CPU V4 input admission spec failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
