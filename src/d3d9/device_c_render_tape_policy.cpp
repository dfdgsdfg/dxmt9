#include "device_c_render_tape_policy.hpp"

#include "device_c_chunk_validate.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace dxmt9::d3d9 {
namespace {

constexpr std::array<std::uint32_t, 4u> kChildCounts{2u, 4u, 8u, 16u};

template <typename T>
bool load(std::span<const std::byte> bytes, std::size_t offset,
          T& value) noexcept {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return true;
}

bool isDraw(std::uint32_t type) noexcept {
  switch (type) {
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
    return true;
  default:
    return false;
  }
}

bool isCoordinator(std::uint32_t type) noexcept {
  return type == D9C_COMMAND_RECORD_CLEAR ||
         type == D9C_COMMAND_RECORD_PRESENT;
}

bool checkedAdd(std::uint32_t left, std::uint32_t right,
                std::uint32_t& out) noexcept {
  if (right > std::numeric_limits<std::uint32_t>::max() - left) return false;
  out = left + right;
  return true;
}

bool countFacts(const ImportedRecordView& record,
                RenderTapePolicyCandidate& out) noexcept {
  for (std::size_t index = 0u; index < record.sections.size(); ++index) {
    const auto section = record.section(index);
    switch (section.descriptor.kind) {
    case D9C_COMMAND_CHUNK_SECTION_SHADER: {
      D9CCommandChunkWireShaderBinding shader{};
      if (!load(section.payload, 0u, shader)) break;
      if (shader.stage == D9C_COMMAND_CHUNK_SHADER_STAGE_VERTEX) {
        if (!checkedAdd(out.vertexShaderFacts, 1u, out.vertexShaderFacts)) {
          return false;
        }
      } else if (shader.stage == D9C_COMMAND_CHUNK_SHADER_STAGE_PIXEL) {
        if (!checkedAdd(out.pixelShaderFacts, 1u, out.pixelShaderFacts)) {
          return false;
        }
      }
      break;
    }
    case D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT:
      if (!checkedAdd(out.vertexInputFacts, 1u, out.vertexInputFacts)) {
        return false;
      }
      break;
    case D9C_COMMAND_CHUNK_SECTION_VS_CONST_F:
    case D9C_COMMAND_CHUNK_SECTION_VS_CONST_I:
    case D9C_COMMAND_CHUNK_SECTION_VS_CONST_B:
    case D9C_COMMAND_CHUNK_SECTION_PS_CONST_F:
    case D9C_COMMAND_CHUNK_SECTION_PS_CONST_I:
    case D9C_COMMAND_CHUNK_SECTION_PS_CONST_B:
      if (!checkedAdd(out.uniformSectionFacts, 1u,
                      out.uniformSectionFacts)) {
        return false;
      }
      break;
    default:
      break;
    }
  }
  return true;
}

bool makeCandidate(const RenderTapeIdentitySource& source,
                   const RenderTapeIdentityRange& range,
                   const ImportedChunkView& chunk, std::uint32_t childCount,
                   RenderTapePolicyCandidate& out) {
  if (childCount < 2u || childCount > kRenderTapePolicyMaxChildren ||
      range.recordCount < childCount ||
      range.firstRecord > chunk.records.size() ||
      range.recordCount > chunk.records.size() - range.firstRecord) {
    return false;
  }
  out = RenderTapePolicyCandidate{
      .eventOrdinal = source.eventOrdinal,
      .sourceOrdinal = source.sourceOrdinal,
      .seqId = source.seqId,
      .logicalPassId = range.logicalPassId,
      .dagPassIndex = range.dagPassIndex,
      .passKind = range.passKind,
      .firstRecord = range.firstRecord,
      .recordCount = range.recordCount,
  };
  std::uint32_t expectedEnd = 0u;
  if (!checkedAdd(range.firstRecord, range.recordCount, expectedEnd)) {
    return false;
  }
  std::uint32_t primitiveTotal = 0u;
  for (std::uint32_t index = 0u; index < range.recordCount; ++index) {
    const auto record = chunk.record(range.firstRecord + index);
    if (!checkedAdd(primitiveTotal, record.drawHeader.primitiveCount,
                    primitiveTotal)) {
      return false;
    }
    if (!countFacts(record, out)) return false;
  }
  std::uint32_t shaderFacts = 0u;
  if (!checkedAdd(out.vertexShaderFacts, out.pixelShaderFacts, shaderFacts) ||
      !checkedAdd(shaderFacts, out.vertexInputFacts,
                  out.pipelineInputSectionFacts)) {
    return false;
  }
  out.drawTotal = range.recordCount;
  out.primitiveTotal = primitiveTotal;
  const auto base = range.recordCount / childCount;
  const auto extra = range.recordCount % childCount;
  out.children.reserve(childCount);
  std::uint32_t firstRecord = range.firstRecord;
  std::uint32_t minDraws = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t maxDraws = 0u;
  for (std::uint32_t child = 0u; child < childCount; ++child) {
    const auto recordCount = base + (child < extra ? 1u : 0u);
    std::uint32_t childDraws = 0u;
    std::uint32_t childPrimitives = 0u;
    for (std::uint32_t index = 0u; index < recordCount; ++index) {
      ++childDraws;
      const auto count = chunk.record(firstRecord + index).drawHeader.primitiveCount;
      if (!checkedAdd(childPrimitives, count, childPrimitives)) return false;
    }
    out.children.push_back(
        {firstRecord, recordCount, childDraws, childPrimitives});
    minDraws = std::min(minDraws, childDraws);
    maxDraws = std::max(maxDraws, childDraws);
    if (!checkedAdd(firstRecord, recordCount, firstRecord)) return false;
  }
  if (firstRecord != expectedEnd) return false;
  out.minChildDraws = minDraws;
  out.maxChildDraws = maxDraws;
  out.imbalance = maxDraws - minDraws;
  return true;
}

} // namespace

std::uint32_t renderTapePolicyCanonicalCandidateCount(
    std::uint32_t recordCount) noexcept {
  return static_cast<std::uint32_t>(std::count_if(
      kChildCounts.begin(), kChildCounts.end(),
      [recordCount](std::uint32_t childCount) {
        return childCount <= recordCount;
      }));
}

bool renderTapePolicyCandidateBudgetAccepts(
    std::size_t currentCount, std::uint32_t recordCount) noexcept {
  const auto additional = static_cast<std::size_t>(
      renderTapePolicyCanonicalCandidateCount(recordCount));
  return currentCount <= kRenderTapePolicyMaxCandidates &&
      additional <= kRenderTapePolicyMaxCandidates - currentCount;
}

RenderTapePolicyExploreResult exploreRenderTapeParallelPolicy(
    std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& verifiedCatalogue,
    std::span<const std::byte> identitySidecar) noexcept {
  RenderTapePolicyExploreResult result;
  RenderTapeIdentityView identity;
  result.identityValidation = validateRenderTapeIdentity(
      tape, verifiedCatalogue, identitySidecar, &identity);
  if (!result.identityValidation.tapeValidation.valid()) {
    result.status = RenderTapePolicyExploreStatus::InvalidTape;
    return result;
  }
  if (!result.identityValidation.valid()) {
    result.status = RenderTapePolicyExploreStatus::InvalidIdentity;
    return result;
  }
  if (identity.header.presentOrdinal == 0u) {
    result.status = RenderTapePolicyExploreStatus::UnsupportedProfile;
    return result;
  }
  ImportedRenderTapeView imported;
  const auto tapeValidation = validateRenderTape(tape, verifiedCatalogue, &imported);
  if (!tapeValidation.valid()) {
    result.status = RenderTapePolicyExploreStatus::InvalidTape;
    return result;
  }
  result.authenticatedInput = true;
  try {
    for (const auto& source : identity.sources) {
      const auto sourceEvent = std::find_if(
          imported.events.begin(), imported.events.end(), [&](const auto& event) {
            return event.ordinal == source.eventOrdinal &&
                   event.type == static_cast<std::uint32_t>(
                                     RenderTapeEventType::CommandChunk);
          });
      if (sourceEvent == imported.events.end()) continue;
      const auto eventIndex = static_cast<std::size_t>(
          std::distance(imported.events.begin(), sourceEvent));
      const auto event = imported.event(eventIndex);
      RenderTapeCommandChunkHeader fixed{};
      if (!load(event.payload, 0u, fixed)) continue;
      ImportedChunkView chunk;
      if (!importPrevalidatedCommandChunk(
              event.payload.subspan(sizeof(fixed), fixed.chunkBytes),
              CommandChunkEnvelope{fixed.wireVersion, fixed.recordCount,
                                   fixed.handleCount}, chunk)) {
        continue;
      }
      for (std::uint32_t local = 0u; local < source.rangeCount; ++local) {
        const auto& range = identity.ranges[source.firstRange + local];
        RenderTapePolicyRejection rejection{
            .eventOrdinal = source.eventOrdinal,
            .sourceOrdinal = source.sourceOrdinal,
            .seqId = source.seqId,
            .logicalPassId = range.logicalPassId,
            .dagPassIndex = range.dagPassIndex,
            .passKind = range.passKind,
            .firstRecord = range.firstRecord,
            .recordCount = range.recordCount,
        };
        if (range.recordCount == 0u || range.firstRecord > chunk.records.size() ||
            range.recordCount > chunk.records.size() - range.firstRecord) {
          rejection.reason = RenderTapePolicyRejectionReason::ArithmeticOverflow;
          result.rejections.push_back(rejection);
          continue;
        }
        bool allDraws = true;
        bool coordinator = false;
        for (std::uint32_t index = 0u; index < range.recordCount; ++index) {
          const auto type = chunk.record(range.firstRecord + index).header.type;
          if (isCoordinator(type)) coordinator = true;
          if (!isDraw(type)) allDraws = false;
        }
        if (!allDraws || coordinator) {
          rejection.reason = coordinator
              ? RenderTapePolicyRejectionReason::CoordinatorRecord
              : RenderTapePolicyRejectionReason::NonDrawRecord;
          result.rejections.push_back(rejection);
          continue;
        }
        if (!renderTapePolicyCandidateBudgetAccepts(result.candidates.size(),
                                                    range.recordCount)) {
          result.status =
              RenderTapePolicyExploreStatus::CandidateLimitExceeded;
          result.candidates.clear();
          result.rejections.clear();
          return result;
        }
        bool added = false;
        for (const auto childCount : kChildCounts) {
          if (childCount > range.recordCount) continue;
          RenderTapePolicyCandidate candidate;
          if (!makeCandidate(source, range, chunk, childCount, candidate)) {
            rejection.reason = RenderTapePolicyRejectionReason::ArithmeticOverflow;
            result.rejections.push_back(rejection);
            break;
          }
          result.candidates.push_back(std::move(candidate));
          added = true;
        }
        if (!added && range.recordCount < 2u) {
          rejection.reason =
              RenderTapePolicyRejectionReason::TooFewDrawRecords;
          result.rejections.push_back(rejection);
        }
      }
    }
  } catch (...) {
    result.status = RenderTapePolicyExploreStatus::AllocationFailed;
    result.candidates.clear();
    result.rejections.clear();
    return result;
  }
  result.status = RenderTapePolicyExploreStatus::Valid;
  return result;
}

const char* renderTapePolicyExploreStatusName(
    RenderTapePolicyExploreStatus status) noexcept {
  switch (status) {
  case RenderTapePolicyExploreStatus::Valid: return "valid";
  case RenderTapePolicyExploreStatus::InvalidTape: return "invalid-tape";
  case RenderTapePolicyExploreStatus::InvalidIdentity: return "invalid-identity";
  case RenderTapePolicyExploreStatus::UnsupportedProfile:
    return "unsupported-profile";
  case RenderTapePolicyExploreStatus::CandidateLimitExceeded:
    return "candidate-limit-exceeded";
  case RenderTapePolicyExploreStatus::AllocationFailed:
    return "allocation-failed";
  }
  return "unknown";
}

const char* renderTapePolicyRejectionReasonName(
    RenderTapePolicyRejectionReason reason) noexcept {
  switch (reason) {
  case RenderTapePolicyRejectionReason::CoordinatorRecord:
    return "coordinator-record";
  case RenderTapePolicyRejectionReason::NonDrawRecord: return "non-draw-record";
  case RenderTapePolicyRejectionReason::TooFewDrawRecords:
    return "too-few-draw-records";
  case RenderTapePolicyRejectionReason::ArithmeticOverflow:
    return "arithmetic-overflow";
  }
  return "unknown";
}

} // namespace dxmt9::d3d9
