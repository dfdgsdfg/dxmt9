#include "../../../src/dxmt9/tracing/relations.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

using dxmt9::tracing::Category;
using dxmt9::tracing::EventId;
using dxmt9::tracing::Phase;
using dxmt9::tracing::TraceEvent;
using dxmt9::tracing::detail::RelationType;
using dxmt9::tracing::detail::build_relations;

namespace {
TraceEvent event(uint64_t source, EventId id, uint64_t owner, uint64_t work,
                 uint64_t related = 0, uint64_t related_owner = 0) {
  TraceEvent result{};
  result.source_record = source;
  result.process_id = 1;
  result.event_id = static_cast<uint16_t>(id);
  result.arg0 = owner;
  result.work_id = work;
  result.arg1 = related;
  result.related_owner_id = related_owner;
  result.phase = static_cast<uint8_t>(Phase::Instant);
  result.category = static_cast<uint8_t>(Category::Queue);
  return result;
}
}  // namespace

int main() {
  std::vector<TraceEvent> events{
      event(1, EventId::ReplayBegin, 99, 700),
      event(2, EventId::ReplayPublish, 99, 700, 55, 10),
      event(3, EventId::QueuePublish, 10, 55),
      event(4, EventId::EncodeBegin, 10, 55),
      event(5, EventId::EncodeBegin, 10, 55),
      event(6, EventId::SubmitBegin, 10, 800),
      event(7, EventId::GpuCommandBufferBegin, 800, 55, 0, 10),
      // A matching sequence on another queue is not a valid join.
      event(8, EventId::QueuePublish, 11, 55),
      event(9, EventId::EncodeBegin, 12, 55),
  };
  auto result = build_relations(events);
  assert(result.relations.size() == 4);
  assert(result.relations[0].relation_id == 1);
  assert(result.relations[0].type == RelationType::ReplayToQueue);
  assert(result.relations[0].source_record == 1);
  assert(result.relations[0].target_record == 3);
  assert(result.relations[1].type == RelationType::QueueToEncode);
  assert(result.relations[1].source_record == 3);
  assert(result.relations[1].target_record == 4);
  assert(result.relations[2].source_record == 3);
  assert(result.relations[2].target_record == 5);
  assert(result.relations[3].type == RelationType::SubmitToGpuCommandBuffer);
  assert(result.relations[3].source_record == 6);
  assert(result.relations[3].target_record == 7);
  assert(result.diagnostics.unmatched_queue_publish == 1);
  assert(result.diagnostics.unmatched_replay_publish == 0);
  assert(result.diagnostics.unmatched_submit_begin == 0);

  // Timestamp changes do not alter exact identity joins.
  events[0].timestamp_ns = UINT64_MAX;
  events[2].timestamp_ns = 1;
  auto reordered_time = build_relations(events);
  assert(reordered_time.relations.size() == result.relations.size());
  for (size_t index = 0; index < result.relations.size(); ++index) {
    assert(reordered_time.relations[index].source_record ==
           result.relations[index].source_record);
    assert(reordered_time.relations[index].target_record ==
           result.relations[index].target_record);
  }

  // Duplicate exact endpoints are diagnosed and never emitted as ambiguous
  // raw IDs or flow endpoints.
  events.push_back(event(3, EventId::QueuePublish, 10, 55));
  auto duplicate = build_relations(events);
  assert(duplicate.diagnostics.duplicate_source_records == 1);
  assert(duplicate.diagnostics.ambiguous_replay_publish == 1);
  assert(duplicate.diagnostics.ambiguous_queue_publish == 2);
  assert(duplicate.relations.size() == 1);
  assert(duplicate.relations.front().type ==
         RelationType::SubmitToGpuCommandBuffer);

  std::cout << "Tracing relation builder passed\n";
}
