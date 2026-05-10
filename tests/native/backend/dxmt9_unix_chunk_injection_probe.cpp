// Unix-side chunk-injection probe — Z1 audit item #2 (B3+B4 isolation).
//
// Goal: measure dxmt9 boundary B3 (CommandQueue) + B4 (encode thread)
// throughput WITHOUT going through the PE D3D9 frontend or the
// WINE_UNIX_CALL bridge. EncodeReplayProbe and ChainParametricProbe both
// route through the real PE CommandRecorder, so their B3+B4 numbers
// contain non-trivial PE recording overhead. This probe constructs a
// wire-format chunk in memory on the unix side and feeds it directly
// into dxmt9c_device_commit_chunk(), the same entry the
// WINE_UNIX_CALL handler uses. The PE pieces — D9CDrawPrimitivePacket
// state pre-shadow, dxmt9c_factory_*, the WINE call dispatcher — are
// bypassed entirely.
//
// What is measured:
//   - B3: command_buffers, sub_command_buffers, queue_writer_wait_*,
//         queue_commit_wait_*  (via the cumulative `[dxmt9-perf]` line)
//   - B4: encode_chunk_calls, encode_chunk_cpu_*, render_pass_*,
//         encode_draw_*  (same)
//   - GPU wall time (B5 cross-check):
//         gpu_command_buffer_time_*  (same)
//
// What is NOT measured:
//   - PE recorder cost (no PE app)
//   - WINE_UNIX_CALL marshalling cost (we're already on the unix side)
//   - Drawable acquisition / present pacing (no Present record in chunk)
//
// Tunables (env vars, read once at startup):
//   UNIX_CHUNK_INJECT_ITERATIONS   default 1000  — chunk count
//   UNIX_CHUNK_INJECT_DRAWS        default 8     — draws per chunk
//
// Output:
//   1. The probe's own line:
//        `[unix_chunk_inject] iterations=N draws_per_chunk=K
//         total_ms=... per_iter_us=... commits=... bytes_per_chunk=...`
//   2. The cumulative `[dxmt9-perf]` line emitted by the existing atexit
//      handler when DXMT_PERF_COUNTERS is set in the environment.
//
// Soft assertion: per-iteration commit cost below kCommitBudgetMs (very
// generous; only catches catastrophic regressions). The probe never
// fails on legitimate timing variance.

#include "device_c_common.hpp"
#include "dxmt9/com.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/device_c.h"
#include "dxmt9/dxmt9_device.hpp"
#include "../../../src/winemetal/Metal.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace dxmt9::core;

// Tunables read once at process start. Generous defaults match
// EncodeReplayProbe (1000 iterations) so per-chunk cost can be compared
// directly across the two probes.
struct ProbeConfig {
  std::uint64_t iterations = 1000;
  std::uint32_t drawsPerChunk = 8;
};

std::uint64_t envU64(const char* name, std::uint64_t fallback) {
  const char* v = std::getenv(name);
  if (!v || v[0] == '\0') return fallback;
  char* end = nullptr;
  const auto parsed = std::strtoull(v, &end, 10);
  return end != v ? parsed : fallback;
}

ProbeConfig readConfig() {
  ProbeConfig cfg;
  cfg.iterations = std::max<std::uint64_t>(envU64("UNIX_CHUNK_INJECT_ITERATIONS", 1000), 1);
  cfg.drawsPerChunk = static_cast<std::uint32_t>(
      std::max<std::uint64_t>(envU64("UNIX_CHUNK_INJECT_DRAWS", 8), 1));
  return cfg;
}

// Soft regression bound — per-iteration commit_chunk + encode + GPU
// completion. EncodeReplayProbe historical numbers are ~22ms / chunk
// (X1, 28 chunks / 621ms total) on a real PE path. Native injection
// should be at least no worse; 100ms/chunk catches only catastrophic
// regressions.
constexpr double kCommitBudgetMs = 100.0;

// Helpers: construct wire blob mirroring resource_hazard_spec.cpp.
std::uint64_t wireValueFromPtr(const void* ptr) {
  return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ptr));
}

D9CWireHandle wireHandleFromPtr(const void* ptr) {
  D9CWireHandle h{};
  const auto v = wireValueFromPtr(ptr);
  h.lo = static_cast<std::uint32_t>(v);
  h.hi = static_cast<std::uint32_t>(v >> 32);
  return h;
}

template <typename T>
void appendPod(std::vector<std::uint8_t>& bytes, const T& value) {
  const auto* begin = reinterpret_cast<const std::uint8_t*>(&value);
  bytes.insert(bytes.end(), begin, begin + sizeof(T));
}

D9CCommandChunkWireHandleEntry wireHandleEntry(std::uint32_t kind,
                                               const void* ptr) {
  return D9CCommandChunkWireHandleEntry{
      .kind = kind,
      .generation = D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
      .opaqueHandle = wireValueFromPtr(ptr),
      .reserved0 = 0u,
      .reserved1 = 0u,
  };
}

D9CCommandChunkWireRecordHeader wireRecordHeader(std::uint32_t type,
                                                 std::uint32_t payloadOffset,
                                                 std::uint32_t payloadSize,
                                                 std::uint32_t firstHandle,
                                                 std::uint32_t handleCount) {
  return D9CCommandChunkWireRecordHeader{
      .type = type,
      .flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
      .payloadOffset = payloadOffset,
      .payloadSize = payloadSize,
      .firstHandle = firstHandle,
      .handleCount = handleCount,
      .reserved0 = 0u,
      .reserved1 = 0u,
  };
}

// Assemble the full wire-format blob: header + record table + handle
// table + payload arena. Mirrors resource_hazard_spec.cpp::makeWireChunkBlob.
std::vector<std::uint8_t> makeWireChunkBlob(
    std::span<const std::uint8_t> payload,
    std::span<const D9CCommandChunkWireRecordHeader> records,
    std::span<const D9CCommandChunkWireHandleEntry> handles) {
  D9CCommandChunkWireHeader header{};
  header.version = D9C_COMMAND_CHUNK_WIRE_VERSION;
  header.headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE;
  header.recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE;
  header.handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE;
  header.recordTableOffset = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE;
  header.recordCount = static_cast<std::uint32_t>(records.size());
  header.handleTableOffset = header.recordTableOffset +
      header.recordCount * D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE;
  header.handleCount = static_cast<std::uint32_t>(handles.size());
  header.payloadArenaOffset = header.handleTableOffset +
      header.handleCount * D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE;
  header.payloadArenaSize = static_cast<std::uint32_t>(payload.size());

  std::vector<std::uint8_t> blob;
  blob.reserve(header.payloadArenaOffset + header.payloadArenaSize);
  appendPod(blob, header);
  for (const auto& r : records) appendPod(blob, r);
  for (const auto& h : handles) appendPod(blob, h);
  blob.insert(blob.end(), payload.begin(), payload.end());
  return blob;
}

// Build the canonical hot-path chunk shape. The chunk shape is fixed
// across iterations; only the records' contents are reused. Resources
// (RT, VB) are referenced by D9CSurface*/D9CBuffer* wrapper pointers,
// which the importer resolves to core::Handle via wireValuePtr.
struct ChunkInputs {
  D9CSurface* renderTargetWire = nullptr;
  D9CBuffer* vertexBufferWire = nullptr;
  std::uint32_t drawsPerChunk = 1;
};

// One APPLY_STATE record (RT bind + VB stream + minimal RS), one CLEAR
// (full-target color clear), then drawsPerChunk DRAW_PRIMITIVE records
// each with no state delta (they coalesce into a draw run via
// scanImportedDrawRun on the import side, exercising the run-coalescer
// hot path on every iteration).
struct BuiltChunk {
  std::vector<std::uint8_t> blob;             // full wire-format blob
  std::uint32_t recordCount = 0;
  std::uint32_t handleCount = 0;
};

BuiltChunk buildChunk(const ChunkInputs& in) {
  std::vector<std::uint8_t> payload;
  std::vector<D9CCommandChunkWireRecordHeader> records;
  std::vector<D9CCommandChunkWireHandleEntry> handles;

  // Handle table — RT (surface) + VB (buffer). Order matters for the
  // chunk's bulk markChunkResources walk.
  handles.push_back(wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE,
                                    in.renderTargetWire));
  handles.push_back(wireHandleEntry(D9C_CHUNK_HANDLE_KIND_BUFFER,
                                    in.vertexBufferWire));

  // Record 1: APPLY_STATE — RT bind + stream source + a couple of
  // render states. Mirrors what a typical mid-frame draw-packet state
  // delta looks like in production.
  {
    D9CCommandRecordApplyState apply{};
    apply.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
    apply.header.size = sizeof(apply);
    auto& packet = apply.packet;
    packet.rtMask = 0x1u;
    packet.rtHandles[0] = wireHandleFromPtr(in.renderTargetWire);
    packet.streamSourceMask = 0x1u;
    packet.streamSources[0].buffer = wireHandleFromPtr(in.vertexBufferWire);
    packet.streamSources[0].offset = 0u;
    packet.streamSources[0].stride = 16u;
    packet.renderStateCount = 2u;
    packet.renderStates[0].state = 7u;   // D3DRS_ZENABLE
    packet.renderStates[0].value = 0u;   // disabled
    packet.renderStates[1].state = 22u;  // D3DRS_CULLMODE
    packet.renderStates[1].value = 1u;   // D3DCULL_NONE
    packet.viewportValid = 1u;
    packet.viewport.x = 0;
    packet.viewport.y = 0;
    packet.viewport.width = 16u;
    packet.viewport.height = 16u;
    packet.viewport.minZ = 0.0f;
    packet.viewport.maxZ = 1.0f;

    const auto offset = static_cast<std::uint32_t>(payload.size());
    appendPod(payload, apply);
    records.push_back(wireRecordHeader(
        D9C_COMMAND_RECORD_APPLY_STATE, offset,
        static_cast<std::uint32_t>(sizeof(apply)),
        /*firstHandle=*/0u, /*handleCount=*/2u));
  }

  // Record 2: CLEAR — full-target color clear. No rect array
  // (rectCount == 0, rectOffset is at the byte after the fixed header
  // but unused for full-target clears).
  {
    D9CCommandRecordClear clear{};
    clear.header.type = D9C_COMMAND_RECORD_CLEAR;
    clear.header.size = sizeof(clear);
    clear.flags = 1u;          // D3DCLEAR_TARGET
    clear.colorARGB = 0xff112233u;
    clear.z = 1.0f;
    clear.stencil = 0u;
    clear.rectCount = 0u;
    clear.rectOffset = sizeof(D9CCommandRecordClear);

    const auto offset = static_cast<std::uint32_t>(payload.size());
    appendPod(payload, clear);
    records.push_back(wireRecordHeader(
        D9C_COMMAND_RECORD_CLEAR, offset,
        static_cast<std::uint32_t>(sizeof(clear)),
        /*firstHandle=*/0u, /*handleCount=*/1u));
  }

  // Records 3..3+N: DRAW_PRIMITIVE with empty state delta. Each draw
  // varies its startVertex so the importer's run-coalescer
  // (scanImportedDrawRun) sees N distinct DrawParams in a single
  // submitDrawRun call.
  for (std::uint32_t i = 0; i < in.drawsPerChunk; ++i) {
    D9CCommandRecordDrawPrimitive draw{};
    draw.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
    draw.header.size = sizeof(draw);
    draw.packet.primitiveType = 4u;  // D3DPT_TRIANGLELIST
    draw.packet.primitiveCount = 1u;
    draw.packet.startVertex = i * 3u;
    // No state delta — coalescer treats consecutive empty-delta draws
    // as one run.

    const auto offset = static_cast<std::uint32_t>(payload.size());
    appendPod(payload, draw);
    records.push_back(wireRecordHeader(
        D9C_COMMAND_RECORD_DRAW_PRIMITIVE, offset,
        static_cast<std::uint32_t>(sizeof(draw)),
        /*firstHandle=*/0u, /*handleCount=*/0u));
  }

  BuiltChunk result;
  result.recordCount = static_cast<std::uint32_t>(records.size());
  result.handleCount = static_cast<std::uint32_t>(handles.size());
  result.blob = makeWireChunkBlob(payload, records, handles);
  return result;
}

// Assemble the D9CCommandChunk header that points into a built blob.
D9CCommandChunk makeCommitChunk(const BuiltChunk& chunk) {
  D9CCommandChunk c{};
  c.version = D9C_COMMAND_CHUNK_VERSION;
  c.recordCount = chunk.recordCount;
  c.recordBytes = static_cast<std::uint32_t>(chunk.blob.size());
  c.records = wireHandleFromPtr(chunk.blob.data());
  c.handleCount = chunk.handleCount;
  c.handles = D9CWireHandle{};
  return c;
}

int probeMain(const ProbeConfig& cfg) {
  // Step 1: pick a Metal device. Mirrors dxmt9c_factory_create in
  // src/d3d9/device_c_factory.cpp. On a host without a Metal-capable
  // GPU (CI box), this returns a 0-element array — bail out
  // gracefully with a print line so the harness still has a parseable
  // marker (the probe is a soft-assertion regression sentry).
  auto wmtDevices = WMT::CopyAllDevices();
  if (!wmtDevices || wmtDevices.count() == 0) {
    std::cout
        << "[unix_chunk_inject] iterations=0 draws_per_chunk=0 status=no-wmt-device\n";
    return EXIT_SUCCESS;
  }

  // Step 2: build the upper dxmt9::Device. CommandQueue's worker
  // threads spawn here; they remain blocked on writeCv_ until the
  // first commit_chunk submits work.
  dxmt9::DEVICE_DESC desc{};
  desc.device = WMT::Device{wmtDevices.object(0)};
  auto upperDevice = dxmt9::CreateDXMT9Device(desc);
  if (!upperDevice) {
    std::cerr << "[unix_chunk_inject] CreateDXMT9Device failed\n";
    return EXIT_FAILURE;
  }

  // Step 3: wrap the upper Device in a COM factory + minimal D3D9
  // device. Direct3DCreate9Ex consumes the upper Device by-value.
  auto* d3d = dxmt9::com::Direct3DCreate9Ex(dxmt9::com::D3D_SDK_VERSION,
                                            std::move(upperDevice));
  if (!d3d) {
    std::cerr << "[unix_chunk_inject] Direct3DCreate9Ex failed\n";
    return EXIT_FAILURE;
  }
  PresentParameters params{};
  params.backBufferWidth = 16u;
  params.backBufferHeight = 16u;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.deviceWindow = Handle{77u};  // sentinel; not composited.
  params.presentationInterval = PresentInterval::Immediate;

  auto* device = d3d->CreateDeviceEx(0u, params, nullptr);
  if (!device) {
    std::cerr << "[unix_chunk_inject] CreateDeviceEx failed\n";
    d3d->Release();
    return EXIT_FAILURE;
  }
  // The factory holds a ref; AddRef again so D9CDevice's dtor's
  // Release() balances cleanly.
  device->AddRef();

  int returnCode = EXIT_SUCCESS;
  std::uint64_t commits = 0;
  std::uint64_t firstChunkBytes = 0;

  // Step 4: build a render target + vertex buffer that every chunk
  // references. Both stay alive for the entire run.
  auto renderTarget = device->CreateSurface(SurfaceDesc{
      .width = 16u,
      .height = 16u,
      .format = Format::A8R8G8B8,
      .pool = Pool::Default,
      .usage = UsageRenderTarget,
      .renderTarget = true,
  });
  auto vertexBuffer = device->CreateBuffer(BufferDesc{
      .size = 256u,
      .pool = Pool::Default,
      .usage = UsageVertexBuffer,
  });

  if (!renderTarget || !vertexBuffer) {
    std::cerr << "[unix_chunk_inject] resource creation failed\n";
    returnCode = EXIT_FAILURE;
  } else {
    D9CDevice cDevice(device);
    D9CSurface renderTargetWire(renderTarget);
    D9CBuffer vertexBufferWire(vertexBuffer);

    ChunkInputs inputs{};
    inputs.renderTargetWire = &renderTargetWire;
    inputs.vertexBufferWire = &vertexBufferWire;
    inputs.drawsPerChunk = cfg.drawsPerChunk;

    // Build the chunk once and reuse the same blob for every commit.
    // `dxmt9c_device_commit_chunk` does not retain the wire blob past
    // the call — it imports + dispatches synchronously — so feeding
    // the same buffer N times is safe.
    BuiltChunk built = buildChunk(inputs);
    firstChunkBytes = static_cast<std::uint64_t>(built.blob.size());
    auto chunk = makeCommitChunk(built);

    // Step 5: warm-up commit. Confirms the chunk is well-formed
    // before we start timing; also primes any one-time pipeline
    // build cost so per-iteration numbers reflect the steady state.
    {
      const std::int32_t hr = dxmt9c_device_commit_chunk(&cDevice, &chunk);
      if (hr != D3D_OK) {
        std::cerr << "[unix_chunk_inject] warm-up commit_chunk failed hr=0x"
                  << std::hex << hr << std::dec << "\n";
        returnCode = EXIT_FAILURE;
      } else {
        ++commits;
      }
    }

    // Step 6: hot loop. N commits of the same chunk shape. Encode
    // thread + GPU run concurrently with this thread; bounded by
    // queue back-pressure (writer-wait counters surface that).
    if (returnCode == EXIT_SUCCESS) {
      const auto t0 = std::chrono::steady_clock::now();
      for (std::uint64_t i = 0; i < cfg.iterations; ++i) {
        const std::int32_t hr = dxmt9c_device_commit_chunk(&cDevice, &chunk);
        if (hr != D3D_OK) {
          std::cerr << "[unix_chunk_inject] commit_chunk failed at i=" << i
                    << " hr=0x" << std::hex << hr << std::dec << "\n";
          returnCode = EXIT_FAILURE;
          break;
        }
        ++commits;
      }
      const auto t1 = std::chrono::steady_clock::now();
      const auto wallNs =
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
      const double totalMs = static_cast<double>(wallNs) / 1'000'000.0;
      const double perIterUs =
          cfg.iterations == 0
              ? 0.0
              : (static_cast<double>(wallNs) / 1000.0) /
                    static_cast<double>(cfg.iterations);

      // Step 7: deterministic scrape line. Format is intentionally
      // self-describing so a future automation script can parse
      // key=value pairs without context. The cumulative
      // `[dxmt9-perf]` line that `dxmt9::perf::report()` emits at
      // atexit is the source of truth for B3+B4 counter values; this
      // line is the wall-clock summary for the wrapper script.
      std::cout << "[unix_chunk_inject] iterations=" << cfg.iterations
                << " draws_per_chunk=" << cfg.drawsPerChunk
                << " commits=" << commits
                << " bytes_per_chunk=" << firstChunkBytes
                << " total_ms=" << totalMs
                << " per_iter_us=" << perIterUs
                << " budget_ms_per_iter=" << kCommitBudgetMs
                << "\n";

      // Soft regression sentinel — only fail on catastrophic
      // slowdown. Real numbers are expected to be a small fraction
      // of the budget.
      if (cfg.iterations > 0 &&
          (perIterUs / 1000.0) > kCommitBudgetMs) {
        std::cerr << "[unix_chunk_inject] mean per-iter cost "
                  << (perIterUs / 1000.0)
                  << " ms exceeds budget " << kCommitBudgetMs
                  << " ms\n";
        returnCode = EXIT_FAILURE;
      }
    }
  }

  // Step 8: release in reverse order so the queue's dtor (joined by
  // the device's release path) waits on outstanding GPU work — that
  // is our only synchronization barrier; CommandQueue::~CommandQueue
  // joins encode/finish/completion threads and the driver flushes
  // any pending buffers before the device handle is freed. The
  // [dxmt9-perf] atexit handler then emits the cumulative line.
  device->Release();
  d3d->Release();
  return returnCode;
}

}  // namespace

int main() {
  const auto cfg = readConfig();
  return probeMain(cfg);
}
