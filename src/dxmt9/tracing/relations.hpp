#pragma once

#include "../dxmt9_trace.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace dxmt9::tracing::detail {

enum class RelationType : uint8_t {
  ReplayToQueue,
  QueueToEncode,
  SubmitToGpuCommandBuffer,
};

struct TraceRelation {
  uint64_t relation_id = 0;
  uint64_t source_record = 0;
  uint64_t target_record = 0;
  uint64_t evidence_record = 0;
  RelationType type = RelationType::ReplayToQueue;
};

struct RelationDiagnostics {
  uint64_t unmatched_replay_publish = 0;
  uint64_t ambiguous_replay_publish = 0;
  uint64_t unmatched_queue_publish = 0;
  uint64_t ambiguous_queue_publish = 0;
  uint64_t unmatched_encode_begin = 0;
  uint64_t unmatched_submit_begin = 0;
  uint64_t ambiguous_submit_begin = 0;
  uint64_t unmatched_gpu_command_buffer = 0;
  uint64_t ambiguous_gpu_command_buffer = 0;
  uint64_t duplicate_source_records = 0;
};

struct RelationBuildResult {
  std::vector<TraceRelation> relations;
  RelationDiagnostics diagnostics;
};

// Cold-path only. Joins use explicit sequence and owner bindings; timestamps
// are never considered. Relation endpoints are exact TraceEvent source_record
// values and are ordered deterministically before relation IDs are assigned.
RelationBuildResult build_relations(std::span<const TraceEvent> events);
const char* relation_type_name(RelationType type) noexcept;

}  // namespace dxmt9::tracing::detail
