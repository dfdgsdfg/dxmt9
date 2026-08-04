#include "device_c_common.hpp"
#include "dxmt9/com.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/device_c.h"
#include "dxmt9/dxmt9_device.hpp"
#include "d3d9_pe_wire_handle.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace dxmt9::core;

constexpr DrawBufferBindingSnapshot kAdmittedSnapshot{
    .metalHandle = 0x1234u,
    .contentsAddress = 0x5678u,
    .byteSize = 256u,
    .contentRevision = 9u,
};

struct RecordingDxmt9Device final : dxmt9::Device {
  RecordingDxmt9Device()
      : limits_{}, queue_(WMT::Device{NULL_OBJECT_HANDLE}, limits_) {}

  WMT::Device wmtDevice() override { return WMT::Device{NULL_OBJECT_HANDLE}; }
  dxmt9::CommandQueue& queue() override { return queue_; }
  const BackendLimits& limits() const override { return limits_; }
  std::shared_ptr<BackendDevice> backend() override { return {}; }

  BufferHandle createBuffer(const BufferDesc&) override {
    return BufferHandle{nextHandle++};
  }

  SurfaceHandle createSurface(const SurfaceDesc&) override {
    return SurfaceHandle{nextHandle++};
  }

  ChunkBufferBindingCaptureResult
  markChunkResourcesAndCaptureBufferBindings(
      std::span<const ChunkHandleEntry> entries,
      std::vector<ChunkBufferBindingSnapshot>& snapshots) override {
    snapshots.clear();
    for (const auto& entry : entries) {
      if (entry.kind == ChunkHandleKind::Buffer) {
        snapshots.push_back(ChunkBufferBindingSnapshot{
            .buffer = entry.handle,
            .snapshot = kAdmittedSnapshot,
            .requiresCapturedBacking = true,
        });
      }
    }
    return ChunkBufferBindingCaptureResult::Complete;
  }

  void submitDrawRun(CanonicalDrawState, const DrawUniformPayload&,
                     std::span<const DrawParam> draws,
                     std::span<const DrawParamPayloadView> payloads) override {
    ++drawCalls;
    if (draws.size() != 1u || payloads.size() != 1u) {
      malformedSubmission = true;
      return;
    }
    bindingBytes.assign(payloads.front().bindingSnapshotData.begin(),
                        payloads.front().bindingSnapshotData.end());
  }

  BackendLimits limits_{};
  dxmt9::CommandQueue queue_;
  std::uint64_t nextHandle = 1u;
  std::uint32_t drawCalls = 0u;
  bool malformedSubmission = false;
  std::vector<u8> bindingBytes;
};

std::size_t alignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

template <typename T>
void writeObject(std::vector<std::byte>& bytes, std::size_t offset,
                 const T& value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::vector<std::byte> makeDrawChunk(const D9CWireObjectIdentity& identity) {
  const D9CCommandChunkWireDrawHeaderV2 draw{
      .primitiveType = 4u,
      .primitiveCount = 1u,
      .sectionCount = 1u,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeaderV2),
      .sectionPayloadOffset =
          sizeof(D9CCommandChunkWireDrawHeaderV2) +
          sizeof(D9CCommandChunkWireSectionDescV2),
  };
  const D9CCommandChunkWireSectionDescV2 section{
      .kind = D9C_COMMAND_CHUNK_V2_SECTION_STREAM,
      .elementSize = sizeof(D9CCommandChunkWireStreamBindingV2),
      .count = 1u,
      .payloadOffset = draw.sectionPayloadOffset,
      .byteSize = sizeof(D9CCommandChunkWireStreamBindingV2),
  };
  const D9CCommandChunkWireStreamBindingV2 stream{
      .slot = 0u,
      .valid = 1u,
      .handleIndex = 0u,
      .offset = 12u,
      .stride = 24u,
      .frequency = 1u,
  };
  const std::size_t payloadSize =
      draw.sectionPayloadOffset + sizeof(D9CCommandChunkWireStreamBindingV2);
  const auto recordTableOffset =
      static_cast<std::uint32_t>(sizeof(D9CCommandChunkWireHeaderV2));
  const auto handleTableOffset = static_cast<std::uint32_t>(
      recordTableOffset + sizeof(D9CCommandChunkWireRecordHeaderV2));
  const auto payloadArenaOffset = static_cast<std::uint32_t>(alignUp(
      handleTableOffset + sizeof(D9CCommandChunkWireHandleEntryV2),
      alignof(std::uint32_t)));
  const D9CCommandChunkWireHeaderV2 header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION_V2,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_V2_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_V2_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_V2_SIZE,
      .recordTableOffset = recordTableOffset,
      .recordCount = 1u,
      .handleTableOffset = handleTableOffset,
      .handleCount = 1u,
      .payloadArenaOffset = payloadArenaOffset,
      .payloadArenaSize = static_cast<std::uint32_t>(payloadSize),
  };
  const D9CCommandChunkWireRecordHeaderV2 record{
      .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      .payloadOffset = 0u,
      .payloadSize = static_cast<std::uint32_t>(payloadSize),
      .firstHandle = 0u,
      .handleCount = 1u,
  };
  const D9CCommandChunkWireHandleEntryV2 handle{
      .kind = identity.kind,
      .generation = identity.generation,
      .objectId = identity.objectId,
  };

  std::vector<std::byte> blob(payloadArenaOffset + payloadSize);
  writeObject(blob, 0u, header);
  writeObject(blob, recordTableOffset, record);
  writeObject(blob, handleTableOffset, handle);
  writeObject(blob, payloadArenaOffset, draw);
  writeObject(blob, payloadArenaOffset + draw.sectionTableOffset, section);
  writeObject(blob, payloadArenaOffset + draw.sectionPayloadOffset, stream);
  return blob;
}

bool verifyBindingBytes(const std::vector<u8>& bytes, Handle buffer) {
  if (bytes.size() != sizeof(DrawBindingSnapshot)) {
    return false;
  }
  DrawBindingSnapshot binding{};
  std::memcpy(&binding, bytes.data(), sizeof(binding));
  return binding.streamMask == 1u &&
         binding.streams[0].buffer == buffer &&
         binding.streams[0].offset == 12u &&
         binding.streams[0].stride == 24u &&
         binding.streams[0].snapshot.metalHandle ==
             kAdmittedSnapshot.metalHandle &&
         binding.streams[0].snapshot.contentsAddress ==
             kAdmittedSnapshot.contentsAddress &&
         binding.streams[0].snapshot.byteSize ==
             kAdmittedSnapshot.byteSize &&
         binding.streams[0].snapshot.contentRevision ==
             kAdmittedSnapshot.contentRevision;
}

}  // namespace

int main(int argc, char** argv) {
  std::optional<bool> expectedArgument;
  std::string outputPath;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--expect-off") == 0) {
      expectedArgument = false;
    } else if (std::strcmp(argv[i], "--expect-on") == 0) {
      expectedArgument = true;
    } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
      outputPath = argv[++i];
    } else {
      std::cerr << "unknown or incomplete argument: " << argv[i] << '\n';
      return 1;
    }
  }

  const char* env = std::getenv("DXMT9_OFFLOAD_COMMIT_REPLAY");
  const bool expectedOffload = !(env && std::strcmp(env, "0") == 0);
  if (expectedArgument && *expectedArgument != expectedOffload) {
    std::cerr << "command-line expectation disagrees with environment\n";
    return 1;
  }
  if (dxmt9::d3d9::offloadCommitReplayEnabled() != expectedOffload) {
    std::cerr << "production offload resolver ignored test gate\n";
    return 1;
  }

  auto upper = std::make_unique<RecordingDxmt9Device>();
  auto* recording = upper.get();
  auto* d3d = dxmt9::com::Direct3DCreate9Ex(
      dxmt9::com::D3D_SDK_VERSION, std::move(upper));
  if (!d3d) {
    std::cerr << "failed to create recording factory\n";
    return 1;
  }

  PresentParameters params{};
  params.backBufferWidth = 16u;
  params.backBufferHeight = 16u;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.deviceWindow = Handle{91u};
  params.presentationInterval = PresentInterval::Immediate;
  auto* device = d3d->CreateDeviceEx(0u, params, nullptr);
  if (!device) {
    std::cerr << "failed to create recording device\n";
    d3d->Release();
    return 1;
  }
  device->AddRef();

  bool ok = true;
  std::uint64_t queuedWatermark = 0u;
  std::uint64_t replayedWatermark = 0u;
  {
    D9CDevice cDevice(device);
    auto buffer = device->CreateBuffer(BufferDesc{
        .size = 256u,
        .pool = Pool::Default,
        .usage = UsageDynamic | UsageVertexBuffer,
    });
    D9CBuffer bufferWire(buffer, &cDevice);
    auto blob = makeDrawChunk(bufferWire.wireIdentity);
    D9CCommandChunk chunk{
        .version = D9C_COMMAND_CHUNK_VERSION_V2,
        .recordCount = 1u,
        .recordBytes = static_cast<std::uint32_t>(blob.size()),
        .records = toWireHandle(blob.data()),
        .handleCount = 1u,
    };

    const auto status = dxmt9c_device_commit_chunk(&cDevice, &chunk);
    dxmt9::d3d9::drainDeferredReplay(
        &cDevice, "replay-byte-identity-dispatch");
    ok = status == D3D_OK &&
         static_cast<bool>(cDevice.replayOffload) == expectedOffload &&
         bufferWire.replayDrainTarget->lastQueuedSeq == 1u &&
         bufferWire.replayDrainTarget->lastReplayedSeq == 1u &&
         recording->drawCalls == 1u && !recording->malformedSubmission &&
         verifyBindingBytes(recording->bindingBytes, buffer->handle());
    queuedWatermark = bufferWire.replayDrainTarget->lastQueuedSeq;
    replayedWatermark = bufferWire.replayDrainTarget->lastReplayedSeq;
  }

  const auto artifactBindingBytes = recording->bindingBytes;
  const std::uint32_t artifactDrawCalls = recording->drawCalls;
  const auto deviceRefs = device->Release();
  const auto factoryRefs = d3d->Release();
  if (!ok || deviceRefs != 0u || factoryRefs != 0u) {
    std::cerr << "OFF/ON dispatcher did not preserve admitted binding bytes "
                 "and ledger outcome\n";
    return 1;
  }
  if (!outputPath.empty()) {
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    const std::array<std::uint64_t, 2> watermarks{
        queuedWatermark, replayedWatermark};
    output.write(reinterpret_cast<const char*>(watermarks.data()),
                 sizeof(watermarks));
    output.write(reinterpret_cast<const char*>(&artifactDrawCalls),
                 sizeof(artifactDrawCalls));
    output.write(reinterpret_cast<const char*>(artifactBindingBytes.data()),
                 static_cast<std::streamsize>(artifactBindingBytes.size()));
    if (!output) {
      std::cerr << "failed to write byte-identity artifact\n";
      return 1;
    }
  }
  return 0;
}
