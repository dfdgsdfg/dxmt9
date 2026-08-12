#include "device_c_render_tape.hpp"

#include <array>
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
  bool bootstrap(const RenderTapeBootstrapHeader&,
                 std::span<const std::byte>) override {
    ++bootstraps;
    ++events;
    return true;
  }

  bool objectDefine(const RenderTapeObjectDefineHeader&,
                    std::span<const std::byte>) override {
    ++defines;
    ++events;
    return true;
  }

  bool resourceMutation(const RenderTapeResourceMutationHeader& mutation) override {
    ++mutations;
    mutationBytes += mutation.byteSize;
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

  bool objectDestroy(const RenderTapeObjectDestroyHeader&) override {
    ++destroys;
    ++events;
    return true;
  }

  bool orderedControl(const RenderTapeOrderedControlHeader&,
                      std::span<const std::byte>) override {
    ++controls;
    ++events;
    return true;
  }

  bool presentComplete(const RenderTapePresentCompleteHeader& complete,
                       std::span<const std::byte>) override {
    ++completions;
    lastPresentOrdinal = complete.presentOrdinal;
    ++events;
    return true;
  }

  std::uint64_t events = 0u;
  std::uint64_t bootstraps = 0u;
  std::uint64_t defines = 0u;
  std::uint64_t mutations = 0u;
  std::uint64_t mutationBytes = 0u;
  std::uint64_t chunks = 0u;
  std::uint64_t records = 0u;
  std::uint64_t handles = 0u;
  std::uint64_t destroys = 0u;
  std::uint64_t controls = 0u;
  std::uint64_t completions = 0u;
  std::uint64_t lastPresentOrdinal = 0u;
};

void usage() {
  std::cerr << "usage: dxmt9-render-tape <validate|inspect> <tape.bin> "
               "[--verified-blob <sha256>:<bytes>]...\n";
}

bool decodeDigest(std::string_view text, RenderTapeDigest& digest) {
  if (text.size() != digest.size() * 2u) {
    return false;
  }
  auto nibble = [](char value) -> int {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0u; i < digest.size(); ++i) {
    const auto high = nibble(text[i * 2u]);
    const auto low = nibble(text[i * 2u + 1u]);
    if (high < 0 || low < 0) return false;
    digest[i] = static_cast<std::byte>((high << 4) | low);
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 3 || (argc - 3) % 2 != 0) {
    usage();
    return 2;
  }
  const std::string command = argv[1];
  if (command != "validate" && command != "inspect") {
    usage();
    return 2;
  }

  try {
    RenderTapeBlobCatalogue catalogue;
    for (int i = 3; i < argc; i += 2) {
      if (std::string_view(argv[i]) != "--verified-blob") {
        usage();
        return 2;
      }
      const std::string_view value(argv[i + 1]);
      const auto separator = value.find(':');
      RenderTapeBlob blob{};
      if (separator == std::string_view::npos ||
          !decodeDigest(value.substr(0u, separator), blob.digest)) {
        throw std::invalid_argument("invalid verified blob reference");
      }
      const auto sizeText = std::string(value.substr(separator + 1u));
      std::size_t parsed = 0u;
      blob.size = std::stoull(sizeText, &parsed);
      if (parsed != sizeText.size()) {
        throw std::invalid_argument("invalid verified blob byte count");
      }
      blob.verified = 1u;
      catalogue.blobs.push_back(blob);
    }
    const auto bytes = readFile(argv[2]);
    ImportedRenderTapeView tape;
    const auto validation = validateRenderTape(bytes, catalogue, &tape);
    if (!validation.valid()) {
      std::cerr << "render tape invalid status="
                << renderTapeValidationStatusName(validation.status)
                << " event=" << validation.failedEventIndex << " chunk_status="
                << static_cast<std::uint32_t>(validation.chunkStatus) << '\n';
      return 1;
    }
    if (command == "validate") {
      std::cout << "{\"schema\":\"dxmt9.render_tape.v2\","
                   "\"profile\":\"frame-tape\",\"valid\":true,"
                   "\"events\":"
                << tape.header.eventCount
                << ",\"presents\":" << tape.header.presentCount << "}\n";
      return 0;
    }

    InspectSink sink;
    const auto replay = replayPrevalidatedRenderTape(tape, catalogue, sink);
    if (!replay.complete) {
      std::cerr << "render tape inspect replay failed event="
                << replay.failedEventIndex << '\n';
      return 1;
    }
    std::cout << "{\"schema\":\"dxmt9.render_tape.v2\","
                 "\"profile\":\"frame-tape\",\"valid\":true,"
                 "\"events\":"
              << sink.events
              << ",\"bootstraps\":" << sink.bootstraps
              << ",\"defines\":" << sink.defines
              << ",\"mutations\":" << sink.mutations
              << ",\"mutation_bytes\":" << sink.mutationBytes
              << ",\"chunks\":" << sink.chunks
              << ",\"records\":" << sink.records
              << ",\"handles\":" << sink.handles
              << ",\"destroys\":" << sink.destroys
              << ",\"controls\":" << sink.controls
              << ",\"present_completions\":" << sink.completions
              << ",\"last_present_ordinal\":" << sink.lastPresentOrdinal
              << "}\n";
  } catch (const std::exception& error) {
    std::cerr << "dxmt9-render-tape: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
