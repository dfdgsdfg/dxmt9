#include "relations.hpp"

#include <algorithm>
#include <map>
#include <tuple>
#include <unordered_map>

namespace dxmt9::tracing::detail {
namespace {

using Key = std::tuple<uint64_t, uint64_t, uint64_t>;

bool is(const TraceEvent& event, EventId id) {
  return event.event_id == static_cast<uint16_t>(id);
}

void add_relation(RelationBuildResult& result, RelationType type,
                  const TraceEvent& source, const TraceEvent& target,
                  const std::unordered_map<uint64_t, uint32_t>& source_counts,
                  uint64_t evidence_record = 0) {
  if (!source.source_record || !target.source_record ||
      source_counts.find(source.source_record)->second != 1 ||
      source_counts.find(target.source_record)->second != 1) return;
  result.relations.push_back({.source_record = source.source_record,
                              .target_record = target.source_record,
                              .evidence_record = evidence_record,
                              .type = type});
}

}  // namespace

const char* relation_type_name(RelationType type) noexcept {
  switch (type) {
    case RelationType::ReplayToQueue: return "publishes";
    case RelationType::QueueToEncode: return "consumes";
    case RelationType::SubmitToGpuCommandBuffer: return "submitted_in";
  }
  return "unknown";
}

RelationBuildResult build_relations(std::span<const TraceEvent> events) {
  RelationBuildResult result;
  std::map<Key, std::vector<const TraceEvent*>> replays_by_owner_work;
  std::map<Key, std::vector<const TraceEvent*>> queues_by_process_owner_sequence;
  std::map<Key, std::vector<const TraceEvent*>> encodes_by_process_owner_sequence;
  std::map<Key, std::vector<const TraceEvent*>> submits_by_process_queue_cb;
  std::map<Key, std::vector<const TraceEvent*>> gpu_by_process_queue_cb;
  std::unordered_map<uint64_t, uint32_t> source_counts;

  for (const TraceEvent& event : events) {
    if (event.source_record && ++source_counts[event.source_record] > 1)
      ++result.diagnostics.duplicate_source_records;
    if (is(event, EventId::ReplayBegin)) {
      replays_by_owner_work[{event.process_id, event.arg0, event.work_id}]
          .push_back(&event);
    } else if (is(event, EventId::QueuePublish)) {
      queues_by_process_owner_sequence[
          {event.process_id, event.arg0, event.work_id}].push_back(&event);
    } else if (is(event, EventId::EncodeBegin)) {
      encodes_by_process_owner_sequence[
          {event.process_id, event.arg0, event.work_id}].push_back(&event);
    } else if (is(event, EventId::SubmitBegin)) {
      submits_by_process_queue_cb[{event.process_id, event.arg0, event.work_id}]
          .push_back(&event);
    } else if (is(event, EventId::GpuCommandBufferBegin)) {
      gpu_by_process_queue_cb[
          {event.process_id, event.related_owner_id, event.arg0}]
          .push_back(&event);
    }
  }

  for (const TraceEvent& event : events) {
    if (is(event, EventId::ReplayPublish)) {
      if (!event.process_id || !event.arg0 || !event.work_id ||
          !event.related_owner_id || !event.arg1) {
        ++result.diagnostics.unmatched_replay_publish;
        continue;
      }
      const auto replay_it = replays_by_owner_work.find(
          {event.process_id, event.arg0, event.work_id});
      const auto queue_it = queues_by_process_owner_sequence.find(
          {event.process_id, event.related_owner_id, event.arg1});
      const size_t replay_count = replay_it == replays_by_owner_work.end()
          ? 0 : replay_it->second.size();
      const size_t queue_count = queue_it == queues_by_process_owner_sequence.end()
          ? 0 : queue_it->second.size();
      if (!replay_count || !queue_count) {
        ++result.diagnostics.unmatched_replay_publish;
      } else if (replay_count != 1 || queue_count != 1) {
        ++result.diagnostics.ambiguous_replay_publish;
      } else {
        add_relation(result, RelationType::ReplayToQueue,
                     *replay_it->second.front(), *queue_it->second.front(),
                     source_counts, event.source_record);
      }
    } else if (is(event, EventId::QueuePublish)) {
      if (!event.process_id || !event.arg0 || !event.work_id) {
        ++result.diagnostics.unmatched_queue_publish;
        continue;
      }
      const auto publications = queues_by_process_owner_sequence.find(
          {event.process_id, event.arg0, event.work_id});
      if (publications == queues_by_process_owner_sequence.end() ||
          publications->second.size() != 1) {
        ++result.diagnostics.ambiguous_queue_publish;
        continue;
      }
      const auto it = encodes_by_process_owner_sequence.find(
          {event.process_id, event.arg0, event.work_id});
      if (it == encodes_by_process_owner_sequence.end() || it->second.empty()) {
        ++result.diagnostics.unmatched_queue_publish;
      } else {
        for (const TraceEvent* encode : it->second)
          add_relation(result, RelationType::QueueToEncode, event, *encode,
                       source_counts);
      }
    } else if (is(event, EventId::SubmitBegin)) {
      if (!event.process_id || !event.arg0 || !event.work_id) {
        ++result.diagnostics.unmatched_submit_begin;
        continue;
      }
      const Key key{event.process_id, event.arg0, event.work_id};
      const auto submit_it = submits_by_process_queue_cb.find(key);
      const auto it = gpu_by_process_queue_cb.find(key);
      const size_t submit_count = submit_it == submits_by_process_queue_cb.end()
          ? 0 : submit_it->second.size();
      const size_t count = it == gpu_by_process_queue_cb.end()
          ? 0 : it->second.size();
      if (submit_count != 1 || count > 1) {
        ++result.diagnostics.ambiguous_submit_begin;
      } else if (count == 0) {
        ++result.diagnostics.unmatched_submit_begin;
      } else {
        add_relation(result, RelationType::SubmitToGpuCommandBuffer, event,
                     *it->second.front(), source_counts);
      }
    }
  }

  for (const TraceEvent& event : events) {
    if (!is(event, EventId::EncodeBegin)) continue;
    if (!event.process_id || !event.arg0 || !event.work_id) {
      ++result.diagnostics.unmatched_encode_begin;
      continue;
    }
    const auto it = queues_by_process_owner_sequence.find(
        {event.process_id, event.arg0, event.work_id});
    if (it == queues_by_process_owner_sequence.end() || it->second.empty())
      ++result.diagnostics.unmatched_encode_begin;
  }

  for (const TraceEvent& event : events) {
    if (!is(event, EventId::GpuCommandBufferBegin)) continue;
    if (!event.process_id || !event.related_owner_id || !event.arg0) {
      ++result.diagnostics.unmatched_gpu_command_buffer;
      continue;
    }
    const auto it = submits_by_process_queue_cb.find(
        {event.process_id, event.related_owner_id, event.arg0});
    if (it == submits_by_process_queue_cb.end() || it->second.empty())
      ++result.diagnostics.unmatched_gpu_command_buffer;
    else if (it->second.size() != 1)
      ++result.diagnostics.ambiguous_gpu_command_buffer;
  }

  std::sort(result.relations.begin(), result.relations.end(),
            [](const TraceRelation& left, const TraceRelation& right) {
              return std::tie(left.type, left.source_record, left.target_record,
                              left.evidence_record) <
                     std::tie(right.type, right.source_record,
                              right.target_record, right.evidence_record);
            });
  result.relations.erase(
      std::unique(result.relations.begin(), result.relations.end(),
                  [](const TraceRelation& left, const TraceRelation& right) {
                    return left.type == right.type &&
                           left.source_record == right.source_record &&
                           left.target_record == right.target_record &&
                           left.evidence_record == right.evidence_record;
                  }),
      result.relations.end());
  uint64_t relation_id = 1;
  for (TraceRelation& relation : result.relations)
    relation.relation_id = relation_id++;
  return result;
}

}  // namespace dxmt9::tracing::detail
