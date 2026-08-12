#include "device_c_render_tape.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace dxmt9::d3d9;

std::vector<std::byte> readFile(const char* path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    throw std::runtime_error(std::string("cannot open tape: ") + path);
  }
  const auto end = stream.tellg();
  if (end < 0) {
    throw std::runtime_error(std::string("cannot size tape: ") + path);
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(end));
  stream.seekg(0);
  if (!bytes.empty() &&
      !stream.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()))) {
    throw std::runtime_error(std::string("cannot read tape: ") + path);
  }
  return bytes;
}

class InspectSink final : public RenderTapeReplaySink {
public:
  bool checkpoint(std::uint32_t,
                  std::span<const D9CWireObjectIdentity> initialObjects,
                  std::span<const std::byte>) override {
    checkpointObjects += initialObjects.size();
    ++events;
    return true;
  }

  bool objectCreate(const RenderTapeObjectCreateHeader&,
                    std::span<const std::byte>) override {
    ++creates;
    ++events;
    return true;
  }

  bool resourceWrite(const RenderTapeResourceWriteHeader&,
                     std::span<const std::byte> data) override {
    ++writes;
    writeBytes += data.size();
    ++events;
    return true;
  }

  bool commandChunk(const CommandChunkEnvelope& envelope,
                    std::span<const std::byte> bytes) override {
    ImportedChunkView chunk;
    if (!validateCommandChunk(bytes, envelope, &chunk).valid()) {
      return false;
    }
    ++chunks;
    records += chunk.records.size();
    handles += chunk.handles.size();
    ++events;
    return true;
  }

  bool objectDestroy(const D9CWireObjectIdentity&) override {
    ++destroys;
    ++events;
    return true;
  }

  bool presentBoundary(const RenderTapePresentBoundary& boundary) override {
    ++presents;
    lastPresentOrdinal = boundary.presentOrdinal;
    ++events;
    return true;
  }

  std::uint64_t events = 0u;
  std::uint64_t checkpointObjects = 0u;
  std::uint64_t creates = 0u;
  std::uint64_t writes = 0u;
  std::uint64_t writeBytes = 0u;
  std::uint64_t chunks = 0u;
  std::uint64_t records = 0u;
  std::uint64_t handles = 0u;
  std::uint64_t destroys = 0u;
  std::uint64_t presents = 0u;
  std::uint64_t lastPresentOrdinal = 0u;
};

void usage() {
  std::cerr << "usage: dxmt9-render-tape <validate|inspect> <tape.bin>\n";
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    usage();
    return 2;
  }
  const std::string command = argv[1];
  if (command != "validate" && command != "inspect") {
    usage();
    return 2;
  }

  try {
    const auto bytes = readFile(argv[2]);
    ImportedRenderTapeView tape;
    const auto validation = validateRenderTape(bytes, &tape);
    if (!validation.valid()) {
      std::cerr << "render tape invalid status="
                << renderTapeValidationStatusName(validation.status)
                << " event=" << validation.failedEventIndex << " chunk_status="
                << static_cast<std::uint32_t>(validation.chunkStatus) << '\n';
      return 1;
    }
    if (command == "validate") {
      std::cout << "{\"schema\":\"dxmt9.render_tape.v1\","
                   "\"profile\":\"frame-tape\",\"valid\":true,"
                   "\"events\":"
                << tape.header.eventCount
                << ",\"presents\":" << tape.header.presentCount << "}\n";
      return 0;
    }

    InspectSink sink;
    const auto replay = replayPrevalidatedRenderTape(tape, sink);
    if (!replay.complete) {
      std::cerr << "render tape inspect replay failed event="
                << replay.failedEventIndex << '\n';
      return 1;
    }
    std::cout << "{\"schema\":\"dxmt9.render_tape.v1\","
                 "\"profile\":\"frame-tape\",\"valid\":true,"
                 "\"events\":"
              << sink.events
              << ",\"checkpoint_objects\":" << sink.checkpointObjects
              << ",\"creates\":" << sink.creates
              << ",\"writes\":" << sink.writes
              << ",\"write_bytes\":" << sink.writeBytes
              << ",\"chunks\":" << sink.chunks
              << ",\"records\":" << sink.records
              << ",\"handles\":" << sink.handles
              << ",\"destroys\":" << sink.destroys
              << ",\"presents\":" << sink.presents
              << ",\"last_present_ordinal\":" << sink.lastPresentOrdinal
              << "}\n";
  } catch (const std::exception& error) {
    std::cerr << "dxmt9-render-tape: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
