#include "device_c_render_tape_provider.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_identity.hpp"

#include "dxmt9_perf_counters.hpp"
#include "dxmt9_render_scheduling.hpp"

#include "dxmt9/device_c.h"

#include <cstddef>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
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
               " [--readback-dir <new-directory>]\n";
  std::cerr << "               [--audit-initial-seeds]"
               " [--audit-fresh-seed-drains] [--audit-texture-prestage]\n";
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
                  const RenderTapeIdentityView* identity) {
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
    std::string outputPath;
    std::string sourceOutputPath;
    std::string identityPath;
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
      } else if (experiment && option == "--audit-initial-seeds") {
        auditInitialSeeds = true;
      } else if (experiment && option == "--audit-fresh-seed-drains") {
        auditFreshSeedDrains = true;
      } else if (experiment && option == "--audit-texture-prestage") {
        auditTexturePrestage = true;
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

    if (experiment && (repeat == 0 || warmup + repeat > 64 ||
        (reset != "cold-device" && reset != "warm-device") ||
        (route != "compatibility" && route != "production-slot") ||
        (auditInitialSeeds && reset != "cold-device") ||
        (auditFreshSeedDrains && reset != "cold-device") ||
        (auditTexturePrestage && (reset != "cold-device" ||
                                 auditInitialSeeds || auditFreshSeedDrains)) ||
        !outputPath.empty() || !sourceOutputPath.empty())) {
      usage(); return 2;
    }
    std::vector<std::string> inputPaths{argv[2]};
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
    std::string identityError;
    if (!identityPath.empty()) {
      const auto sidecar = readFile(identityPath);
      identity.emplace();
      const auto identityValidation = validateRenderTapeIdentity(
          tape, makeIdentityCatalogue(blobs), sidecar, &*identity);
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
