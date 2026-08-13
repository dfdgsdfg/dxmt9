#include "device_c_render_tape_provider.hpp"
#include "device_c_render_tape_capture.hpp"

#include "dxmt9/device_c.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace dxmt9::d3d9;

std::vector<std::byte> readFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    throw std::runtime_error("cannot open file: " + path);
  }
  const auto end = stream.tellg();
  if (end < 0) {
    throw std::runtime_error("cannot size file: " + path);
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(end));
  stream.seekg(0);
  if (!bytes.empty() &&
      !stream.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()))) {
    throw std::runtime_error("cannot read file: " + path);
  }
  return bytes;
}

std::string digestHex(const RenderTapeDigest& digest) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(digest.size() * 2u);
  for (const auto value : digest) {
    const auto byte = std::to_integer<unsigned>(value);
    result.push_back(digits[byte >> 4u]);
    result.push_back(digits[byte & 0xfu]);
  }
  return result;
}

void printResult(const FrameTapeReplayResult& result) {
  const auto& validity = result.validity;
  const auto& coverage = result.coverage;
  const auto& conservation = result.conservation;
  std::cout << "{\"schema\":\"dxmt9.render_tape.provider_replay.v1\",";
  std::cout << "\"profile\":\"frame-tape\",\"status\":\""
            << frameTapeReplayStatusName(result.status) << "\",";
  std::cout << "\"failed_event\":" << result.failedEventIndex << ',';
  std::cout << "\"requirements\":{";
  std::cout << "\"output_width\":" << result.requirements.outputWidth << ',';
  std::cout << "\"output_height\":" << result.requirements.outputHeight << ',';
  std::cout << "\"output_format\":" << result.requirements.outputFormat << "},";
  std::cout << "\"validity\":{";
  std::cout << "\"structurally_valid\":"
            << (validity.structurallyValid ? "true" : "false") << ',';
  std::cout << "\"digests_valid\":"
            << (validity.digestsValid ? "true" : "false") << ',';
  std::cout << "\"output_readback\":"
            << (validity.outputReadback ? "true" : "false") << ',';
  std::cout << "\"expected_digest_captured\":"
            << (validity.expectedDigestCaptured ? "true" : "false") << ',';
  std::cout << "\"expected_digest_matched\":"
            << (validity.expectedDigestMatched ? "true" : "false") << ',';
  std::cout << "\"output_non_degenerate\":"
            << (validity.outputNonDegenerate ? "true" : "false") << ',';
  std::cout << "\"output_bytes\":" << validity.outputBytes << ',';
  std::cout << "\"output_sha256\":\""
            << digestHex(validity.outputDigest) << "\"},";
  std::cout << "\"coverage\":{";
  std::cout << "\"event_count\":" << coverage.eventCount << ',';
  std::cout << "\"object_definitions\":" << coverage.objectDefinitions << ',';
  std::cout << "\"seed_mutations\":" << coverage.seedMutations << ',';
  std::cout << "\"bootstrap_chunks\":" << coverage.bootstrapChunks << ',';
  std::cout << "\"command_chunks\":" << coverage.commandChunks << ',';
  std::cout << "\"command_records\":" << coverage.commandRecords << ',';
  std::cout << "\"clear_records\":" << coverage.clearRecords << ',';
  std::cout << "\"draw_primitive_up_records\":"
            << coverage.drawPrimitiveUpRecords << ',';
  std::cout << "\"present_records\":" << coverage.presentRecords << ',';
  std::cout << "\"present_outputs\":" << coverage.presentOutputs << "},";
  std::cout << "\"conservation\":{";
  std::cout << "\"input_blobs\":" << conservation.inputBlobs << ',';
  std::cout << "\"referenced_blobs\":" << conservation.referencedBlobs << ',';
  std::cout << "\"objects_created\":" << conservation.objectsCreated << ',';
  std::cout << "\"objects_released\":" << conservation.objectsReleased << ',';
  std::cout << "\"present_ordinal\":" << conservation.presentOrdinal << ',';
  std::cout << "\"completion_ordinal\":" << conservation.completionOrdinal << "}}\n";
}

void usage() {
  std::cerr << "usage: dxmt9-render-tape-provider replay <events.bin>"
               " [--blob <path>]...\n";
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 3 || std::string_view(argv[1]) != "replay") {
    usage();
    return 2;
  }

  try {
    std::vector<std::string> blobPaths;
    for (int index = 3; index < argc; ++index) {
      if (std::string_view(argv[index]) != "--blob" || index + 1 >= argc) {
        usage();
        return 2;
      }
      blobPaths.emplace_back(argv[++index]);
    }

    const auto tape = readFile(argv[2]);
    std::vector<std::vector<std::byte>> blobStorage;
    blobStorage.reserve(blobPaths.size());
    std::vector<RenderTapeProviderBlob> blobs;
    blobs.reserve(blobPaths.size());
    for (const auto& path : blobPaths) {
      blobStorage.push_back(readFile(path));
      const auto& bytes = blobStorage.back();
      blobs.push_back(RenderTapeProviderBlob{
          .digest = RenderTapeCaptureSession::sha256(bytes),
          .bytes = bytes,
      });
    }

    const auto preflight = preflightFrameTapeIdentity(tape, blobs);
    if (!preflight.complete()) {
      printResult(preflight);
      return 1;
    }

    auto* factory = dxmt9c_factory_create();
    if (!factory) {
      auto failure = preflight;
      failure.status = FrameTapeReplayStatus::ObjectCreationFailed;
      printResult(failure);
      return 1;
    }
    const D9CPresentParams params{
        .backBufferWidth = preflight.requirements.outputWidth,
        .backBufferHeight = preflight.requirements.outputHeight,
        .backBufferFormat = preflight.requirements.outputFormat,
        .backBufferCount = 1u,
        .swapEffect = 1u,
        .windowed = 1u,
        .presentationInterval = 0x80000000u,
    };
    auto* device = dxmt9c_factory_create_device(
        factory, 0u, &params, 0u, nullptr);
    if (!device) {
      dxmt9c_factory_release(factory);
      auto failure = preflight;
      failure.status = FrameTapeReplayStatus::ObjectCreationFailed;
      printResult(failure);
      return 1;
    }
    const auto result = replayFrameTapeIdentity(device, tape, blobs);
    dxmt9c_device_release(device);
    dxmt9c_factory_release(factory);
    printResult(result);
    return result.complete() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "dxmt9-render-tape-provider: " << error.what() << '\n';
    return 1;
  }
}
