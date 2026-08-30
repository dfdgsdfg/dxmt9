// Concrete production oracle for the ordinary Direct ChunkSlot route.
//
// Each child process creates the real dxmt9::Device after selecting one
// DXMT9_DIRECT_CHUNK_SLOT_REPLAY value, replays the same ColorFill chunk, and
// reads the resulting offscreen texture back from Metal. The Direct child also
// runs a separate forced-commit probe: the existing production test seam can
// fire only from commitDirectChunkSlotReplay, so a fail-stop result proves the
// ordinary route was entered before the uninstrumented pixel run.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "device_c_common.hpp"
#include "device_c_chunk_replay.hpp"
#include "device_c_replay_offload.hpp"
#include "dxmt9/com.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/dxmt9_command_queue.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace dxmt9 {

// This access type is already a declared friend of CommandQueue. Keep this
// test-local and value-only: it reads the existing native commit-failure seam
// and the queue watermarks needed to compare command/resource/completion
// identity; it does not add a production observer or alter the hot path.
struct CommandQueueArenaLeaseTestAccess {
  static void forceNextDirectChunkSlotCommitFailure(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlyForceNextDirectChunkSlotCommitFailure_ = true;
  }

  static bool stopped(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.stop_;
  }

  static std::uint64_t lastCommittedSeqId(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.lastCommittedSeqId_;
  }

  static std::uint64_t completedSeqId(CommandQueue& queue) {
    return queue.completedSeqIdAcquire();
  }

  static std::size_t writingCommandCount(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    if (!queue.writingSlot_ || *queue.writingSlot_ >= queue.slots_.size()) {
      return 0u;
    }
    return queue.slots_[*queue.writingSlot_].commandCount();
  }
};

}  // namespace dxmt9

namespace {

constexpr std::uint32_t kWidth = 16u;
constexpr std::uint32_t kHeight = 16u;
constexpr std::uint32_t kColorArgb = 0xff123456u;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

std::size_t alignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

struct WireFixture {
  std::vector<std::byte> bytes;
  dxmt9::d3d9::CommandChunkEnvelope envelope{};
};

void retainSurface(std::uint32_t kind, void* object) noexcept {
  if (kind == D9C_CHUNK_HANDLE_KIND_SURFACE && object) {
    dxmt9c_surface_addref(static_cast<D9CSurface*>(object));
  }
}

WireFixture makeColorFillChunk(const D9CWireObjectIdentity& identity) {
  const D9CCommandChunkWireColorFill fixed{
      .surfaceHandleIndex = 0u,
      .colorARGB = kColorArgb,
      .hasRect = 0u,
      .reserved0 = 0u,
      .rect = {.left = 0, .top = 0,
               .right = static_cast<std::int32_t>(kWidth),
               .bottom = static_cast<std::int32_t>(kHeight)},
  };
  const D9CCommandChunkWireRecordHeader record{
      .type = D9C_COMMAND_RECORD_COLOR_FILL,
      .flags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
      .payloadOffset = 0u,
      .payloadSize = sizeof(fixed),
      .firstHandle = 0u,
      .handleCount = 1u,
  };
  const D9CCommandChunkWireHandleEntry handle =
      dxmt9::d3d9::wireHandleEntry(identity);
  D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordCount = 1u,
      .handleCount = 1u,
      .payloadArenaSize = sizeof(fixed),
  };
  header.handleTableOffset = static_cast<std::uint32_t>(alignUp(
      header.recordTableOffset + sizeof(record),
      alignof(D9CCommandChunkWireHandleEntry)));
  header.payloadArenaOffset = static_cast<std::uint32_t>(alignUp(
      header.handleTableOffset + sizeof(handle), alignof(std::uint32_t)));

  WireFixture result;
  result.bytes.resize(header.payloadArenaOffset + sizeof(fixed));
  std::memcpy(result.bytes.data(), &header, sizeof(header));
  std::memcpy(result.bytes.data() + header.recordTableOffset, &record,
              sizeof(record));
  std::memcpy(result.bytes.data() + header.handleTableOffset, &handle,
              sizeof(handle));
  std::memcpy(result.bytes.data() + header.payloadArenaOffset, &fixed,
              sizeof(fixed));
  result.envelope = {
      .version = D9C_COMMAND_CHUNK_VERSION,
      .recordCount = 1u,
      .handleCount = 1u,
  };
  return result;
}

dxmt9::d3d9::RawCommandChunk makeRaw(const WireFixture& fixture,
                                     dxmt9::d3d9::WireObjectRegistry& registry,
                                     D9CSurface& surface) {
  dxmt9::d3d9::RawCommandChunk raw;
  check(dxmt9::d3d9::prepareOffloadChunk(
            fixture.bytes, fixture.envelope, registry, retainSurface, raw),
        "production ColorFill raw chunk passes owned preflight");
  raw.replaySeq = 1u;
  raw.cpuReadyTapePlanningEnabled = false;
  raw.resourceEntries.push_back({
      .kind = dxmt9::core::ChunkHandleKind::Surface,
      .handle = surface.obj->handle(),
  });
  return raw;
}

struct ProductionFixture {
  explicit ProductionFixture(WMT::Device metalDevice) {
    auto upper = dxmt9::CreateDXMT9Device(
        dxmt9::DEVICE_DESC{.device = metalDevice});
    check(upper != nullptr, "production Metal device constructs");
    upperRaw = upper.get();
    factory = dxmt9::com::Direct3DCreate9Ex(
        dxmt9::com::D3D_SDK_VERSION, std::move(upper));
    check(factory != nullptr, "production core factory constructs");

    dxmt9::core::PresentParameters params{};
    params.backBufferWidth = kWidth;
    params.backBufferHeight = kHeight;
    params.backBufferFormat = dxmt9::core::Format::A8R8G8B8;
    params.windowed = true;
    params.presentationInterval = dxmt9::core::PresentInterval::Immediate;
    device = factory->CreateDeviceEx(0u, params, nullptr);
    check(device != nullptr, "production core device constructs");
    device->AddRef();
    cDevice = std::make_unique<D9CDevice>(device);
    surface = dxmt9c_device_create_render_target(
        cDevice.get(), kWidth, kHeight, 21u, 0u, 0u, 0u, nullptr);
    check(surface != nullptr && surface->obj,
          "production offscreen render target constructs");
  }

  ~ProductionFixture() {
    if (surface) {
      dxmt9c_surface_release(surface);
      surface = nullptr;
    }
    cDevice.reset();
    if (device) {
      (void)device->Release();
    }
    if (factory) {
      (void)factory->Release();
    }
  }

  dxmt9::Device* upperRaw = nullptr;
  dxmt9::com::IDirect3D9Ex* factory = nullptr;
  dxmt9::com::IDirect3DDevice9Ex* device = nullptr;
  std::unique_ptr<D9CDevice> cDevice;
  D9CSurface* surface = nullptr;
};

bool directSelection() {
  return dxmt9::resolveDirectChunkSlotReplayEnabled(
      std::getenv("DXMT9_DIRECT_CHUNK_SLOT_REPLAY"),
      /*traceRender=*/false);
}

void directCommitProbe(WMT::Device metalDevice) {
  ProductionFixture fixture(metalDevice);
  check(fixture.upperRaw->supportsDirectChunkSlotReplay(),
        "env=1 production device advertises Direct ChunkSlot replay");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_SURFACE,
                                        fixture.surface);
  const auto wire = makeColorFillChunk(identity);
  auto raw = makeRaw(wire, registry, *fixture.surface);
  dxmt9::CommandQueueArenaLeaseTestAccess::forceNextDirectChunkSlotCommitFailure(
      fixture.upperRaw->queue());
  const auto hr = dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw);
  check(hr != dxmt9::core::D3D_OK &&
            dxmt9::CommandQueueArenaLeaseTestAccess::stopped(
                fixture.upperRaw->queue()),
        "forced commit seam proves the production Direct route reached its commit");
  dxmt9::d3d9::releaseRetainedWrappers(raw);
}

struct ChildResult {
  bool direct = false;
  bool directCommitProved = false;
  std::uint64_t commandCount = 0;
  std::uint64_t resourceHandle = 0;
  std::uint64_t resourceSeq = 0;
  std::uint64_t committedSeq = 0;
  std::uint64_t completedSeq = 0;
  std::vector<std::byte> pixels;
};

ChildResult runChild(bool expectedDirect) {
  setenv("DXMT9_DIRECT_CHUNK_SLOT_REPLAY", expectedDirect ? "1" : "0", 1);
  unsetenv("DXMT_TRACE_RENDER");
  check(directSelection() == expectedDirect,
        "production resolver selects the requested explicit env mode");

  @autoreleasepool {
    auto devices = WMT::CopyAllDevices();
    if (!devices || devices.count() == 0u) {
      std::cout << "SKIP\n";
      std::exit(77);
    }
    WMT::Device metalDevice = devices.object(0u);
    if (expectedDirect) {
      directCommitProbe(metalDevice);
    }

    ProductionFixture fixture(metalDevice);
    check(fixture.upperRaw->supportsDirectChunkSlotReplay() == expectedDirect,
          "real production device capability matches selected env mode");
    dxmt9::d3d9::WireObjectRegistry registry;
    const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_SURFACE,
                                          fixture.surface);
    const auto wire = makeColorFillChunk(identity);
    auto raw = makeRaw(wire, registry, *fixture.surface);
    check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) ==
              dxmt9::core::D3D_OK,
          "unforced production ColorFill replay succeeds");
    const auto& queue = fixture.upperRaw->queue();
    const auto commandCount =
        dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
            const_cast<dxmt9::CommandQueue&>(queue));
    check(commandCount == 1u,
          "same ColorFill semantic input creates one production command");
    fixture.upperRaw->flush();

    dxmt9::core::ReadbackPixels readback;
    check(fixture.upperRaw->readbackSurface(
              dxmt9::core::ReadbackDesc{.source = fixture.surface->obj->handle()},
              readback),
          "production Metal offscreen readback succeeds");
    const std::size_t expectedBytes =
        static_cast<std::size_t>(kWidth) * kHeight * 4u;
    check(readback.pitch >= kWidth * 4u &&
              readback.bytes.size() >=
                  static_cast<std::size_t>(readback.pitch) * kHeight,
          "readback has a bounded row span");
    ChildResult result;
    result.direct = expectedDirect;
    result.directCommitProved = expectedDirect;
    result.commandCount = commandCount;
    result.resourceHandle = fixture.surface->obj->handle().value;
    const auto* surfaceRecord = fixture.upperRaw->pool()->findSurface(
        fixture.surface->obj->handle().value);
    check(surfaceRecord != nullptr, "production surface resource remains present");
    result.resourceSeq = surfaceRecord->lastUsedSeqId;
    result.committedSeq =
        dxmt9::CommandQueueArenaLeaseTestAccess::lastCommittedSeqId(
            const_cast<dxmt9::CommandQueue&>(queue));
    result.completedSeq =
        dxmt9::CommandQueueArenaLeaseTestAccess::completedSeqId(
            const_cast<dxmt9::CommandQueue&>(queue));
    result.pixels.resize(expectedBytes);
    for (std::uint32_t row = 0u; row < kHeight; ++row) {
      std::memcpy(result.pixels.data() +
                      static_cast<std::size_t>(row) * kWidth * 4u,
                  readback.bytes.data() +
                      static_cast<std::size_t>(row) * readback.pitch,
                  kWidth * 4u);
    }
    for (std::size_t pixel = 0u; pixel < result.pixels.size(); pixel += 4u) {
      if (static_cast<std::uint8_t>(result.pixels[pixel + 0u]) != 0x56u ||
          static_cast<std::uint8_t>(result.pixels[pixel + 1u]) != 0x34u ||
          static_cast<std::uint8_t>(result.pixels[pixel + 2u]) != 0x12u ||
          static_cast<std::uint8_t>(result.pixels[pixel + 3u]) != 0xffu) {
        std::cerr << "pixel " << pixel / 4u << " = "
                  << static_cast<unsigned>(static_cast<std::uint8_t>(
                         result.pixels[pixel + 0u]))
                  << ','
                  << static_cast<unsigned>(static_cast<std::uint8_t>(
                         result.pixels[pixel + 1u]))
                  << ','
                  << static_cast<unsigned>(static_cast<std::uint8_t>(
                         result.pixels[pixel + 2u]))
                  << ','
                  << static_cast<unsigned>(static_cast<std::uint8_t>(
                         result.pixels[pixel + 3u]))
                  << '\n';
        throw TestFailure("Metal readback pixels match the ColorFill semantic input");
      }
    }
    dxmt9::d3d9::releaseRetainedWrappers(raw);
    return result;
  }
}

std::string serialize(const ChildResult& result) {
  std::ostringstream out;
  out << (result.direct ? 1 : 0) << ' ' << (result.directCommitProved ? 1 : 0)
      << ' ' << result.commandCount << ' ' << result.resourceHandle << ' '
      << result.resourceSeq << ' ' << result.committedSeq << ' '
      << result.completedSeq << ' ';
  for (const auto byte : result.pixels) {
    out << static_cast<unsigned>(static_cast<std::uint8_t>(byte)) << ',';
  }
  return out.str();
}

ChildResult parseResult(std::string_view text) {
  check(text.rfind("SKIP\n", 0) != 0u,
        "child should not be parsed as a Metal skip result");
  std::istringstream in{std::string(text)};
  ChildResult result;
  int direct = 0;
  int proved = 0;
  check(static_cast<bool>(in >> direct >> proved >> result.commandCount >>
                          result.resourceHandle >> result.resourceSeq >>
                          result.committedSeq >> result.completedSeq),
        "child emits command/resource/completion identity");
  result.direct = direct != 0;
  result.directCommitProved = proved != 0;
  std::string pixelText;
  in >> pixelText;
  std::size_t begin = 0u;
  while (begin < pixelText.size()) {
    const auto end = pixelText.find(',', begin);
    check(end != std::string::npos, "child pixel serialization is bounded");
    result.pixels.push_back(static_cast<std::byte>(std::stoul(
        pixelText.substr(begin, end - begin))));
    begin = end + 1u;
  }
  return result;
}

std::string runMode(const char* self, const char* mode) {
  int pipefd[2] = {-1, -1};
  check(pipe(pipefd) == 0, "create child result pipe");
  const pid_t child = fork();
  check(child >= 0, "fork child mode");
  if (child == 0) {
    close(pipefd[0]);
    check(dup2(pipefd[1], STDOUT_FILENO) >= 0,
          "redirect child result stdout");
    close(pipefd[1]);
    execl(self, self, "--child", mode, static_cast<char*>(nullptr));
    _exit(127);
  }
  close(pipefd[1]);
  std::string output;
  std::array<char, 4096> buffer{};
  for (;;) {
    const ssize_t count = read(pipefd[0], buffer.data(), buffer.size());
    if (count > 0) {
      output.append(buffer.data(), static_cast<std::size_t>(count));
      continue;
    }
    check(count == 0 || errno == EINTR, "read child result");
    if (count == 0) break;
  }
  close(pipefd[0]);
  int status = 0;
  check(waitpid(child, &status, 0) == child, "wait for child mode");
  check(WIFEXITED(status), "child mode exits normally");
  const int exitCode = WEXITSTATUS(status);
  if (exitCode == 77) {
    return "SKIP\n";
  }
  check(exitCode == 0, "child mode succeeds");
  return output;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--child") {
      const bool direct = std::string_view(argv[2]) == "1";
      std::cout << serialize(runChild(direct)) << '\n';
      return 0;
    }
    check(argc >= 1, "test executable has a path");
    check(dxmt9::resolveDirectChunkSlotReplayEnabled("0", false) == false &&
              dxmt9::resolveDirectChunkSlotReplayEnabled("1", false) == true,
          "explicit Direct ChunkSlot resolver values remain distinct");
    const auto legacyText = runMode(argv[0], "0");
    if (legacyText == "SKIP\n") return 77;
    const auto directText = runMode(argv[0], "1");
    if (directText == "SKIP\n") return 77;
    const auto legacy = parseResult(legacyText);
    const auto direct = parseResult(directText);
    check(!legacy.direct && !legacy.directCommitProved,
          "env=0 child remains on the Legacy route");
    check(direct.direct && direct.directCommitProved,
          "env=1 child proves the ordinary Direct route before readback");
    check(legacy.commandCount == direct.commandCount &&
              legacy.resourceHandle == direct.resourceHandle &&
              legacy.resourceSeq == direct.resourceSeq &&
              legacy.committedSeq == direct.committedSeq &&
              legacy.completedSeq == direct.completedSeq,
          "Direct/Legacy preserve command, resource, and completion identity");
    check(legacy.pixels == direct.pixels,
          "Direct/Legacy exact offscreen Metal readback pixels match");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
