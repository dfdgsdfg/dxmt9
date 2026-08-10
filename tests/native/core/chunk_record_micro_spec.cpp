// CPU-only canonical chunk-build microbenchmark for boundary B1.
//
// Measures a representative sparse APPLY_STATE, two constant uploads, eight
// draws, canonical table/arena sealing, and unix-side canonical preflight validation.
#include "device_c_chunk_validate.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <type_traits>
#include <vector>

namespace {

using dxmt9::d3d9::CommandChunkEnvelope;
using dxmt9::d3d9::validateCommandChunk;

constexpr std::size_t kInnerIterations = 1000u;
constexpr std::size_t kOuterBatches = 100u;
constexpr std::size_t kTotalIterations =
    kInnerIterations * kOuterBatches;
constexpr std::size_t kDrawsPerChunk = 8u;
constexpr std::uint64_t kBuildBudgetNs = 100'000ull;

std::size_t alignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

struct ChunkScratch {
  std::vector<D9CCommandChunkWireRecordHeader> records;
  std::vector<D9CCommandChunkWireHandleEntry> handles;
  std::vector<std::byte> payload;
  std::vector<std::byte> blob;

  ChunkScratch() {
    records.reserve(16u);
    handles.reserve(8u);
    payload.reserve(8192u);
    blob.reserve(12288u);
  }

  void clear() {
    records.clear();
    handles.clear();
    payload.clear();
    blob.clear();
  }
};

template <typename T>
std::uint32_t appendValues(std::vector<std::byte>& bytes,
                           const T* values,
                           std::size_t count,
                           std::size_t base) {
  const auto offset = alignUp(bytes.size(), alignof(T));
  bytes.resize(offset + sizeof(T) * count);
  std::memcpy(bytes.data() + offset, values, sizeof(T) * count);
  return static_cast<std::uint32_t>(offset - base);
}

template <typename T>
void overwriteValue(std::vector<std::byte>& bytes,
                    std::size_t offset,
                    const T& value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void appendApplyState(ChunkScratch& scratch) {
  const auto payloadStart = alignUp(scratch.payload.size(), 8u);
  scratch.payload.resize(payloadStart);

  std::array<D9CCommandChunkWireRenderState, 16> renderStates{};
  for (std::uint32_t i = 0u; i < renderStates.size(); ++i) {
    renderStates[i] = D9CCommandChunkWireRenderState{7u + i, 1u + i};
  }

  std::array<D9CCommandChunkWireTextureBinding, 4> textures{};
  for (std::uint32_t slot = 0u; slot < textures.size(); ++slot) {
    textures[slot] = D9CCommandChunkWireTextureBinding{
        .slot = slot,
        .valid = 1u,
        .handleIndex = slot,
    };
    scratch.handles.push_back(D9CCommandChunkWireHandleEntry{
        .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
        .generation = 1u,
        .objectId = 0x10000ull + slot,
    });
  }

  std::array<D9CDrawPacketSamplerState, 8> samplerStates{};
  for (std::uint32_t slot = 0u; slot < samplerStates.size(); ++slot) {
    samplerStates[slot] =
        D9CDrawPacketSamplerState{slot, 5u, 2u};
  }

  std::array<D9CDrawPacketTransform, 4> transforms{};
  for (std::uint32_t i = 0u; i < transforms.size(); ++i) {
    transforms[i].state = 256u + i;
    for (std::uint32_t j = 0u; j < 16u; ++j) {
      transforms[i].matrix.m[j] = static_cast<float>(j) * 0.5f;
    }
  }

  D9CCommandChunkWireDrawHeader draw{
      .sectionCount = 4u,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
      .sectionPayloadOffset =
          sizeof(D9CCommandChunkWireDrawHeader) +
          4u * sizeof(D9CCommandChunkWireSectionDesc),
  };
  const auto drawOffset = appendValues(
      scratch.payload, &draw, 1u, payloadStart);
  std::array<D9CCommandChunkWireSectionDesc, 4> sections{};
  const auto sectionTableOffset = appendValues(
      scratch.payload, sections.data(), sections.size(), payloadStart);

  const auto appendSection = [&](std::size_t index, std::uint16_t kind,
                                 const auto& values) {
    using Value = typename std::decay_t<decltype(values)>::value_type;
    const auto offset = appendValues(
        scratch.payload, values.data(), values.size(), payloadStart);
    sections[index] = D9CCommandChunkWireSectionDesc{
        .kind = kind,
        .elementSize = static_cast<std::uint16_t>(sizeof(Value)),
        .count = static_cast<std::uint32_t>(values.size()),
        .payloadOffset = offset,
        .byteSize =
            static_cast<std::uint32_t>(sizeof(Value) * values.size()),
    };
  };
  appendSection(0u, D9C_COMMAND_CHUNK_SECTION_RENDER_STATE,
                renderStates);
  appendSection(1u, D9C_COMMAND_CHUNK_SECTION_TEXTURE, textures);
  appendSection(2u, D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE,
                samplerStates);
  appendSection(3u, D9C_COMMAND_CHUNK_SECTION_TRANSFORM, transforms);
  overwriteValue(scratch.payload, payloadStart + drawOffset, draw);
  std::memcpy(scratch.payload.data() + payloadStart + sectionTableOffset,
              sections.data(), sizeof(sections));

  scratch.records.push_back(D9CCommandChunkWireRecordHeader{
      .type = D9C_COMMAND_RECORD_APPLY_STATE,
      .flags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
      .payloadOffset = static_cast<std::uint32_t>(payloadStart),
      .payloadSize =
          static_cast<std::uint32_t>(scratch.payload.size() - payloadStart),
      .firstHandle = 0u,
      .handleCount = static_cast<std::uint32_t>(scratch.handles.size()),
  });
}

void appendConstant(ChunkScratch& scratch, std::uint32_t type,
                    std::uint32_t count) {
  const auto payloadStart = alignUp(scratch.payload.size(), 8u);
  scratch.payload.resize(payloadStart);
  const D9CCommandChunkWireSetConst fixed{
      .startRegister = 0u,
      .registerCount = count,
  };
  appendValues(scratch.payload, &fixed, 1u, payloadStart);
  std::array<std::byte, 128> values{};
  appendValues(scratch.payload, values.data(), count * 16u, payloadStart);
  scratch.records.push_back(D9CCommandChunkWireRecordHeader{
      .type = type,
      .flags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
      .payloadOffset = static_cast<std::uint32_t>(payloadStart),
      .payloadSize =
          static_cast<std::uint32_t>(scratch.payload.size() - payloadStart),
      .firstHandle = static_cast<std::uint32_t>(scratch.handles.size()),
  });
}

void appendDraw(ChunkScratch& scratch, std::uint32_t startVertex) {
  const auto payloadStart = alignUp(scratch.payload.size(), 8u);
  scratch.payload.resize(payloadStart);
  const D9CCommandChunkWireDrawHeader draw{
      .primitiveType = 4u,
      .startVertex = startVertex,
      .primitiveCount = 1u,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
      .sectionPayloadOffset = sizeof(D9CCommandChunkWireDrawHeader),
  };
  appendValues(scratch.payload, &draw, 1u, payloadStart);
  scratch.records.push_back(D9CCommandChunkWireRecordHeader{
      .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      .flags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
      .payloadOffset = static_cast<std::uint32_t>(payloadStart),
      .payloadSize = sizeof(draw),
      .firstHandle = static_cast<std::uint32_t>(scratch.handles.size()),
  });
}

void buildChunk(ChunkScratch& scratch) {
  scratch.clear();
  appendApplyState(scratch);
  appendConstant(scratch, D9C_COMMAND_RECORD_SET_VS_CONST_F, 8u);
  appendConstant(scratch, D9C_COMMAND_RECORD_SET_PS_CONST_F, 4u);
  for (std::uint32_t draw = 0u; draw < kDrawsPerChunk; ++draw) {
    appendDraw(scratch, draw * 3u);
  }
}

bool sealAndValidate(ChunkScratch& scratch) {
  const auto recordTableOffset =
      static_cast<std::uint32_t>(sizeof(D9CCommandChunkWireHeader));
  const auto handleTableOffset = recordTableOffset +
      static_cast<std::uint32_t>(
          scratch.records.size() *
          sizeof(D9CCommandChunkWireRecordHeader));
  const auto payloadArenaOffset = static_cast<std::uint32_t>(
      alignUp(handleTableOffset +
                  scratch.handles.size() *
                      sizeof(D9CCommandChunkWireHandleEntry),
              8u));
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = recordTableOffset,
      .recordCount = static_cast<std::uint32_t>(scratch.records.size()),
      .handleTableOffset = handleTableOffset,
      .handleCount = static_cast<std::uint32_t>(scratch.handles.size()),
      .payloadArenaOffset = payloadArenaOffset,
      .payloadArenaSize = static_cast<std::uint32_t>(scratch.payload.size()),
  };
  scratch.blob.resize(payloadArenaOffset + scratch.payload.size());
  std::memcpy(scratch.blob.data(), &header, sizeof(header));
  std::memcpy(scratch.blob.data() + recordTableOffset,
              scratch.records.data(),
              scratch.records.size() * sizeof(scratch.records[0]));
  std::memcpy(scratch.blob.data() + handleTableOffset,
              scratch.handles.data(),
              scratch.handles.size() * sizeof(scratch.handles[0]));
  std::memcpy(scratch.blob.data() + payloadArenaOffset,
              scratch.payload.data(), scratch.payload.size());
  return validateCommandChunk(
             scratch.blob,
             CommandChunkEnvelope{
                 .version = D9C_COMMAND_CHUNK_VERSION,
                 .recordCount =
                     static_cast<std::uint32_t>(scratch.records.size()),
                 .handleCount =
                     static_cast<std::uint32_t>(scratch.handles.size()),
             })
      .valid();
}

void runMicroBench() {
  ChunkScratch scratch;
  buildChunk(scratch);
  if (!sealAndValidate(scratch)) {
    std::cerr << "chunk_record_micro_spec: warm-up validation failed\n";
    std::exit(EXIT_FAILURE);
  }

  std::vector<std::uint64_t> samples;
  samples.reserve(kTotalIterations);
  for (std::size_t batch = 0u; batch < kOuterBatches; ++batch) {
    for (std::size_t i = 0u; i < kInnerIterations; ++i) {
      const auto begin = std::chrono::steady_clock::now();
      buildChunk(scratch);
      const bool valid = sealAndValidate(scratch);
      const auto end = std::chrono::steady_clock::now();
      if (!valid) {
        std::cerr << "chunk_record_micro_spec: canonical validation failed\n";
        std::exit(EXIT_FAILURE);
      }
      samples.push_back(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              end - begin)
              .count()));
    }
  }

  std::sort(samples.begin(), samples.end());
  std::uint64_t sum = 0u;
  for (const auto sample : samples) {
    sum += sample;
  }
  const auto percentile = [&](std::size_t numerator) {
    return samples[(samples.size() - 1u) * numerator / 100u];
  };
  const auto mean = sum / samples.size();
  std::cout << "[chunk_record_micro] iterations=" << samples.size()
            << " mean_ns=" << mean
            << " p50_ns=" << percentile(50u)
            << " p95_ns=" << percentile(95u)
            << " p99_ns=" << percentile(99u) << '\n';
  if (mean >= kBuildBudgetNs) {
    std::cerr << "chunk_record_micro_spec: mean canonical build cost "
              << mean << " ns exceeds budget\n";
    std::exit(EXIT_FAILURE);
  }
}

}  // namespace

int main() {
  runMicroBench();
  return EXIT_SUCCESS;
}
