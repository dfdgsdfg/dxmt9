#include "device_c_render_tape_provider.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_identity.hpp"

#include "dxmt9_perf_counters.hpp"
#include "dxmt9_render_scheduling.hpp"

#include "dxmt9/device_c.h"

#include <algorithm>
#include <cstddef>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

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

std::vector<std::byte> readFile(const std::string& path,
                              std::size_t maxBytes = std::numeric_limits<std::size_t>::max()) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    throw std::runtime_error("cannot open file: " + path);
  }
  const auto end = stream.tellg();
  if (end < 0) {
    throw std::runtime_error("cannot size file: " + path);
  }
  if (static_cast<std::uintmax_t>(end) > maxBytes)
    throw std::runtime_error("file exceeds byte limit: " + path);
  std::vector<std::byte> bytes(static_cast<std::size_t>(end));
  stream.seekg(0);
  if (!bytes.empty() &&
      !stream.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()))) {
    throw std::runtime_error("cannot read file: " + path);
  }
  return bytes;
}

std::filesystem::path normalizedPath(const std::filesystem::path& path) {
  std::error_code error;
  const auto canonical = std::filesystem::weakly_canonical(path, error);
  if (!error) return canonical;
  error.clear();
  const auto absolute = std::filesystem::absolute(path, error);
  if (error) return path.lexically_normal();
  return absolute.lexically_normal();
}

void validateReadbackDirectory(const std::string& requested,
                               std::span<const std::string> inputPaths) {
  if (requested.empty()) return;
  const std::filesystem::path directory(requested);
  if (directory.empty()) {
    throw std::runtime_error("readback directory must not be empty");
  }
  std::error_code error;
  const auto state = std::filesystem::symlink_status(directory, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("cannot inspect readback directory: " + requested);
  }
  if (state.type() != std::filesystem::file_type::not_found) {
    throw std::runtime_error("readback directory already exists: " + requested);
  }
  const auto normalizedDirectory = normalizedPath(directory);
  for (const auto& input : inputPaths) {
    if (input.empty()) continue;
    if (normalizedDirectory == normalizedPath(input)) {
      throw std::runtime_error("readback directory aliases an input: " + input);
    }
  }
}

struct ReadbackArtifactEvidence {
  std::string path;
  std::size_t bytes = 0u;
  RenderTapeDigest digest{};
};

ReadbackArtifactEvidence writeReadbackArtifact(
    const std::filesystem::path& directory, unsigned iteration,
    std::string_view kind, std::span<const std::byte> bytes) {
  const auto filename = "iteration-" + std::string(6u -
      std::to_string(iteration).size(), '0') + std::to_string(iteration) +
      "-" + std::string(kind) + ".rgba";
  const auto path = directory / filename;
  std::error_code error;
  const auto state = std::filesystem::symlink_status(path, error);
  if ((state.type() != std::filesystem::file_type::not_found) ||
      (error && error != std::errc::no_such_file_or_directory)) {
    throw std::runtime_error("readback artifact already exists: " +
                             path.string());
  }
  std::ofstream output(path, std::ios::binary);
  if (!output ||
      (!bytes.empty() && !output.write(
          reinterpret_cast<const char*>(bytes.data()),
          static_cast<std::streamsize>(bytes.size())))) {
    throw std::runtime_error("cannot write readback artifact: " + path.string());
  }
  output.flush();
  if (!output) {
    throw std::runtime_error("cannot flush readback artifact: " + path.string());
  }
  return ReadbackArtifactEvidence{
      .path = filename,
      .bytes = bytes.size(),
      .digest = RenderTapeCaptureSession::sha256(bytes),
  };
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

RenderTapeDigest parseDigestHex(std::string_view text) {
  if (text.size() != kRenderTapeDigestSize * 2u)
    throw std::runtime_error("source plan digest must be exactly 64 lowercase hexadecimal characters");
  RenderTapeDigest digest{};
  const auto nibble = [](char value) -> unsigned {
    if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
    if (value >= 'a' && value <= 'f') return static_cast<unsigned>(value - 'a') + 10u;
    return 16u;
  };
  for (std::size_t index = 0u; index < digest.size(); ++index) {
    const auto high = nibble(text[index * 2u]);
    const auto low = nibble(text[index * 2u + 1u]);
    if (high > 15u || low > 15u)
      throw std::runtime_error("source plan digest must be exactly 64 lowercase hexadecimal characters");
    digest[index] = static_cast<std::byte>((high << 4u) | low);
  }
  if (std::all_of(digest.begin(), digest.end(), [](std::byte value) {
        return value == std::byte{};
      }))
    throw std::runtime_error("source plan digest must be nonzero");
  return digest;
}

const char* identityAuthorityName(std::uint32_t authority) noexcept {
  switch (static_cast<RenderTapeIdentityAuthority>(authority)) {
  case RenderTapeIdentityAuthority::Capture: return "capture";
  case RenderTapeIdentityAuthority::ProviderReplay: return "provider-replay";
  case RenderTapeIdentityAuthority::DerivedProjection:
    return "derived-projection";
  }
  return "rejected";
}

RenderTapeBlobCatalogue makeIdentityCatalogue(
    std::span<const RenderTapeProviderBlob> blobs) {
  RenderTapeBlobCatalogue catalogue;
  catalogue.blobs.reserve(blobs.size());
  for (const auto& blob : blobs) {
    catalogue.blobs.push_back(RenderTapeBlob{
        .digest = blob.digest,
        .size = blob.bytes.size(),
        .verified = 1u,
    });
  }
  return catalogue;
}

void printQueryReplay(const RenderTapeQueryReplayEvidence& evidence) {
  const auto boolean = [](bool value) { return value ? "true" : "false"; };
  const auto bytesHex = [](const auto& bytes, std::uint32_t count) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (std::size_t index = 0u; index < std::min<std::size_t>(count, bytes.size()); ++index) {
      const auto byte = std::to_integer<unsigned>(bytes[index]);
      result.push_back(digits[byte >> 4u]);
      result.push_back(digits[byte & 0xfu]);
    }
    return result;
  };
  constexpr std::array<const char*, 9> failures{
      "none", "unsupported-result-kind", "snapshot-unavailable", "cycle-mismatch",
      "poll-failed", "result-mismatch", "progress-exhausted", "backend-unavailable", "unknown"};
  const auto failure = static_cast<std::size_t>(evidence.failure);
  constexpr std::array<const char*, 4> progressLimits{
      "none", "poll-budget", "wall-budget", "unknown"};
  const auto progressLimit = static_cast<std::size_t>(evidence.progressLimit);
  std::cout << "{\"schema\":\"dxmt9.render_tape.query_replay.v1\","
               "\"backend\":\"metal-real\",\"mode\":\"qualification\",\"present\":"
            << boolean(evidence.present)
            << ",\"transcript_complete\":" << boolean(evidence.transcriptComplete)
            << ",\"oracle_complete\":" << boolean(evidence.oracleComplete)
            << ",\"actual_metal\":" << boolean(evidence.actualMetal)
            << ",\"gpu_result_oracle\":" << boolean(evidence.gpuResultOracle)
            << ",\"query_definitions\":" << evidence.queryDefinitions
            << ",\"issue_records\":" << evidence.issueRecords
            << ",\"replayed_issue_records\":" << evidence.replayedIssueRecords
            << ",\"recorded_polls\":" << evidence.recordedPolls
            << ",\"replayed_polls\":" << evidence.replayedPolls
            << ",\"captured_pending\":" << evidence.capturedPending
            << ",\"replay_pending\":" << evidence.replayPending
            << ",\"captured_ready\":" << evidence.capturedReady
            << ",\"replay_ready\":" << evidence.replayReady
            << ",\"required_results\":" << evidence.requiredResults
            << ",\"matched_results\":" << evidence.matchedResults
            << ",\"readiness_required\":" << evidence.readinessRequired
            << ",\"readiness_satisfied\":" << evidence.readinessSatisfied
            << ",\"substituted_results\":" << evidence.substitutedResults
            << ",\"supplemental_polls\":" << evidence.supplementalPolls
            << ",\"supplemental_drains\":" << evidence.supplementalDrains
            << ",\"checkpoint_definitions\":" << evidence.checkpointDefinitions
            << ",\"checkpoint_setup_ends\":" << evidence.checkpointSetupEnds
            << ",\"checkpoint_setup_publications\":" << evidence.checkpointSetupPublications
            << ",\"checkpoint_setup_polls\":" << evidence.checkpointSetupPolls
            << ",\"checkpoint_setup_waits\":" << evidence.checkpointSetupWaits
            << ",\"checkpoint_completed\":" << evidence.checkpointCompleted
            << ",\"checkpoint_activated\":" << evidence.checkpointActivated
            << ",\"checkpoint_setup_wall_ns\":" << evidence.checkpointSetupWallNanos
            << ",\"transcript_wall_ns\":" << evidence.transcriptWallNanos
            << ",\"oracle_wall_ns\":" << evidence.oracleWallNanos
            << ",\"max_oracle_polls\":" << evidence.maxOraclePolls
            << ",\"max_oracle_wall_ns\":" << evidence.maxOracleWallNanos
            << ",\"progress_limit\":\""
            << progressLimits[std::min(progressLimit, progressLimits.size() - 1u)] << '"'
            << ",\"failure\":\"" << failures[std::min(failure, failures.size() - 1u)]
            << "\",\"rows\":[";
  for (std::size_t index = 0u; index < evidence.rows.size(); ++index) {
    if (index) std::cout << ',';
    const auto& row = evidence.rows[index];
    std::cout << "{\"event_index\":" << row.eventIndex
              << ",\"event_ordinal\":" << row.eventOrdinal
              << ",\"identity\":{\"kind\":" << row.identity.kind
              << ",\"generation\":" << row.identity.generation
              << ",\"object_id\":\"" << row.identity.objectId << "\"}"
              << ",\"issue_cycle\":" << row.issueCycle
              << ",\"flags\":" << row.flags
              << ",\"has_output\":" << boolean(row.hasOutput)
              << ",\"data_size\":" << row.dataSize
              << ",\"captured_result\":" << row.capturedResult
              << ",\"replay_result\":" << row.replayResult
              << ",\"oracle_result\":" << row.oracleResult
              << ",\"defined_bytes\":" << row.definedBytes
              << ",\"captured_hex\":\"" << bytesHex(row.capturedBytes, row.definedBytes)
              << "\",\"replay_hex\":\"" << bytesHex(row.replayBytes, row.definedBytes) << '"'
              << ",\"local_query_identity\":" << row.localQueryIdentity
              << ",\"local_issue_ordinal\":" << row.localIssueOrdinal
              << ",\"local_device_epoch\":" << row.localDeviceEpoch
              << ",\"local_queue\":" << row.localQueue
              << ",\"local_sequence\":" << row.localSequence
              << ",\"local_initializer\":" << row.localInitializer
              << ",\"completed_sequence\":" << row.completedSequence
              << ",\"completed_initializer\":" << row.completedInitializer
              << ",\"oracle_polls\":" << row.oraclePolls
              << ",\"result_required\":" << boolean(row.resultRequired)
              << ",\"result_matched\":" << boolean(row.resultMatched)
              << ",\"readiness_satisfied\":" << boolean(row.readinessSatisfied)
              << ",\"result_substituted\":" << boolean(row.resultSubstituted)
              << ",\"supplemental\":" << boolean(row.supplemental) << '}';
  }
  std::cout << "],\"checkpoints\":[";
  for (std::size_t index = 0u; index < evidence.checkpoints.size(); ++index) {
    if (index) std::cout << ',';
    const auto& row = evidence.checkpoints[index];
    std::cout << "{\"identity\":{\"kind\":" << row.identity.kind
              << ",\"generation\":" << row.identity.generation
              << ",\"object_id\":\"" << row.identity.objectId << "\"}"
              << ",\"captured_issue_cycle\":" << row.capturedIssueCycle
              << ",\"captured_issue_ordinal\":" << row.capturedIssueOrdinal
              << ",\"captured_device_epoch\":" << row.capturedDeviceEpoch
              << ",\"local_query_identity\":" << row.localQueryIdentity
              << ",\"local_issue_ordinal\":" << row.localIssueOrdinal
              << ",\"local_device_epoch\":" << row.localDeviceEpoch
              << ",\"local_queue\":" << row.localQueue
              << ",\"local_sequence\":" << row.localSequence
              << ",\"local_initializer\":" << row.localInitializer
              << ",\"completed_sequence\":" << row.completedSequence
              << ",\"completed_initializer\":" << row.completedInitializer
              << ",\"setup_polls\":" << row.setupPolls
              << ",\"setup_issued\":" << boolean(row.setupIssued)
              << ",\"completed\":" << boolean(row.completed)
              << ",\"activated\":" << boolean(row.activated) << '}';
  }
  std::cout << "]}";
}

void printResult(const FrameTapeReplayResult& result,
                 std::string_view requestedPartitionMode,
                 std::string_view resolvedPartitionMode,
                 const dxmt9::perf::RenderTapeParallelJoinSnapshot& counters,
                 bool sourceOracleRequested = false,
                 bool sourceOracleMatched = false,
                 bool sourceOracleExactMatched = false,
                 const RenderTapeIdentityView* identity = nullptr,
                 std::string_view identityError = {}) {
  const auto& validity = result.validity;
  const auto& coverage = result.coverage;
  const auto& conservation = result.conservation;
  std::cout << "{\"schema\":\"dxmt9.render_tape.provider_replay.v2\",";
  std::cout << "\"query_replay\":";
  printQueryReplay(result.queryReplay);
  std::cout << ',';
  std::cout << "\"archive_policy\":\"disabled\",";
  std::cout << "\"partition_mode\":{\"requested\":\""
            << requestedPartitionMode << "\",\"resolved\":\""
            << resolvedPartitionMode << "\"},";
  std::cout << "\"identity_evidence\":{";
  if (identity) {
    const auto authority = static_cast<RenderTapeIdentityAuthority>(
        identity->header.authority);
    const bool capture = authority == RenderTapeIdentityAuthority::Capture;
    const bool derived = authority ==
        RenderTapeIdentityAuthority::DerivedProjection;
    const bool providerReplay = authority ==
        RenderTapeIdentityAuthority::ProviderReplay;
    const bool queueSettled = capture && !identity->settlements.empty();
    // ProviderReplay rows describe an untrusted input sidecar, not this
    // process's queue completion.  Keep provenance visible, but expose no
    // completion or settlement evidence without an invocation-bound nonce.
    const std::size_t evidenceSettlementCount =
        providerReplay ? 0u : identity->settlements.size();
    const std::uint32_t evidenceTableCount =
        providerReplay ? 0u : identity->header.settlementCount;
    std::cout << "\"authority\":\""
              << identityAuthorityName(identity->header.authority)
              << "\",\"source_count\":" << identity->sources.size()
              << ",\"segment_count\":" << identity->sources.size()
              << ",\"provenance_segment_count\":" << identity->sources.size()
              << ",\"completed_segment_count\":"
              << (queueSettled ? identity->sources.size() : 0u)
              << ",\"completion_evidence\":\""
              << (capture ? (queueSettled ? "queue-tail-fenced" : "none")
                          : derived ? "not-queue-authenticated"
                                    : "process-local-unverified")
              << "\""
              << ",\"settlement_count\":"
              << evidenceSettlementCount << ",\"settlement_table_count\":"
              << evidenceTableCount << ",\"segments\":[";
    for (std::size_t index = 0; index < identity->sources.size(); ++index) {
      if (index != 0u) std::cout << ',';
      const auto& source = identity->sources[index];
      std::cout << "{\"segment_index\":" << index
                << ",\"event_ordinal\":" << source.eventOrdinal
                << ",\"source_ordinal\":" << source.sourceOrdinal
                << ",\"seq_id\":" << source.seqId
                << ",\"first_record\":" << source.firstRecord
                << ",\"record_count\":" << source.recordCount << "}";
    }
    std::cout << "],\"event_settlement_table\":[";
    if (capture) for (std::size_t index = 0; index < identity->settlements.size(); ++index) {
      if (index != 0u) std::cout << ',';
      const auto& settlement = identity->settlements[index];
      std::cout << "{\"event_ordinal\":" << settlement.eventOrdinal
                << ",\"raw_ordinal\":" << settlement.rawOrdinal
                << ",\"build_generation\":" << settlement.buildGeneration
                << ",\"first_source_ordinal\":"
                << settlement.firstSourceOrdinal << ",\"tail_seq_id\":"
                << settlement.tailSeqId << ",\"source_count\":"
                << settlement.sourceCount << "}";
    }
    std::cout << "],\"derived_settlement_table\":[";
    if (derived) for (std::size_t index = 0; index < identity->settlements.size(); ++index) {
      if (index != 0u) std::cout << ',';
      const auto& settlement = identity->settlements[index];
      std::cout << "{\"event_ordinal\":" << settlement.eventOrdinal
                << ",\"raw_ordinal\":" << settlement.rawOrdinal
                << ",\"build_generation\":" << settlement.buildGeneration
                << ",\"first_source_ordinal\":"
                << settlement.firstSourceOrdinal << ",\"tail_seq_id\":"
                << settlement.tailSeqId << ",\"source_count\":"
                << settlement.sourceCount << "}";
    }
    std::cout << "],\"final_event_settlement\":";
    if (!capture || identity->settlements.empty()) {
      std::cout << "null";
    } else {
      const auto& settlement = identity->settlements.back();
      std::cout << "{\"event_ordinal\":" << settlement.eventOrdinal
                << ",\"tail_seq_id\":" << settlement.tailSeqId
                << ",\"source_count\":" << settlement.sourceCount << "}";
    }
    std::cout << ",\"derived_sidecar\":" << (derived ? "true" : "false");
  } else {
    std::cout << "\"authority\":\"rejected\",\"source_count\":0"
                 ",\"segment_count\":0,\"provenance_segment_count\":0"
                 ",\"completed_segment_count\":0"
                 ",\"completion_evidence\":\"none\""
                 ",\"settlement_count\":0,\"settlement_table_count\":0"
                 ",\"segments\":[],\"event_settlement_table\":[]"
                 ",\"derived_settlement_table\":[]"
                 ",\"final_event_settlement\":null,\"derived_sidecar\":false";
  }
  std::cout << ",\"rejected\":" << (identityError.empty() ? "false" : "true")
            << ",\"rejection_reason\":\"" << identityError << "\"},";
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
            << digestHex(validity.outputDigest) << "\",";
  std::cout << "\"source_readback\":"
            << (validity.sourceReadback ? "true" : "false") << ',';
  std::cout << "\"source_bytes\":" << validity.sourceBytes << ',';
  std::cout << "\"source_sha256\":\""
            << digestHex(validity.sourceDigest) << "\",";
  std::cout << "\"source_oracle_requested\":"
            << (sourceOracleRequested ? "true" : "false") << ',';
  std::cout << "\"source_oracle_exact_matched\":"
            << (sourceOracleExactMatched ? "true" : "false") << ',';
  std::cout << "\"source_oracle_matched\":"
            << (sourceOracleMatched ? "true" : "false") << ',';
  std::cout << "\"source_oracle_mode\":\""
            << (!sourceOracleRequested
                    ? "not-requested"
                    : sourceOracleExactMatched
                        ? "strict"
                        : sourceOracleMatched ? "pixel-envelope-equivalent"
                                              : "rejected")
            << "\",";
  std::cout << "\"oracle_failure\":\""
            << (sourceOracleRequested && !sourceOracleMatched
                    ? "source-mismatch"
                    : (!validity.expectedDigestMatched &&
                       !validity.pixelEnvelopeMatched
                           ? "output-only-mismatch"
                           : "none"))
            << "\"},";
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
            << "}";
  const auto& audit = result.initialSeedAudit;
  const bool showInitialSeedAudit = result.prepared.preflightCompleted ||
                                    audit.requested;
  if (showInitialSeedAudit) {
    std::cout << ",\"initial_seed_audit\":{";
    std::cout << "\"requested\":" << (audit.requested ? "true" : "false")
              << ",\"diagnostic\":true,\"perturbative\":true"
              << ",\"attempted\":" << (audit.attempted ? "true" : "false")
              << ",\"complete\":" << (audit.complete ? "true" : "false")
              << ",\"count\":" << audit.count
              << ",\"matched\":" << audit.matched
              << ",\"supported_count\":" << audit.supportedCount
              << ",\"mismatched_count\":" << audit.mismatchedCount
              << ",\"unsupported_count\":" << audit.unsupportedCount
              << ",\"capture_failed_count\":" << audit.captureFailedCount
              << ",\"wall_ns\":" << audit.wallNanos << ",\"entries\":[";
    for (std::size_t i = 0u; i < audit.entries.size(); ++i) {
      if (i != 0u) std::cout << ',';
      const auto& entry = audit.entries[i];
      const char* kind = entry.kind == RenderTapeInitialSeedAuditKind::Depth
                             ? "depth"
                             : "color";
      const char* failure = "unknown";
      switch (entry.failure) {
      case RenderTapeInitialSeedAuditFailure::None: failure = "none"; break;
      case RenderTapeInitialSeedAuditFailure::Unsupported:
        failure = "unsupported"; break;
      case RenderTapeInitialSeedAuditFailure::BlobUnavailable:
        failure = "blob-unavailable"; break;
      case RenderTapeInitialSeedAuditFailure::ObjectUnavailable:
        failure = "object-unavailable"; break;
      case RenderTapeInitialSeedAuditFailure::CaptureFailed:
        failure = "capture-failed"; break;
      case RenderTapeInitialSeedAuditFailure::DigestMismatch:
        failure = "digest-mismatch"; break;
      }
      std::cout << "{\"kind\":\"" << kind << "\",\"identity\":{";
      std::cout << "\"kind\":" << entry.identity.kind
                << ",\"generation\":" << entry.identity.generation
                << ",\"object_id\":" << entry.identity.objectId << "}"
                << ",\"subresource\":" << entry.subresource
                << ",\"expected_sha256\":\""
                << digestHex(entry.expectedDigest)
                << "\",\"actual_sha256\":\""
                << digestHex(entry.actualDigest)
                << "\",\"bytes\":" << entry.bytes
                << ",\"supported\":"
                << (entry.supported ? "true" : "false")
                << ",\"match\":" << (entry.match ? "true" : "false")
                << ",\"failure\":\"" << failure << "\"}";
    }
    std::cout << "]}";
  }
  const auto& freshSeedDrainAudit = result.freshSeedDrainAudit;
  const bool showFreshSeedDrainAudit = result.prepared.preflightCompleted ||
                                       freshSeedDrainAudit.requested;
  if (showFreshSeedDrainAudit) {
    std::cout << ",\"fresh_seed_drain_audit\":{"
                 "\"scope\":\"resource-mutation-preflush\",";
    std::cout << "\"requested\":"
              << (freshSeedDrainAudit.requested ? "true" : "false")
              << ",\"attempted\":"
              << (freshSeedDrainAudit.attempted ? "true" : "false")
              << ",\"diagnostic\":true"
              << ",\"complete\":"
              << (freshSeedDrainAudit.complete ? "true" : "false")
              << ",\"mutation_count\":"
              << freshSeedDrainAudit.mutationCount
              << ",\"inspected_mutations\":"
              << freshSeedDrainAudit.inspectedMutations
              << ",\"required_drains\":"
              << freshSeedDrainAudit.requiredDrains
              << ",\"bypassed_drains\":"
              << freshSeedDrainAudit.bypassedDrains << '}';
  }
  const auto& prestage = result.texturePrestageAudit;
  if (result.prepared.preflightCompleted || prestage.requested) {
    std::cout << ",\"texture_prestage_audit\":{\"diagnostic\":true"
                 ",\"object_scope\":\"provider-owned-object-wrappers\""
              << ",\"requested\":" << (prestage.requested ? "true" : "false")
              << ",\"attempted\":" << (prestage.attempted ? "true" : "false")
              << ",\"complete\":" << (prestage.complete ? "true" : "false")
              << ",\"candidate_objects\":" << prestage.candidateObjects
              << ",\"candidate_subresources\":" << prestage.candidateSubresources
              << ",\"candidate_bytes\":" << prestage.candidateBytes
              << ",\"immediate_upload_subresources\":"
              << prestage.immediateUploadSubresources
              << ",\"deferred_upload_subresources\":"
              << prestage.deferredUploadSubresources
              << ",\"initializer_flushes\":" << prestage.initializerFlushes
              << ",\"initializer_waits\":" << prestage.initializerWaits
              << ",\"initializer_upload_value\":" << prestage.initializerUploadValue
              << ",\"activated_objects\":" << prestage.activatedObjects
              << ",\"consumed_seeds\":" << prestage.consumedSeeds
              << ",\"staged_objects_remaining\":" << prestage.stagedObjectsRemaining
              << ",\"staged_subresources_remaining\":" << prestage.stagedSubresourcesRemaining
              << ",\"staged_bytes_remaining\":" << prestage.stagedBytesRemaining
              << ",\"hidden_objects_created\":" << prestage.hiddenObjectsCreated
              << ",\"hidden_objects_released\":" << prestage.hiddenObjectsReleased
              << ",\"active_objects_created\":" << prestage.activeObjectsCreated
              << ",\"active_objects_released\":" << prestage.activeObjectsReleased
              << ",\"physical_objects_created\":" << prestage.physicalObjectsCreated
              << ",\"physical_objects_released\":" << prestage.physicalObjectsReleased
              << ",\"physical_conservation\":"
              << (prestage.physicalConservation ? "true" : "false") << '}';
  }
  const auto& sourceBoundaryAudit = result.sourceBoundaryAudit;
  std::cout << ",\"source_boundary_audit\":{";
  std::cout << "\"requested\":"
            << (sourceBoundaryAudit.requested ? "true" : "false")
            << ",\"diagnostic\":true"
            << ",\"validated\":"
            << (sourceBoundaryAudit.validated ? "true" : "false")
            << ",\"complete\":"
            << (sourceBoundaryAudit.complete ? "true" : "false")
            << ",\"source_count\":" << sourceBoundaryAudit.sourceCount
            << ",\"completed_source_count\":"
            << sourceBoundaryAudit.completedSourceCount
            << ",\"batch_cut_count\":" << sourceBoundaryAudit.batchCutCount
            << ",\"completed_batch_cut_count\":"
            << sourceBoundaryAudit.completedBatchCutCount << '}';
  const auto& sourceReplay = result.sourceReplay;
  const auto& inputAudit = result.textureInputAudit;
  std::cout << ",\"texture_input_audit\":{\"requested\":"
            << (inputAudit.requested ? "true" : "false")
            << ",\"diagnostic\":true,\"validated\":" << (inputAudit.validated ? "true" : "false")
            << ",\"attempted\":" << (inputAudit.attempted ? "true" : "false")
            << ",\"uploaded\":" << (inputAudit.uploaded ? "true" : "false")
            << ",\"readback_matched\":" << (inputAudit.readbackMatched ? "true" : "false")
            << ",\"kind\":" << inputAudit.identity.kind
            << ",\"generation\":" << inputAudit.identity.generation
            << ",\"object_id\":" << inputAudit.identity.objectId
            << ",\"after_captured_seq\":" << inputAudit.afterCapturedSeqId
            << ",\"bytes\":" << inputAudit.byteCount
            << ",\"input_sha256\":\"" << digestHex(inputAudit.inputDigest)
            << "\",\"before_sha256\":\"" << digestHex(inputAudit.beforeDigest)
            << "\",\"after_sha256\":\"" << digestHex(inputAudit.afterDigest) << "\"}";
  std::cout << ",\"source_replay\":{\"requested\":"
            << (sourceReplay.requested ? "true" : "false")
            << ",\"validated\":" << (sourceReplay.validated ? "true" : "false")
            << ",\"complete\":" << (sourceReplay.complete ? "true" : "false")
            << ",\"plan_sha256\":\"" << digestHex(sourceReplay.planDigest) << '"'
            << ",\"source_count\":" << sourceReplay.sourceCount
            << ",\"completed_source_count\":" << sourceReplay.completedSourceCount
            << ",\"publication_count\":" << sourceReplay.publicationCount
            << ",\"empty_source_count\":" << sourceReplay.emptySourceCount
            << ",\"present_source_count\":" << sourceReplay.presentSourceCount
            << ",\"storage_overflow\":"
            << (sourceReplay.storageOverflow ? "true" : "false")
            << ",\"sources\":[";
  for (std::size_t sourceIndex = 0u;
       sourceIndex < sourceReplay.sources.size(); ++sourceIndex) {
    if (sourceIndex != 0u) std::cout << ',';
    const auto& source = sourceReplay.sources[sourceIndex];
    const char* settlement = "empty";
    if (source.settlement == RenderTapeSourceReplaySettlement::Published)
      settlement = "published";
    else if (source.settlement ==
             RenderTapeSourceReplaySettlement::PresentPublished)
      settlement = "present-published";
    std::cout << "{\"event_ordinal\":" << source.eventOrdinal
              << ",\"source_ordinal\":" << source.sourceOrdinal
              << ",\"captured_seq_id\":" << source.capturedSeqId
              << ",\"command_chunk_index\":" << source.commandChunkIndex
              << ",\"first_record\":" << source.firstRecord
              << ",\"record_count\":" << source.recordCount
              << ",\"local_raw_ordinal\":" << source.actualRawOrdinal
              << ",\"range_index\":" << source.rangeIndex
              << ",\"settled\":" << (source.settled ? "true" : "false")
              << ",\"settlement\":\"" << settlement << "\",\"publications\":[";
    const auto first = static_cast<std::size_t>(source.publicationOffset);
    const auto last = first + source.publicationCount;
    for (std::size_t publicationIndex = first; publicationIndex < last;
         ++publicationIndex) {
      if (publicationIndex != first) std::cout << ',';
      const auto& publication = sourceReplay.publications[publicationIndex];
      std::cout << "{\"source_index\":" << publication.sourceIdIndex
                << ",\"source_generation\":"
                << publication.sourceIdGeneration
                << ",\"storage_first_page\":"
                << publication.storageFirstPage
                << ",\"storage_page_count\":"
                << publication.storagePageCount
                << ",\"storage_generation\":"
                << publication.storageGeneration
                << ",\"raw_ordinal\":" << publication.metadataRawOrdinal
                << ",\"source_ordinal\":"
                << publication.metadataSourceOrdinal
                << ",\"seq_id\":" << publication.metadataSeqId
                << ",\"build_generation\":"
                << publication.metadataBuildGeneration
                << ",\"used_bytes\":" << publication.metadataUsedBytes
                << ",\"page_count\":" << publication.metadataPageCount
                << ",\"padding_pages_before\":"
                << publication.metadataPaddingPagesBefore
                << ",\"strict_admission\":"
                << (publication.metadataStrictAdmission ? "true" : "false")
                << ",\"control_index\":" << publication.controlIndex
                << ",\"has_present\":"
                << (publication.hasPresent ? "true" : "false") << '}';
    }
    std::cout << "]}";
  }
  std::cout << "]}";
  std::cout << ",\"intervals\":[";
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
              << digestHex(interval.validity.outputDigest) << "\",";
    std::cout << "\"source_readback\":"
              << (interval.validity.sourceReadback ? "true" : "false") << ',';
    std::cout << "\"source_bytes\":" << interval.validity.sourceBytes << ',';
    std::cout << "\"source_sha256\":\""
              << digestHex(interval.validity.sourceDigest) << "\"}}";
  }
  std::cout << "]}\n";
  std::cout.flush();
}

void usage() {
  std::cerr << "usage: dxmt9-render-tape-provider replay <events.bin>"
               " [--blob <path>]... [--expected-rgba <path>]"
               " [--expected-source-rgba <path>]"
               " [--output-rgba <path>]"
               " [--output-source-rgba <path>]"
               " [--partition-mode identity|serial|parallel]"
               " [--identity <identity-v2.bin>]\n";
  std::cerr << "       dxmt9-render-tape-provider experiment <events.bin>"
               " [replay options] [--warmup N] [--repeat N]"
               " [--reset cold-device|warm-device]"
               " [--replay-route compatibility|production-slot]"
               " [--source-boundary-mode chunk|capture-source]"
               " [--source-plan-sha256 <64-lowercase-hex>]"
               " [--readback-dir <new-directory>]\n";
  std::cerr << "               [--audit-initial-seeds]"
               " [--audit-fresh-seed-drains] [--audit-texture-prestage]"
               " [--audit-source-boundaries]"
               " [--audit-texture-input GEN:ID:AFTERSEQ:SHA256:PATH]"
               " [--audit-batch-cut EVENT:RECORD]...\n";
}

using Clock = std::chrono::steady_clock;

std::uint64_t nanosSince(Clock::time_point begin) noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
}

unsigned parseCount(std::string_view value) {
  unsigned result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result > 64)
    throw std::runtime_error("iteration count must be an integer in [0,64]");
  return result;
}

RenderTapeBatchCut parseBatchCut(std::string_view value) {
  const auto separator = value.find(':');
  if (separator == std::string_view::npos || separator == 0u ||
      separator + 1u >= value.size() ||
      value.find(':', separator + 1u) != std::string_view::npos) {
    throw std::runtime_error(
        "batch cut must be specified as EVENT:RECORD with unsigned decimal values");
  }
  RenderTapeBatchCut result{};
  const auto event = value.substr(0u, separator);
  const auto record = value.substr(separator + 1u);
  for (const auto digit : event) {
    if (digit < '0' || digit > '9') {
      throw std::runtime_error(
          "batch cut must be specified as EVENT:RECORD with unsigned decimal values");
    }
  }
  for (const auto digit : record) {
    if (digit < '0' || digit > '9') {
      throw std::runtime_error(
          "batch cut must be specified as EVENT:RECORD with unsigned decimal values");
    }
  }
  const auto eventParsed = std::from_chars(
      event.data(), event.data() + event.size(), result.eventOrdinal);
  const auto recordParsed = std::from_chars(
      record.data(), record.data() + record.size(), result.beforeRecord);
  if (eventParsed.ec != std::errc{} ||
      eventParsed.ptr != event.data() + event.size() ||
      recordParsed.ec != std::errc{} ||
      recordParsed.ptr != record.data() + record.size()) {
    throw std::runtime_error(
        "batch cut must be specified as EVENT:RECORD with unsigned decimal values");
  }
  return result;
}

RenderTapeTextureInputAuditRequest parseTextureInputAudit(
    std::string_view value, std::string& path) {
  std::array<std::string_view, 4> fields;
  for (auto& field : fields) {
    const auto separator = value.find(':');
    if (separator == std::string_view::npos || separator == 0u)
      throw std::runtime_error("texture input audit requires GEN:ID:AFTERSEQ:SHA256:PATH");
    field = value.substr(0u, separator);
    value.remove_prefix(separator + 1u);
  }
  const auto number = [](std::string_view field) {
    if (field.empty() || field.front() == '0')
      throw std::runtime_error("texture input target requires canonical positive decimal identities");
    std::uint64_t out = 0u;
    const auto parsed = std::from_chars(field.data(), field.data() + field.size(), out);
    if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size() || out == 0u)
      throw std::runtime_error("texture input target requires positive decimal identities");
    return out;
  };
  const auto generation = number(fields[0]);
  if (generation > UINT32_MAX || value.empty())
    throw std::runtime_error("invalid texture input generation/path");
  path = value;
  return {.identity = {D9C_CHUNK_HANDLE_KIND_TEXTURE,
                       static_cast<std::uint32_t>(generation), number(fields[1])},
          .afterCapturedSeqId = number(fields[2]), .digest = parseDigestHex(fields[3])};
}

void printLoadedImages() {
  std::cout << '[';
#if defined(__APPLE__)
  bool first = true;
  for (std::uint32_t i = 0; i < _dyld_image_count(); ++i) {
    const auto* name = _dyld_get_image_name(i);
    if (!name) continue;
    const auto base = std::filesystem::path(name).filename().string();
    if (base.find("dxmt9") == std::string::npos &&
        base.find("winemetal") == std::string::npos) continue;
    const auto bytes = readFile(name);
    if (!first) std::cout << ',';
    first = false;
    std::cout << "{\"path\":" << std::quoted(name)
              << ",\"sha256\":\"" << digestHex(RenderTapeCaptureSession::sha256(bytes))
              << "\",\"bytes\":" << bytes.size() << '}';
  }
#endif
  std::cout << ']';
}

void printPathDelta(const dxmt9::perf::RenderTapeReplaySnapshot& before,
                    const dxmt9::perf::RenderTapeReplaySnapshot& after) {
  std::cout << "{\"direct_leases\":" << after.directLeases - before.directLeases
            << ",\"direct_lease_draws\":" << after.directLeaseDraws - before.directLeaseDraws
            << ",\"compatibility_batches\":" << after.compatibilityBatches - before.compatibilityBatches
            << ",\"compatibility_draws\":" << after.compatibilityDraws - before.compatibilityDraws
            << ",\"compatibility_unbatched_draws\":" << after.compatibilityUnbatchedDraws - before.compatibilityUnbatchedDraws
            << ",\"fallback_draws\":" << after.fallbackDraws - before.fallbackDraws
            << ",\"soa_growth_after_reserve\":" << after.soaGrowthAfterReserve - before.soaGrowthAfterReserve
            << ",\"encode_chunks\":" << after.encodeChunks - before.encodeChunks
            << ",\"gpu_errors\":" << after.gpuErrors - before.gpuErrors
            << ",\"dispositions\":[";
  for (std::size_t i = 0; i < before.dispositions.size(); ++i) {
    if (i) std::cout << ',';
    std::cout << after.dispositions[i] - before.dispositions[i];
  }
  std::cout << "],\"outcomes\":[";
  for (std::size_t i = 0; i < before.outcomes.size(); ++i) {
    if (i) std::cout << ',';
    std::cout << after.outcomes[i] - before.outcomes[i];
  }
  std::cout << "]}";
}

const char* observedReplayPath(const dxmt9::perf::RenderTapeReplaySnapshot& before,
                               const dxmt9::perf::RenderTapeReplaySnapshot& after) noexcept {
  const bool direct = after.directLeaseDraws > before.directLeaseDraws;
  const bool compatibility = after.compatibilityDraws > before.compatibilityDraws;
  return direct ? (compatibility ? "mixed" : "direct-slot")
                : (compatibility ? "compatibility" : "no-qualified-draw-path");
}

int runExperiment(const PreparedRenderTape& prepared, unsigned warmup,
                  unsigned repeat, const std::string& reset,
                  const std::string& route, const std::string& partitionMode,
                  const std::string& expectedPath,
                  const std::string& expectedSourcePath,
                  const std::string& readbackDir,
                  bool auditInitialSeeds,
                  bool auditFreshSeedDrains,
                  bool auditTexturePrestage,
                  bool auditSourceBoundaries,
                  RenderTapeSourceBoundaryMode sourceBoundaryMode,
                  RenderTapeDigest expectedSourcePlanDigest,
                  std::span<const std::byte> sourceBoundaryIdentity,
                  std::span<const RenderTapeBatchCut> auditBatchCuts,
                  const RenderTapeTextureInputAuditRequest* textureInputAudit,
                  const RenderTapeIdentityView* identity) {
  const bool expectedSourceDigestProvided = std::any_of(
      expectedSourcePlanDigest.begin(), expectedSourcePlanDigest.end(),
      [](std::byte value) { return value != std::byte{}; });
  const auto expected = expectedPath.empty() ? std::vector<std::byte>{} : readFile(expectedPath);
  const auto expectedSource = expectedSourcePath.empty() ? std::vector<std::byte>{} : readFile(expectedSourcePath);
  if (!forceHermeticReplayEnvironment(partitionMode))
    throw std::runtime_error("cannot establish hermetic experiment environment");
  const std::filesystem::path readbackRoot(readbackDir);
  if (!readbackDir.empty()) {
    std::error_code error;
    if (!std::filesystem::create_directory(readbackRoot, error) || error) {
      throw std::runtime_error("cannot create readback directory: " +
                               readbackDir);
    }
  }
  const auto& preflight = prepared.preflightResult();
  const D9CPresentParams params{
      .backBufferWidth = preflight.requirements.outputWidth,
      .backBufferHeight = preflight.requirements.outputHeight,
      .backBufferFormat = preflight.requirements.outputFormat,
      .backBufferCount = 1u, .swapEffect = 1u, .windowed = 1u,
      .presentationInterval = 0x80000000u,
  };
  auto factoryDeleter = [](D9CFactory* value) { if (value) dxmt9c_factory_release(value); };
  auto deviceDeleter = [](D9CDevice* value) { if (value) dxmt9c_device_release(value); };
  std::unique_ptr<D9CFactory, decltype(factoryDeleter)> factory(nullptr, factoryDeleter);
  std::unique_ptr<D9CDevice, decltype(deviceDeleter)> device(nullptr, deviceDeleter);
  std::array<RenderTapeIntervalEvidence, kRenderTapeMaxReplayIntervals> reference{};
  std::uint32_t referenceCount = 0;
  bool deterministic = true;
  bool complete = prepared.valid();
  std::cout << "{\"schema\":\"dxmt9.render_tape.experiment.v1\",\"backend\":\"metal\""
            << ",\"timing_scope\":{\"setup_ns\":\"factory-device-create\","
               "\"replay_ns\":\"resource-setup-replay-encode-wait-readback-cleanup\","
               "\"reset_ns\":\"explicit-reset-or-device-teardown\","
               "\"service_ns\":\"prepared-api-inclusive\"}"
            << ",\"readback_artifacts_enabled\":"
            << (readbackDir.empty() ? "false" : "true")
            << ",\"completion\":\"real-gpu\",\"reset\":\"" << reset
            << "\",\"replay_route\":\"" << route
            << "\",\"audit_initial_seeds\":"
            << (auditInitialSeeds ? "true" : "false")
            << ",\"audit_fresh_seed_drains\":"
            << (auditFreshSeedDrains ? "true" : "false")
            << ",\"audit_texture_prestage\":"
            << (auditTexturePrestage ? "true" : "false")
            << ",\"audit_source_boundaries\":"
            << (auditSourceBoundaries ? "true" : "false")
            << ",\"audit_texture_input\":" << (textureInputAudit ? "true" : "false")
            << ",\"source_boundary_mode\":\""
            << (sourceBoundaryMode == RenderTapeSourceBoundaryMode::CaptureSource
                    ? "capture-source" : "chunk") << '"'
            << ",\"source_plan_sha256\":\""
            << (expectedSourceDigestProvided ? digestHex(expectedSourcePlanDigest)
                                              : std::string{}) << '"'
            << ",\"audit_batch_cuts\":[";
  for (std::size_t index = 0u; index < auditBatchCuts.size(); ++index) {
    if (index != 0u) std::cout << ',';
    std::cout << "{\"event_ordinal\":" << auditBatchCuts[index].eventOrdinal
              << ",\"before_record\":"
              << auditBatchCuts[index].beforeRecord << '}';
  }
  std::cout << ']'
            << ",\"prepare_count\":" << prepared.prepareCount()
            << ",\"prepare_ns\":" << prepared.evidence().timing.prepareWallNanos
            << ",\"source_identity_domain\":\"provider-local-unqualified\""
            << ",\"loaded_images\":";
  printLoadedImages();
  std::cout << ",\"iterations\":[";
  for (unsigned i = 0; complete && i < warmup + repeat; ++i) {
    const auto setupBegin = Clock::now();
    if (!factory) factory.reset(dxmt9c_factory_create());
    if (factory && !device)
      device.reset(dxmt9c_factory_create_device(factory.get(), 0u, &params, 0u, nullptr));
    const auto setupNs = nanosSince(setupBegin);
    auto result = preflight;
    const auto before = dxmt9::perf::snapshotRenderTapeReplay();
    const auto replayBegin = Clock::now();
    if (!device) {
      result.status = FrameTapeReplayStatus::ObjectCreationFailed;
    } else {
      result = replayPreparedRenderTapeIdentity(device.get(), prepared, {
          .route = route == "production-slot" ? RenderTapePreparedRoute::ProductionSlot
                                               : RenderTapePreparedRoute::Compatibility,
          .reset = reset == "warm-device" ? RenderTapePreparedReset::WarmDevice
                                           : RenderTapePreparedReset::FreshDevice,
          .auditInitialSeeds = auditInitialSeeds,
          .auditFreshSeedDrains = auditFreshSeedDrains,
          .auditTexturePrestage = auditTexturePrestage,
          .auditSourceBoundaries = auditSourceBoundaries,
          .sourceBoundaryMode = sourceBoundaryMode,
          .sourceBoundaryIdentity = sourceBoundaryIdentity,
          .expectedSourcePlanDigest = expectedSourcePlanDigest,
          .hasExpectedSourcePlanDigest = expectedSourceDigestProvided,
          .auditBatchCuts = auditBatchCuts,
          .auditTextureInput = textureInputAudit,
      });
    }
    const auto serviceNs = nanosSince(replayBegin);
    const auto replayNs = result.prepared.timing.replayWallNanos;
    if (!expectedPath.empty()) (void)applyRenderTapePixelOracleEnvelope(result, expected);
    const bool sourceExact = !expectedSourcePath.empty() && result.validity.sourceReadback &&
                             expectedSource == result.sourcePixels;
    const bool sourceMatches = expectedSourcePath.empty() ||
        (result.validity.sourceReadback && renderTapeSourceOracleMatchesOutputEvidence(
            expectedSource, result.sourcePixels, expected, result.outputPixels,
            result.validity.pixelEnvelopeMatched));
    if (i == 0) {
      reference = result.intervals;
      referenceCount = result.intervalCount;
    } else {
      deterministic &= referenceCount == result.intervalCount;
      for (std::uint32_t interval = 0; interval < result.intervalCount; ++interval)
        deterministic &= reference[interval].validity.outputDigest == result.intervals[interval].validity.outputDigest &&
                         reference[interval].validity.sourceDigest == result.intervals[interval].validity.sourceDigest;
    }
    const auto resetBegin = Clock::now();
    // Warm reset is performed by the prepared provider's explicit contract;
    // cold-device mode additionally destroys every device/compilation cache.
    if (reset == "cold-device") {
      device.reset();
      factory.reset();
    }
    const auto resetNs = nanosSince(resetBegin) + result.prepared.timing.resetWallNanos;
    const auto after = dxmt9::perf::snapshotRenderTapeReplay();
    const bool resetOk = result.complete() && result.prepared.planReused &&
        result.conservation.objectsCreated == result.conservation.objectsReleased &&
        (reset != "warm-device" ||
         (result.prepared.resetPerformed && result.prepared.resetVerified));
    complete = result.complete() && sourceMatches && resetOk && deterministic &&
               after.gpuErrors == before.gpuErrors;
    std::optional<ReadbackArtifactEvidence> outputArtifact;
    std::optional<ReadbackArtifactEvidence> sourceArtifact;
    if (!readbackDir.empty()) {
      outputArtifact = writeReadbackArtifact(
          readbackRoot, i, "output", std::span<const std::byte>(result.outputPixels));
      sourceArtifact = writeReadbackArtifact(
          readbackRoot, i, "source", std::span<const std::byte>(result.sourcePixels));
    }
    if (i) std::cout << ',';
    std::cout << "{\"index\":" << i << ",\"warmup\":" << (i < warmup ? "true" : "false")
              << ",\"setup_ns\":" << setupNs << ",\"replay_ns\":" << replayNs
              << ",\"service_ns\":" << serviceNs
              << ",\"reset_ns\":" << resetNs << ",\"reset_ok\":" << (resetOk ? "true" : "false")
              << ",\"phases_wall_ns\":{\"resource_setup_mutation\":"
              << result.prepared.timing.resourceSetupNanos
              << ",\"chunk_replay\":" << result.prepared.timing.chunkReplayNanos
              << ",\"readback_drain\":" << result.prepared.timing.readbackDrainNanos
              << ",\"cleanup\":" << result.prepared.timing.cleanupNanos << '}'
              << ",\"observed_replay_path\":\"" << observedReplayPath(before, after) << '\"'
              << ",\"cpu_scope_wall_ns\":{\"prepare_slot\":"
              << after.prepareSlotWallNs - before.prepareSlotWallNs
              << ",\"resource_mark\":" << after.resourceMarkWallNs - before.resourceMarkWallNs
              << ",\"prepared_build_and_mark\":" << after.preparedSerialBuildWallNs - before.preparedSerialBuildWallNs
              << ",\"encode_chunk\":" << after.encodeChunkWallNs - before.encodeChunkWallNs
              << ",\"prepared_reads\":" << after.preparedSerialReads - before.preparedSerialReads
              << ",\"prepared_read_fallbacks\":"
              << after.preparedSerialReadFallbacks - before.preparedSerialReadFallbacks << '}'
              << ",\"path_counters\":";
    printPathDelta(before, after);
    std::cout << ",\"readback_artifacts\":";
    if (!outputArtifact || !sourceArtifact) {
      std::cout << "null";
    } else {
      std::cout << "{\"output_rgba\":{\"path\":\""
                << outputArtifact->path << "\",\"bytes\":"
                << outputArtifact->bytes << ",\"sha256\":\""
                << digestHex(outputArtifact->digest)
                << "\"},\"source_rgba\":{\"path\":\""
                << sourceArtifact->path << "\",\"bytes\":"
                << sourceArtifact->bytes << ",\"sha256\":\""
                << digestHex(sourceArtifact->digest) << "\"}}";
    }
    std::cout << ",\"provider_result\":";
    printResult(result, partitionMode, partitionMode, dxmt9::perf::snapshotRenderTapeParallelJoin(),
                !expectedSourcePath.empty(), sourceMatches, sourceExact, identity);
    std::cout << '}';
  }
  device.reset();
  factory.reset();
  std::cout << "],\"pixel_deterministic\":" << (deterministic ? "true" : "false")
            << ",\"status\":\"" << (complete ? "complete" : "failed") << "\"}\n";
  return complete ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 3 || (std::string_view(argv[1]) != "replay" && std::string_view(argv[1]) != "experiment")) {
    usage();
    return 2;
  }

  try {
    const bool experiment = std::string_view(argv[1]) == "experiment";
    unsigned warmup = 1, repeat = 5;
    std::string reset = "cold-device", route = "compatibility";
    std::vector<std::string> blobPaths;
    std::string expectedPath;
    std::string expectedSourcePath;
    std::string readbackDir;
    bool auditInitialSeeds = false;
    bool auditFreshSeedDrains = false;
    bool auditTexturePrestage = false;
    bool auditSourceBoundaries = false;
    RenderTapeSourceBoundaryMode sourceBoundaryMode = RenderTapeSourceBoundaryMode::Chunk;
    bool sourceBoundaryModeSpecified = false;
    RenderTapeDigest expectedSourcePlanDigest{};
    bool sourcePlanDigestSpecified = false;
    std::vector<RenderTapeBatchCut> auditBatchCuts;
    std::string outputPath;
    std::string sourceOutputPath;
    std::string identityPath;
    std::string textureInputArgument;
    std::string partitionMode = "identity";
    bool partitionModeSpecified = false;
    for (int index = 3; index < argc; ++index) {
      const std::string_view option = argv[index];
      if (experiment && (option == "--warmup" || option == "--repeat" ||
                         option == "--reset" || option == "--replay-route" ||
                         option == "--readback-dir")) {
        if (index + 1 >= argc) { usage(); return 2; }
        const std::string value = argv[++index];
        if (option == "--warmup") warmup = parseCount(value);
        else if (option == "--repeat") repeat = parseCount(value);
        else if (option == "--reset") reset = value;
        else if (option == "--replay-route") route = value;
        else readbackDir = value;
      } else if (experiment && option == "--source-boundary-mode") {
        if (sourceBoundaryModeSpecified || index + 1 >= argc) { usage(); return 2; }
        const std::string_view value = argv[++index];
        if (value == "capture-source")
          sourceBoundaryMode = RenderTapeSourceBoundaryMode::CaptureSource;
        else if (value != "chunk") { usage(); return 2; }
        sourceBoundaryModeSpecified = true;
      } else if (experiment && option == "--source-plan-sha256") {
        if (sourcePlanDigestSpecified || index + 1 >= argc) {
          usage(); return 2;
        }
        expectedSourcePlanDigest = parseDigestHex(argv[++index]);
        sourcePlanDigestSpecified = true;
      } else if (experiment && option == "--audit-texture-input") {
        if (!textureInputArgument.empty() || index + 1 >= argc) { usage(); return 2; }
        textureInputArgument = argv[++index];
        if (textureInputArgument.empty()) { usage(); return 2; }
      } else if (experiment && option == "--audit-initial-seeds") {
        auditInitialSeeds = true;
      } else if (experiment && option == "--audit-fresh-seed-drains") {
        auditFreshSeedDrains = true;
      } else if (experiment && option == "--audit-texture-prestage") {
        auditTexturePrestage = true;
      } else if (experiment && option == "--audit-source-boundaries") {
        auditSourceBoundaries = true;
      } else if (experiment && option == "--audit-batch-cut") {
        if (index + 1 >= argc || auditBatchCuts.size() >= 4096u) {
          usage();
          return 2;
        }
        auditBatchCuts.push_back(parseBatchCut(argv[++index]));
      } else if (option == "--partition-mode") {
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
      } else if (option == "--expected-source-rgba" &&
                 expectedSourcePath.empty()) {
        if (index + 1 >= argc) {
          usage();
          return 2;
        }
        expectedSourcePath = argv[++index];
      } else if (option == "--output-rgba" && outputPath.empty()) {
        if (index + 1 >= argc) {
          usage();
          return 2;
        }
        outputPath = argv[++index];
      } else if (option == "--output-source-rgba" &&
                 sourceOutputPath.empty()) {
        if (index + 1 >= argc) {
          usage();
          return 2;
        }
        sourceOutputPath = argv[++index];
      } else if (option == "--identity" && identityPath.empty()) {
        if (index + 1 >= argc) {
          usage();
          return 2;
        }
        identityPath = argv[++index];
      } else {
        usage();
        return 2;
      }
    }

    const bool captureSource =
        sourceBoundaryMode == RenderTapeSourceBoundaryMode::CaptureSource;
    if (experiment && (repeat == 0 || warmup + repeat > 64 ||
        (reset != "cold-device" && reset != "warm-device") ||
        (route != "compatibility" && route != "production-slot") ||
        (auditInitialSeeds && reset != "cold-device") ||
        (auditFreshSeedDrains && reset != "cold-device") ||
        (auditTexturePrestage && (reset != "cold-device" ||
                                 auditInitialSeeds || auditFreshSeedDrains)) ||
        (auditSourceBoundaries &&
         (reset != "cold-device" || route != "compatibility" ||
          auditInitialSeeds || auditFreshSeedDrains || auditTexturePrestage)) ||
        (!auditBatchCuts.empty() && !auditSourceBoundaries) ||
        (captureSource && (reset != "cold-device" || auditInitialSeeds ||
            auditFreshSeedDrains || auditTexturePrestage || auditSourceBoundaries ||
            !auditBatchCuts.empty())) ||
        (sourcePlanDigestSpecified && !captureSource) ||
        (!textureInputArgument.empty() && (!captureSource || !sourcePlanDigestSpecified ||
          reset != "cold-device")) ||
        !outputPath.empty() || !sourceOutputPath.empty())) {
      usage(); return 2;
    }
    if ((auditSourceBoundaries || captureSource) && identityPath.empty()) {
      throw std::runtime_error(
          "capture source boundaries require an authenticated --identity sidecar");
    }
    std::vector<std::string> inputPaths{argv[2]};
    std::string textureInputPath;
    std::optional<RenderTapeTextureInputAuditRequest> textureInputAudit;
    std::vector<std::byte> textureInputBytes;
    if (!textureInputArgument.empty()) {
      textureInputAudit = parseTextureInputAudit(textureInputArgument, textureInputPath);
      const std::filesystem::path input(textureInputPath);
      if (std::filesystem::is_symlink(input) || !std::filesystem::is_regular_file(input) ||
          std::filesystem::file_size(input) > 64u * 1024u * 1024u)
        throw std::runtime_error("texture input must be a bounded regular file");
      textureInputBytes = readFile(textureInputPath, 64u * 1024u * 1024u);
      if (textureInputBytes.empty())
        throw std::runtime_error("texture input must not be empty");
      textureInputAudit->bytes = textureInputBytes;
      if (RenderTapeCaptureSession::sha256(textureInputBytes) != textureInputAudit->digest)
        throw std::runtime_error("texture input SHA-256 mismatch");
      inputPaths.push_back(textureInputPath);
    }
    inputPaths.insert(inputPaths.end(), blobPaths.begin(), blobPaths.end());
    inputPaths.push_back(expectedPath);
    inputPaths.push_back(expectedSourcePath);
    inputPaths.push_back(identityPath);
    validateReadbackDirectory(readbackDir, inputPaths);
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

    auto prepared = experiment ? prepareRenderTapeIdentity(tape, blobs) : PreparedRenderTape{};
    const auto preflight = experiment ? prepared.preflightResult() : preflightRenderTapeIdentity(tape, blobs);
    std::optional<RenderTapeIdentityView> identity;
    std::vector<std::byte> identitySidecar;
    std::string identityError;
    if (!identityPath.empty()) {
      identitySidecar = readFile(identityPath);
      identity.emplace();
      const auto identityValidation = validateRenderTapeIdentity(
          tape, makeIdentityCatalogue(blobs), identitySidecar, &*identity);
      if (!identityValidation.valid()) {
        identityError = renderTapeIdentityStatusName(identityValidation.status);
        identity.reset();
      }
    }
    const bool sourceOracleRequested = !expectedSourcePath.empty();
    const auto partitionConfig =
        dxmt9::render::resolveRenderPartitionConfig(partitionMode.c_str());
    const std::string_view resolvedPartitionMode =
        dxmt9::render::partitionModeName(partitionConfig.resolved);
    if (!identityPath.empty() && !identityError.empty()) {
      printResult(preflight, partitionMode, resolvedPartitionMode,
                  dxmt9::perf::RenderTapeParallelJoinSnapshot{},
                  sourceOracleRequested, false, false, nullptr, identityError);
      return 1;
    }
    if (!preflight.complete()) {
      printResult(preflight, partitionMode, resolvedPartitionMode,
                  dxmt9::perf::RenderTapeParallelJoinSnapshot{},
                  sourceOracleRequested, false, false,
                  identity ? &*identity : nullptr, identityError);
      return 1;
    }

    if (experiment)
        return runExperiment(prepared, warmup, repeat, reset, route, partitionMode,
                           expectedPath, expectedSourcePath, readbackDir,
                           auditInitialSeeds,
                           auditFreshSeedDrains,
                           auditTexturePrestage,
                           auditSourceBoundaries,
                           sourceBoundaryMode,
                           expectedSourcePlanDigest,
                           // Keep the byte-for-byte authenticated sidecar
                           // borrowed only for the prepared execution call.
                           (auditSourceBoundaries || captureSource)
                               ? std::span<const std::byte>(identitySidecar)
                               : std::span<const std::byte>{},
                           std::span<const RenderTapeBatchCut>(auditBatchCuts),
                           textureInputAudit ? &*textureInputAudit : nullptr,
                           identity ? &*identity : nullptr);

    if (!forceHermeticReplayEnvironment(partitionMode)) {
      throw std::runtime_error(
          "cannot establish hermetic shader-archive environment");
    }

    auto* factory = dxmt9c_factory_create();
    if (!factory) {
      auto failure = preflight;
      failure.status = FrameTapeReplayStatus::ObjectCreationFailed;
      printResult(failure, partitionMode, resolvedPartitionMode,
                  dxmt9::perf::RenderTapeParallelJoinSnapshot{},
                  sourceOracleRequested, false, false,
                  identity ? &*identity : nullptr, identityError);
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
                  dxmt9::perf::RenderTapeParallelJoinSnapshot{},
                  sourceOracleRequested, false, false,
                  identity ? &*identity : nullptr, identityError);
      return 1;
    }
    auto result = replayRenderTapeIdentity(device, tape, blobs);
    dxmt9c_device_release(device);
    dxmt9c_factory_release(factory);
    const auto parallelCounters = dxmt9::perf::snapshotRenderTapeParallelJoin();
    std::vector<std::byte> expectedPixels;
    if (!expectedPath.empty()) {
      expectedPixels = readFile(expectedPath);
      (void)applyRenderTapePixelOracleEnvelope(result, expectedPixels);
    }
    bool sourceOracleMatched = !sourceOracleRequested;
    bool sourceOracleExactMatched = false;
    if (sourceOracleRequested) {
      const auto expectedSource = readFile(expectedSourcePath);
      sourceOracleExactMatched = result.validity.sourceReadback &&
          expectedSource == result.sourcePixels;
      sourceOracleMatched = result.validity.sourceReadback &&
          renderTapeSourceOracleMatchesOutputEvidence(
              expectedSource, result.sourcePixels, expectedPixels,
              result.outputPixels, result.validity.pixelEnvelopeMatched);
    }
    if (!outputPath.empty() && !result.outputPixels.empty()) {
      std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
      if (!output ||
          !output.write(reinterpret_cast<const char*>(result.outputPixels.data()),
                        static_cast<std::streamsize>(result.outputPixels.size()))) {
        throw std::runtime_error("cannot write replay output: " + outputPath);
      }
    }
    if (!sourceOutputPath.empty() && !result.sourcePixels.empty()) {
      std::ofstream output(sourceOutputPath,
                           std::ios::binary | std::ios::trunc);
      if (!output ||
          !output.write(
              reinterpret_cast<const char*>(result.sourcePixels.data()),
              static_cast<std::streamsize>(result.sourcePixels.size()))) {
        throw std::runtime_error("cannot write replay source: " +
                                 sourceOutputPath);
      }
    }
    printResult(result, partitionMode, resolvedPartitionMode, parallelCounters,
                sourceOracleRequested, sourceOracleMatched,
                sourceOracleExactMatched, identity ? &*identity : nullptr,
                identityError);
    return result.complete() && sourceOracleMatched ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "dxmt9-render-tape-provider: " << error.what() << '\n';
    return 1;
  }
}
