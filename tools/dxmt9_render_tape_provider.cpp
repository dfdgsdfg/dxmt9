#include "device_c_render_tape_provider.hpp"
#include "device_c_render_tape_capture.hpp"

#include "dxmt9_perf_counters.hpp"
#include "dxmt9_render_scheduling.hpp"

#include "dxmt9/device_c.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace dxmt9::d3d9;

// A provider replay is an isolated evidence run.  It must not observe or
// mutate the user's shared shader archive: loading it can change the first
// frame, and serializing a newly compiled archive during teardown can race
// the short-lived replay process.  Set both knobs before factory creation so
// device initialization cannot resolve an archive path.
bool forceHermeticReplayEnvironment(const std::string& partitionMode) noexcept {
#if defined(_WIN32)
  return _putenv_s("DXMT_DEBUG_DISABLE_SHADER_ARCHIVE", "1") == 0 &&
         _putenv_s("DXMT9_PREWARM", "disabled") == 0 &&
         _putenv_s("DXMT9_RENDER_PARTITION_MODE", partitionMode.c_str()) == 0 &&
         _putenv_s("DXMT_PERF_COUNTERS", "1") == 0;
#else
  return ::setenv("DXMT_DEBUG_DISABLE_SHADER_ARCHIVE", "1", 1) == 0 &&
         ::setenv("DXMT9_PREWARM", "disabled", 1) == 0 &&
         ::setenv("DXMT9_RENDER_PARTITION_MODE", partitionMode.c_str(), 1) == 0 &&
         ::setenv("DXMT_PERF_COUNTERS", "1", 1) == 0;
#endif
}

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

void printResult(const FrameTapeReplayResult& result,
                 std::string_view requestedPartitionMode,
                 std::string_view resolvedPartitionMode,
                 const dxmt9::perf::RenderTapeParallelJoinSnapshot& counters) {
  const auto& validity = result.validity;
  const auto& coverage = result.coverage;
  const auto& conservation = result.conservation;
  std::cout << "{\"schema\":\"dxmt9.render_tape.provider_replay.v1\",";
  std::cout << "\"archive_policy\":\"disabled\",";
  std::cout << "\"partition_mode\":{\"requested\":\""
            << requestedPartitionMode << "\",\"resolved\":\""
            << resolvedPartitionMode << "\"},";
  std::cout << "\"parallel_counters\":{";
  std::cout << "\"selected\":" << counters.selected << ',';
  std::cout << "\"children\":" << counters.children << ',';
  std::cout << "\"draws\":" << counters.draws << ',';
  std::cout << "\"worker_batches\":" << counters.workerBatches << ',';
  std::cout << "\"worker_tasks\":" << counters.workerTasks << ',';
  std::cout << "\"worker_cpu_ns\":" << counters.workerCpuNs << ',';
  std::cout << "\"worker_wall_ns\":" << counters.workerWallNs << ',';
  std::cout << "\"worker_active_peak\":" << counters.workerActivePeak << ',';
  std::cout << "\"pre_effect_fallbacks\":"
            << counters.preEffectFallbacks << ',';
  std::cout << "\"gpu_command_buffer_errors\":"
            << counters.gpuCommandBufferErrors << "},";
  std::cout << "\"profile\":\"" << renderTapeProfileName(result.profile)
            << "\",\"status\":\""
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
  std::cout << "\"expected_pixels_compared\":"
            << (validity.expectedPixelsCompared ? "true" : "false") << ',';
  std::cout << "\"pixel_envelope_matched\":"
            << (validity.pixelEnvelopeMatched ? "true" : "false") << ',';
  std::cout << "\"output_oracle_matched\":"
            << (validity.expectedDigestMatched || validity.pixelEnvelopeMatched
                    ? "true" : "false")
            << ',';
  std::cout << "\"oracle_mode\":\""
            << (validity.expectedDigestMatched
                    ? "strict"
                    : validity.pixelEnvelopeMatched ? "pixel-envelope"
                                                     : "rejected")
            << "\",";
  std::cout << "\"output_non_degenerate\":"
            << (validity.outputNonDegenerate ? "true" : "false") << ',';
  std::cout << "\"output_bytes\":" << validity.outputBytes << ',';
  std::cout << "\"allowed_differing_pixels\":"
            << validity.allowedDifferingPixels << ',';
  std::cout << "\"differing_pixels\":" << validity.differingPixels << ',';
  std::cout << "\"max_rgb_delta\":" << validity.maxRgbDelta << ',';
  std::cout << "\"total_rgb_delta\":" << validity.totalRgbDelta << ',';
  std::cout << "\"differing_alpha_pixels\":"
            << validity.differingAlphaPixels << ',';
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
  std::cout << "\"draw_primitive_records\":"
            << coverage.drawPrimitiveRecords << ',';
  std::cout << "\"draw_indexed_primitive_records\":"
            << coverage.drawIndexedPrimitiveRecords << ',';
  std::cout << "\"draw_primitive_up_records\":"
            << coverage.drawPrimitiveUpRecords << ',';
  std::cout << "\"state_constant_records\":"
            << coverage.stateConstantRecords << ',';
  std::cout << "\"apply_state_records\":"
            << coverage.applyStateRecords << ',';
  std::cout << "\"present_records\":" << coverage.presentRecords << ',';
  std::cout << "\"present_source_mappings\":"
            << coverage.presentSourceMappings << ',';
  std::cout << "\"present_outputs\":" << coverage.presentOutputs << ',';
  std::cout << "\"object_destroys\":" << coverage.objectDestroys << "},";
  std::cout << "\"conservation\":{";
  std::cout << "\"input_blobs\":" << conservation.inputBlobs << ',';
  std::cout << "\"referenced_blobs\":" << conservation.referencedBlobs << ',';
  std::cout << "\"objects_created\":" << conservation.objectsCreated << ',';
  std::cout << "\"objects_released\":" << conservation.objectsReleased << ',';
  std::cout << "\"present_ordinal\":" << conservation.presentOrdinal << ',';
  std::cout << "\"completion_ordinal\":" << conservation.completionOrdinal
            << "},\"intervals\":[";
  for (std::uint32_t i = 0u; i < result.intervalCount; ++i) {
    if (i != 0u) std::cout << ',';
    const auto& interval = result.intervals[i];
    std::cout << "{\"present_ordinal\":" << interval.presentOrdinal << ',';
    std::cout << "\"completion_ordinal\":" << interval.completionOrdinal << ',';
    std::cout << "\"validity\":{";
    std::cout << "\"output_readback\":"
              << (interval.validity.outputReadback ? "true" : "false") << ',';
    std::cout << "\"expected_digest_captured\":"
              << (interval.validity.expectedDigestCaptured ? "true" : "false")
              << ',';
    std::cout << "\"expected_digest_matched\":"
              << (interval.validity.expectedDigestMatched ? "true" : "false")
              << ',';
    std::cout << "\"expected_pixels_compared\":"
              << (interval.validity.expectedPixelsCompared ? "true" : "false")
              << ',';
    std::cout << "\"pixel_envelope_matched\":"
              << (interval.validity.pixelEnvelopeMatched ? "true" : "false")
              << ',';
    std::cout << "\"output_oracle_matched\":"
              << (interval.validity.expectedDigestMatched ||
                          interval.validity.pixelEnvelopeMatched
                      ? "true" : "false")
              << ',';
    std::cout << "\"oracle_mode\":\""
              << (interval.validity.expectedDigestMatched
                      ? "strict"
                      : interval.validity.pixelEnvelopeMatched
                            ? "pixel-envelope"
                            : "rejected")
              << "\",";
    std::cout << "\"output_non_degenerate\":"
              << (interval.validity.outputNonDegenerate ? "true" : "false")
              << ',';
    std::cout << "\"output_bytes\":" << interval.validity.outputBytes << ',';
    std::cout << "\"output_sha256\":\""
              << digestHex(interval.validity.outputDigest) << "\"}}";
  }
  std::cout << "]}\n";
  std::cout.flush();
}

void usage() {
  std::cerr << "usage: dxmt9-render-tape-provider replay <events.bin>"
               " [--blob <path>]... [--expected-rgba <path>]"
               " [--output-rgba <path>]"
               " [--partition-mode identity|serial|parallel]\n";
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 3 || std::string_view(argv[1]) != "replay") {
    usage();
    return 2;
  }

  try {
    std::vector<std::string> blobPaths;
    std::string expectedPath;
    std::string outputPath;
    std::string partitionMode = "identity";
    bool partitionModeSpecified = false;
    for (int index = 3; index < argc; ++index) {
      const std::string_view option = argv[index];
      if (option == "--partition-mode") {
        if (partitionModeSpecified || index + 1 >= argc) {
          usage();
          return 2;
        }
        partitionMode = argv[++index];
        if (partitionMode != "identity" && partitionMode != "serial" &&
            partitionMode != "parallel") {
          usage();
          return 2;
        }
        partitionModeSpecified = true;
      } else if (option == "--blob") {
        if (index + 1 >= argc) {
          usage();
          return 2;
        }
        blobPaths.emplace_back(argv[++index]);
      } else if (option == "--expected-rgba" && expectedPath.empty()) {
        if (index + 1 >= argc) {
          usage();
          return 2;
        }
        expectedPath = argv[++index];
      } else if (option == "--output-rgba" && outputPath.empty()) {
        if (index + 1 >= argc) {
          usage();
          return 2;
        }
        outputPath = argv[++index];
      } else {
        usage();
        return 2;
      }
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

    const auto preflight = preflightRenderTapeIdentity(tape, blobs);
    const auto partitionConfig =
        dxmt9::render::resolveRenderPartitionConfig(partitionMode.c_str());
    const std::string_view resolvedPartitionMode =
        dxmt9::render::partitionModeName(partitionConfig.resolved);
    if (!preflight.complete()) {
      printResult(preflight, partitionMode, resolvedPartitionMode,
                  dxmt9::perf::RenderTapeParallelJoinSnapshot{});
      return 1;
    }

    if (!forceHermeticReplayEnvironment(partitionMode)) {
      throw std::runtime_error(
          "cannot establish hermetic shader-archive environment");
    }

    auto* factory = dxmt9c_factory_create();
    if (!factory) {
      auto failure = preflight;
      failure.status = FrameTapeReplayStatus::ObjectCreationFailed;
      printResult(failure, partitionMode, resolvedPartitionMode,
                  dxmt9::perf::RenderTapeParallelJoinSnapshot{});
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
      printResult(failure, partitionMode, resolvedPartitionMode,
                  dxmt9::perf::RenderTapeParallelJoinSnapshot{});
      return 1;
    }
    auto result = replayRenderTapeIdentity(device, tape, blobs);
    dxmt9c_device_release(device);
    dxmt9c_factory_release(factory);
    const auto parallelCounters = dxmt9::perf::snapshotRenderTapeParallelJoin();
    if (!expectedPath.empty()) {
      const auto expectedPixels = readFile(expectedPath);
      (void)applyRenderTapePixelOracleEnvelope(result, expectedPixels);
    }
    if (!outputPath.empty() && !result.outputPixels.empty()) {
      std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
      if (!output ||
          !output.write(reinterpret_cast<const char*>(result.outputPixels.data()),
                        static_cast<std::streamsize>(result.outputPixels.size()))) {
        throw std::runtime_error("cannot write replay output: " + outputPath);
      }
    }
    printResult(result, partitionMode, resolvedPartitionMode, parallelCounters);
    return result.complete() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "dxmt9-render-tape-provider: " << error.what() << '\n';
    return 1;
  }
}
