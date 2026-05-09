// CPU-only chunk-build microbenchmark — V1 audit item (a), boundary B1.
//
// Purpose: measure the cost of building a POD chunk record from a typical
// D3D9 hot-path mix of state changes + draw calls, isolated from any
// bridge crossing, Metal driver, or GPU work. A regression in chunk-record
// build time (e.g., a new validation pass added to apply_state or
// record_draw) would not surface in any other counter today; this spec
// is the regression sentry.
//
// What is measured per iteration:
//   1) Encode a typical hot-path mix into POD records:
//        - SetTexture / SetSamplerState / SetRenderState / SetTransform
//          (carried inside an APPLY_STATE D9CDrawPrimitivePacket delta)
//        - SetVertexShaderConstantF / SetPixelShaderConstantF
//          (variable-size SetConst records appended into the payload arena)
//        - N DrawPrimitive records
//   2) Assemble a wire chunk: build header table, handle table, payload
//      arena (the equivalent of CommandRecorder::commit*() — sealing a
//      chunk for transfer).
//   3) Validate the chunk via validateImportedWireChunk (canonical
//      receiving-end parse cost; the importer's first job).
//
// What is NOT measured:
//   - dxmt9c bridge crossing (no WINE_UNIX_CALL invocation)
//   - Metal command encoding (no MTLCommandBuffer)
//   - GPU dispatch (no completion handler)
//   - Wine PE COM ref-counting (no D9CTexture* etc.)
//
// The inner loop builds the same chunk shape repeatedly and times the
// build-only cost with std::chrono::steady_clock. Output is a single
// deterministic line for future automation:
//
//   [chunk_record_micro] iterations=N mean_ns=... p50_ns=... p95_ns=... p99_ns=...
//
// Soft assertion: build cost below kBuildBudgetNs (very generous; intent
// is to catch only catastrophic regressions, not flake the test).

#include "device_c_record_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using dxmt9::d3d9::devicec::makeImportedWireChunkView;
using dxmt9::d3d9::devicec::validateImportedWireChunk;

// Iteration / batch counts. The outer batch loop reports per-iteration
// percentiles; the inner loop runs the build cycle. Total iterations =
// kInnerIterations * kOuterBatches. Chosen for ~1-2s wall time on a
// modern machine while keeping the percentile sample large enough to
// produce stable p99 numbers.
constexpr std::size_t kInnerIterations = 1000;
constexpr std::size_t kOuterBatches = 100;
constexpr std::size_t kTotalIterations = kInnerIterations * kOuterBatches;

// Typical hot-path workload shape per chunk:
//   - One APPLY_STATE record carrying the state delta (texture binds,
//     sampler states, render states, transforms — packed into one packet)
//   - One SetVsConstF (8 const4 = 128 bytes payload)
//   - One SetPsConstF (4 const4 = 64 bytes payload)
//   - kDrawsPerChunk DrawPrimitive records
constexpr std::size_t kDrawsPerChunk = 8;
constexpr std::uint32_t kVsConstFCount = 8;  // 8 × float4
constexpr std::uint32_t kPsConstFCount = 4;  // 4 × float4

// Soft regression bound: per-iteration build cycle should be well below
// 100 microseconds. Real numbers on a modern machine are ~1-10 us. The
// 100 us bound only fails on a catastrophic regression (e.g., 10x slowdown
// or accidental allocation in the inner loop).
constexpr std::uint64_t kBuildBudgetNs = 100'000ull;

// Build a state-delta APPLY_STATE record carrying the typical hot-path
// state mix: 4 texture binds, 8 sampler-state changes, 16 render states,
// 4 transform updates. The packet is the canonical PE→CommandRecorder
// payload shape, so building it dominates the boundary-B1 cost.
D9CCommandRecordApplyState makeApplyStateRecordHotPath() {
  D9CCommandRecordApplyState apply{};
  apply.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
  apply.header.size = sizeof(apply);

  auto& packet = apply.packet;

  // 4 texture binds (stages 0..3)
  packet.textureMask = 0x0fu;
  for (std::uint32_t stage = 0; stage < 4; ++stage) {
    const std::uint64_t handle = 0x10000ull + stage * 8u;
    packet.textures[stage] = D9CWireHandle{
        static_cast<std::uint32_t>(handle & 0xffffffffu),
        static_cast<std::uint32_t>(handle >> 32),
    };
  }

  // 8 sampler-state changes (D3DSAMP_MAGFILTER per stage 0..7)
  packet.samplerStateCount = 8u;
  for (std::uint32_t i = 0; i < 8; ++i) {
    packet.samplerStates[i].sampler = i;
    packet.samplerStates[i].type = 5u;   // D3DSAMP_MAGFILTER
    packet.samplerStates[i].value = 2u;  // D3DTEXF_LINEAR
  }

  // 16 render-state changes (typical mid-frame mix: ZWRITEENABLE,
  // ALPHABLENDENABLE, SRCBLEND, DESTBLEND, CULLMODE, etc.)
  packet.renderStateCount = 16u;
  for (std::uint32_t i = 0; i < 16; ++i) {
    packet.renderStates[i].state = 7u + i;
    packet.renderStates[i].value = 1u + i;
  }

  // 4 transforms (WORLD, VIEW, PROJECTION, TEXTURE0)
  packet.transformCount = 4u;
  for (std::uint32_t i = 0; i < 4; ++i) {
    packet.transforms[i].state = 256u + i;
    for (std::uint32_t j = 0; j < 16; ++j) {
      packet.transforms[i].matrix.m[j] = static_cast<float>(j) * 0.5f;
    }
  }

  return apply;
}

D9CCommandRecordDrawPrimitive makeDrawRecordHotPath(std::uint32_t startVertex,
                                                    std::uint32_t primitiveCount) {
  D9CCommandRecordDrawPrimitive draw{};
  draw.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  draw.header.size = sizeof(draw);
  draw.packet.primitiveType = 4u;  // D3DPT_TRIANGLELIST
  draw.packet.startVertex = startVertex;
  draw.packet.primitiveCount = primitiveCount;
  return draw;
}

// Reusable scratch buffers for the inner loop. Reserved once before
// timing; cleared (not freed) per iteration so the build cost we measure
// reflects the steady-state path, not first-time allocation.
struct ChunkScratch {
  std::vector<D9CCommandChunkWireRecordHeader> records;
  std::vector<std::uint8_t> payloadArena;
  std::vector<D9CCommandChunkWireHandleEntry> handles;

  ChunkScratch() {
    // Reserve generous capacity so push_back / resize never reallocate
    // during a steady-state iteration. These are the per-iteration
    // upper bounds for the workload shape encoded above.
    records.reserve(16);
    payloadArena.reserve(8192);
    handles.reserve(16);
  }

  void clear() {
    records.clear();
    payloadArena.clear();
    handles.clear();
  }
};

// Append a record's bytes into the payload arena, returning the offset
// at which the bytes start.
template <typename T>
std::uint32_t appendPayload(std::vector<std::uint8_t>& arena, const T& record) {
  const auto offset = static_cast<std::uint32_t>(arena.size());
  arena.resize(arena.size() + sizeof(record));
  std::memcpy(arena.data() + offset, &record, sizeof(record));
  return offset;
}

// Append a SetConst record's fixed header followed by the float4 element
// tail (count * 4 floats). Returns the payload offset of the record.
std::uint32_t appendSetConstFRecord(std::vector<std::uint8_t>& arena,
                                    std::uint32_t type, std::uint32_t start,
                                    std::uint32_t count) {
  D9CCommandRecordSetConst setConst{};
  setConst.header.type = type;
  setConst.header.size = static_cast<std::uint32_t>(
      sizeof(D9CCommandRecordSetConst) +
      static_cast<std::uint64_t>(count) * sizeof(float) * 4u);
  setConst.start = start;
  setConst.count = count;

  const auto offset = static_cast<std::uint32_t>(arena.size());
  arena.resize(arena.size() + setConst.header.size);
  std::memcpy(arena.data() + offset, &setConst, sizeof(setConst));
  // The element tail bytes are left as the prior arena content; their
  // exact value does not matter for the wire-format validator. This
  // mirrors how the PE recorder copies pre-staged const bytes from its
  // dirty-range tracker into the payload arena.
  return offset;
}

// Build one chunk worth of records into the scratch buffers. This
// function is the hot path being measured.
void buildChunk(ChunkScratch& scratch,
                const D9CCommandRecordApplyState& applyState,
                const D9CCommandRecordDrawPrimitive& drawTemplate) {
  scratch.clear();

  // 1) APPLY_STATE: state-delta record carries the texture / sampler /
  //    render-state / transform changes. Texture handles also rivet into
  //    the chunk handle table so the importer can resolve them.
  {
    const auto offset = appendPayload(scratch.payloadArena, applyState);
    const auto firstHandle = static_cast<std::uint32_t>(scratch.handles.size());
    // Mirror the recorder's handle-table append: 4 texture handles for
    // the 4 active stages.
    for (std::uint32_t stage = 0; stage < 4; ++stage) {
      const auto& wire = applyState.packet.textures[stage];
      const std::uint64_t handle = static_cast<std::uint64_t>(wire.lo) |
                                   (static_cast<std::uint64_t>(wire.hi) << 32);
      scratch.handles.push_back(D9CCommandChunkWireHandleEntry{
          .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
          .generation = D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
          .opaqueHandle = handle,
          .reserved0 = 0u,
          .reserved1 = 0u,
      });
    }
    const auto handleCount =
        static_cast<std::uint32_t>(scratch.handles.size()) - firstHandle;

    scratch.records.push_back(D9CCommandChunkWireRecordHeader{
        .type = static_cast<std::uint32_t>(D9C_COMMAND_RECORD_APPLY_STATE),
        .flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
        .payloadOffset = offset,
        .payloadSize = static_cast<std::uint32_t>(sizeof(applyState)),
        .firstHandle = firstHandle,
        .handleCount = handleCount,
        .reserved0 = 0u,
        .reserved1 = 0u,
    });
  }

  // 2) SetVsConstF: 8 × float4 = 128 byte tail.
  {
    const auto offset = appendSetConstFRecord(
        scratch.payloadArena, D9C_COMMAND_RECORD_SET_VS_CONST_F, 0u,
        kVsConstFCount);
    const auto payloadSize =
        static_cast<std::uint32_t>(sizeof(D9CCommandRecordSetConst) +
                                    kVsConstFCount * sizeof(float) * 4u);
    scratch.records.push_back(D9CCommandChunkWireRecordHeader{
        .type = static_cast<std::uint32_t>(D9C_COMMAND_RECORD_SET_VS_CONST_F),
        .flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
        .payloadOffset = offset,
        .payloadSize = payloadSize,
        .firstHandle = static_cast<std::uint32_t>(scratch.handles.size()),
        .handleCount = 0u,
        .reserved0 = 0u,
        .reserved1 = 0u,
    });
  }

  // 3) SetPsConstF: 4 × float4 = 64 byte tail.
  {
    const auto offset = appendSetConstFRecord(
        scratch.payloadArena, D9C_COMMAND_RECORD_SET_PS_CONST_F, 0u,
        kPsConstFCount);
    const auto payloadSize =
        static_cast<std::uint32_t>(sizeof(D9CCommandRecordSetConst) +
                                    kPsConstFCount * sizeof(float) * 4u);
    scratch.records.push_back(D9CCommandChunkWireRecordHeader{
        .type = static_cast<std::uint32_t>(D9C_COMMAND_RECORD_SET_PS_CONST_F),
        .flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
        .payloadOffset = offset,
        .payloadSize = payloadSize,
        .firstHandle = static_cast<std::uint32_t>(scratch.handles.size()),
        .handleCount = 0u,
        .reserved0 = 0u,
        .reserved1 = 0u,
    });
  }

  // 4) kDrawsPerChunk DrawPrimitive records. Each varies its startVertex
  //    so the run-coalescer (if invoked) sees distinct draw params.
  for (std::size_t i = 0; i < kDrawsPerChunk; ++i) {
    auto draw = drawTemplate;
    draw.packet.startVertex = static_cast<std::uint32_t>(i * 3u);
    const auto offset = appendPayload(scratch.payloadArena, draw);
    scratch.records.push_back(D9CCommandChunkWireRecordHeader{
        .type = static_cast<std::uint32_t>(D9C_COMMAND_RECORD_DRAW_PRIMITIVE),
        .flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
        .payloadOffset = offset,
        .payloadSize = static_cast<std::uint32_t>(sizeof(draw)),
        .firstHandle = static_cast<std::uint32_t>(scratch.handles.size()),
        .handleCount = 0u,
        .reserved0 = 0u,
        .reserved1 = 0u,
    });
  }
}

// "Seal a chunk" — assemble the wire view and run the importer's
// validator. This is the canonical PE→CommandRecorder boundary: the
// recorder builds, the chunk gets validated before transfer.
bool sealAndValidate(const ChunkScratch& scratch) {
  const auto wire = makeImportedWireChunkView(
      scratch.records.data(),
      static_cast<std::uint32_t>(scratch.records.size()),
      scratch.payloadArena.data(),
      static_cast<std::uint32_t>(scratch.payloadArena.size()),
      scratch.handles.data(),
      static_cast<std::uint32_t>(scratch.handles.size()));
  const auto validation = validateImportedWireChunk(wire);
  return validation.valid();
}

void runMicroBench() {
  // Pre-build the constant inputs so the timed region only measures the
  // scratch encode + assemble + validate path.
  const auto applyState = makeApplyStateRecordHotPath();
  const auto drawTemplate = makeDrawRecordHotPath(0u, 1u);

  ChunkScratch scratch;

  // One-time warm-up to populate scratch capacities, prime caches, and
  // confirm validation succeeds before timing.
  buildChunk(scratch, applyState, drawTemplate);
  if (!sealAndValidate(scratch)) {
    std::cerr << "chunk_record_micro_spec: warm-up validation failed\n";
    std::exit(EXIT_FAILURE);
  }

  std::vector<std::uint64_t> samples;
  samples.reserve(kTotalIterations);

  for (std::size_t batch = 0; batch < kOuterBatches; ++batch) {
    for (std::size_t inner = 0; inner < kInnerIterations; ++inner) {
      const auto t0 = std::chrono::steady_clock::now();
      buildChunk(scratch, applyState, drawTemplate);
      const bool ok = sealAndValidate(scratch);
      const auto t1 = std::chrono::steady_clock::now();

      if (!ok) {
        std::cerr << "chunk_record_micro_spec: validation failed at batch="
                  << batch << " inner=" << inner << "\n";
        std::exit(EXIT_FAILURE);
      }

      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          t1 - t0)
                          .count();
      samples.push_back(static_cast<std::uint64_t>(ns));
    }
  }

  // Compute mean / p50 / p95 / p99.
  std::sort(samples.begin(), samples.end());
  const std::size_t n = samples.size();
  std::uint64_t sum = 0;
  for (auto v : samples) sum += v;
  const std::uint64_t mean = sum / n;
  const auto pct = [&](double p) -> std::uint64_t {
    auto idx = static_cast<std::size_t>(static_cast<double>(n) * p);
    if (idx >= n) idx = n - 1;
    return samples[idx];
  };
  const std::uint64_t p50 = pct(0.50);
  const std::uint64_t p95 = pct(0.95);
  const std::uint64_t p99 = pct(0.99);

  // Deterministic scrape line. Future CI scrapers should match on the
  // [chunk_record_micro] prefix and parse key=value pairs.
  std::cout << "[chunk_record_micro] iterations=" << n
            << " mean_ns=" << mean
            << " p50_ns=" << p50
            << " p95_ns=" << p95
            << " p99_ns=" << p99
            << " budget_ns=" << kBuildBudgetNs
            << " draws_per_chunk=" << kDrawsPerChunk
            << "\n";

  // Soft assertion: only fail on a catastrophic regression. The mean
  // is the appropriate signal — high-percentile noise from scheduler
  // jitter / steal cycles should not flake the test.
  if (mean > kBuildBudgetNs) {
    std::cerr << "chunk_record_micro_spec: mean build cost " << mean
              << " ns exceeds budget " << kBuildBudgetNs << " ns\n";
    std::exit(EXIT_FAILURE);
  }
}

}  // namespace

int main() {
  runMicroBench();
  return EXIT_SUCCESS;
}
