#include "perfetto_export.hpp"
#include "artifact.hpp"
#include "relations.hpp"
#include "metrics.hpp"
#include <tuple>

#include <perfetto.h>

#include <chrono>
#include <algorithm>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace dxmt9::tracing::detail {
class EngineTraceDataSource : public perfetto::DataSource<EngineTraceDataSource> {};
}  // namespace dxmt9::tracing::detail
PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(
    dxmt9::tracing::detail::EngineTraceDataSource);

namespace dxmt9::tracing::detail {
namespace {

std::once_flag sdk_registration_once;
bool sdk_registration_ok = false;

struct SessionQuarantine {
  std::mutex mutex;
  std::unique_ptr<perfetto::TracingSession> session;
};

SessionQuarantine& session_quarantine() {
  // A timed-out SDK session may still have callbacks in flight during process
  // teardown. Keep its one bounded quarantine slot for process lifetime.
  static SessionQuarantine* quarantine = new SessionQuarantine();
  return *quarantine;
}

std::mutex& export_mutex() {
  static std::mutex* mutex = new std::mutex();
  return *mutex;
}

const char* category_name(uint8_t category) {
  switch (static_cast<Category>(category)) {
    case Category::Import: return "import";
    case Category::Replay: return "replay";
    case Category::Queue: return "queue";
    case Category::Encode: return "encode";
    case Category::Wait: return "wait";
    case Category::Submit: return "submit";
    case Category::Completion: return "completion";
    case Category::Pe: return "pe";
    case Category::Gpu: return "gpu";
    case Category::Detail: return "detail";
    case Category::Count: return "unknown";
  }
  return "unknown";
}

const char* event_name(uint16_t event) {
  switch (static_cast<EventId>(event)) {
    case EventId::ImportBegin: return "import_begin";
    case EventId::ImportEnd: return "import_end";
    case EventId::ReplayBegin: return "replay_begin";
    case EventId::ReplayEnd: return "replay_end";
    case EventId::QueuePublish: return "queue_publish";
    case EventId::EncodeBegin: return "encode_begin";
    case EventId::EncodeEnd: return "encode_end";
    case EventId::WaitBegin: return "wait_begin";
    case EventId::WaitEnd: return "wait_end";
    case EventId::Submit: return "submit";
    case EventId::Completion: return "completion";
    case EventId::ReplayPublish: return "replay_publish";
    case EventId::SubmitMember: return "submit_member";
    case EventId::CompletionDequeue: return "completion_dequeue";
    case EventId::Retire: return "retire";
    case EventId::RawReady: return "raw_ready";
    case EventId::DetailDraw: return "draw";
    case EventId::GpuCommandBufferBegin: return "gpu_command_buffer_begin";
    case EventId::GpuCommandBufferEnd: return "gpu_command_buffer_end";
    case EventId::QueryPollBegin: return "query_poll_begin";
    case EventId::QueryPollEnd: return "query_poll_end";
    case EventId::GpuStageBegin: return "gpu_stage_begin";
    case EventId::GpuStageEnd: return "gpu_stage_end";
    case EventId::ReplayDispatchBegin: return "replay_dispatch_begin";
    case EventId::ReplayDispatchEnd: return "replay_dispatch_end";
    case EventId::SubmitBegin: return "submit_begin";
    case EventId::PeChunkCommitBegin: return "pe_chunk_commit_begin";
    case EventId::PeChunkCommitEnd: return "pe_chunk_commit_end";
    case EventId::PeClockCalibration: return "pe_clock_calibration";
    case EventId::Metric: return "metric";
  }
  return "engine_event";
}

perfetto::protos::pbzero::TrackEvent::Type event_type(uint8_t phase) {
  switch (static_cast<Phase>(phase)) {
    case Phase::Begin: return perfetto::protos::pbzero::TrackEvent::TYPE_SLICE_BEGIN;
    case Phase::End: return perfetto::protos::pbzero::TrackEvent::TYPE_SLICE_END;
    case Phase::Instant: return perfetto::protos::pbzero::TrackEvent::TYPE_INSTANT;
    case Phase::Counter: return perfetto::protos::pbzero::TrackEvent::TYPE_COUNTER;
  }
  return perfetto::protos::pbzero::TrackEvent::TYPE_INSTANT;
}

std::string annotation_name(uint8_t phase, const char* name) {
  if (static_cast<Phase>(phase) == Phase::Begin)
    return std::string("begin_") + name;
  if (static_cast<Phase>(phase) == Phase::End)
    return std::string("end_") + name;
  return name;
}

void write_event(const TraceEvent& event, uint64_t pid, uint64_t counter_track,
                 const std::vector<uint64_t>& outgoing_flows,
                 const std::vector<uint64_t>& incoming_flows) {
  EngineTraceDataSource::Trace([&](EngineTraceDataSource::TraceContext context) {
    const uint64_t track_id = counter_track ? counter_track :
        (event.track_id ? event.track_id : event.thread_id);
    const int32_t process_id = static_cast<int32_t>(
        event.process_id ? event.process_id : pid);
    if (counter_track) {
      auto parent_packet = context.NewTracePacket();
      auto* parent = parent_packet->set_track_descriptor();
      parent->set_uuid(event.track_id);
      parent->set_name("dxmt9 thread");
      auto* thread = parent->set_thread();
      thread->set_pid(process_id);
      thread->set_tid(static_cast<int32_t>(event.thread_id));
    }
    {
      auto descriptor_packet = context.NewTracePacket();
      auto* descriptor = descriptor_packet->set_track_descriptor();
      descriptor->set_uuid(track_id);
      if (counter_track) {
        descriptor->set_name(metric_track_name(event));
        descriptor->set_parent_uuid(event.track_id);
        auto* counter = descriptor->set_counter();
        counter->set_unit_name(metric_unit(event.metric_id));
        counter->set_is_incremental(false);
      } else if (event.category == static_cast<uint8_t>(Category::Gpu)) {
        // Synthetic track IDs distinguish device/queue, CB-vs-stage, and
        // interval colors. They are never native thread identities.
        const bool stage = event.event_id ==
                static_cast<uint16_t>(EventId::GpuStageBegin) ||
            event.event_id == static_cast<uint16_t>(EventId::GpuStageEnd);
        const std::string name = "dxmt9 GPU device " +
            std::to_string(event.device_generation) + " queue " +
            std::to_string(event.lane_id ? event.lane_id
                                         : event.related_owner_id) +
            (stage ? " stage lane " : " command buffer lane ") +
            std::to_string(track_id);
        descriptor->set_name(name.c_str());
        descriptor->set_process()->set_pid(process_id);
      } else {
        descriptor->set_name("dxmt9 thread");
        auto* thread = descriptor->set_thread();
        thread->set_pid(process_id);
        thread->set_tid(static_cast<int32_t>(event.thread_id));
      }
    }

    auto packet = context.NewTracePacket();
    packet->set_timestamp(event.timestamp_ns);
    packet->set_timestamp_clock_id(event.clock_id);
    auto* track_event = packet->set_track_event();
    track_event->set_track_uuid(track_id);
    track_event->set_type(event_type(event.phase));
    if (counter_track) {
      track_event->set_counter_value(static_cast<int64_t>(event.metric_value));
      return;
    }
    for (uint64_t flow_id : outgoing_flows)
      track_event->add_flow_ids(flow_id);
    for (uint64_t flow_id : incoming_flows)
      track_event->add_terminating_flow_ids(flow_id);
    if (static_cast<Phase>(event.phase) != Phase::End)
      track_event->set_name(event_name(event.event_id));
    track_event->add_categories(category_name(event.category));
    auto* store = track_event->add_debug_annotations();
    store->set_name(annotation_name(event.phase, "observation_store"));
    store->set_string_value("observations.sqlite");
    auto* event_type_annotation = track_event->add_debug_annotations();
    event_type_annotation->set_name(annotation_name(event.phase, "event_type"));
    event_type_annotation->set_uint_value(event.event_id);
    if (static_cast<Phase>(event.phase) == Phase::End) {
      auto* end_name = track_event->add_debug_annotations();
      end_name->set_name("end_event_name");
      end_name->set_string_value(event_name(event.event_id));
    } else if (static_cast<Phase>(event.phase) == Phase::Begin) {
      auto* begin_name = track_event->add_debug_annotations();
      begin_name->set_name("begin_event_name");
      begin_name->set_string_value(event_name(event.event_id));
    }
    if (event.arg0) {
      auto* owner = track_event->add_debug_annotations();
      owner->set_name(annotation_name(event.phase, "owner_id"));
      owner->set_uint_value(event.arg0);
    }
    if (event.work_id) {
      auto* work = track_event->add_debug_annotations();
      work->set_name(annotation_name(event.phase, "work_id"));
      work->set_uint_value(event.work_id);
    }
    if (event.guest_process_id) {
      auto* guest_process = track_event->add_debug_annotations();
      guest_process->set_name(
          annotation_name(event.phase, "guest_process_id"));
      guest_process->set_uint_value(event.guest_process_id);
    }
    if (event.guest_thread_id) {
      auto* guest_thread = track_event->add_debug_annotations();
      guest_thread->set_name(annotation_name(event.phase, "guest_thread_id"));
      guest_thread->set_uint_value(event.guest_thread_id);
    }
    if (event.process_create_time100ns) {
      auto* process_created = track_event->add_debug_annotations();
      process_created->set_name(
          annotation_name(event.phase, "process_create_time100ns"));
      process_created->set_uint_value(event.process_create_time100ns);
    }
    if (event.thread_create_time100ns) {
      auto* thread_created = track_event->add_debug_annotations();
      thread_created->set_name(
          annotation_name(event.phase, "thread_create_time100ns"));
      thread_created->set_uint_value(event.thread_create_time100ns);
    }
    if (event.arg1) {
      auto* relation = track_event->add_debug_annotations();
      relation->set_name(annotation_name(event.phase, "related_id"));
      relation->set_uint_value(event.arg1);
      const char* binding_kind = nullptr;
      if (event.event_id == static_cast<uint16_t>(EventId::ReplayPublish))
        binding_kind = "raw_publication";
      else if (event.event_id == static_cast<uint16_t>(EventId::SubmitMember))
        binding_kind = "completion_source_membership";
      if (binding_kind) {
        auto* binding = track_event->add_debug_annotations();
        binding->set_name(annotation_name(event.phase, "binding_kind"));
        binding->set_string_value(binding_kind);
      }
    }
    if (event.related_owner_id) {
      auto* relation = track_event->add_debug_annotations();
      relation->set_name(annotation_name(event.phase, "relation_queue_identity"));
      relation->set_uint_value(event.related_owner_id);
    }
    uint64_t queue_id = event.related_owner_id;
    switch (static_cast<EventId>(event.event_id)) {
      case EventId::QueuePublish:
      case EventId::EncodeBegin:
      case EventId::EncodeEnd:
      case EventId::SubmitBegin:
      case EventId::Submit:
      case EventId::SubmitMember:
      case EventId::Retire:
        if (!queue_id) queue_id = event.arg0;
        break;
      default:
        break;
    }
    if (queue_id) {
      auto* queue = track_event->add_debug_annotations();
      queue->set_name(annotation_name(event.phase, "queue_id"));
      queue->set_uint_value(queue_id);
    }
    if (event.device_generation) {
      auto* device = track_event->add_debug_annotations();
      device->set_name(annotation_name(event.phase, "device_generation"));
      device->set_uint_value(event.device_generation);
    }
    if (event.lane_id) {
      auto* lane = track_event->add_debug_annotations();
      lane->set_name(annotation_name(event.phase, "lane_id"));
      lane->set_uint_value(event.lane_id);
    }
    auto* source = track_event->add_debug_annotations();
    source->set_name(annotation_name(event.phase, "source_record"));
    source->set_uint_value(event.source_record);
    if (event.clock_frequency) {
      auto* frequency = track_event->add_debug_annotations();
      frequency->set_name(annotation_name(event.phase, "clock_frequency"));
      frequency->set_uint_value(event.clock_frequency);
    }
    if (event.source_clock_id) {
      auto* source_clock = track_event->add_debug_annotations();
      source_clock->set_name(annotation_name(event.phase, "source_clock_id"));
      source_clock->set_uint_value(event.source_clock_id);
    }
    if (event.source_timestamp) {
      auto* source_timestamp = track_event->add_debug_annotations();
      source_timestamp->set_name(
          annotation_name(event.phase, "source_timestamp"));
      source_timestamp->set_uint_value(event.source_timestamp);
    }
    if (event.calibration_id || event.calibration_scale != 0.0 ||
        event.calibration_offset != 0.0) {
      auto* calibration = track_event->add_debug_annotations();
      calibration->set_name(annotation_name(event.phase, "calibration_id"));
      calibration->set_uint_value(event.calibration_id);
      auto* scale = track_event->add_debug_annotations();
      scale->set_name(annotation_name(event.phase, "calibration_scale"));
      scale->set_double_value(event.calibration_scale);
      auto* offset = track_event->add_debug_annotations();
      offset->set_name(annotation_name(event.phase, "calibration_offset"));
      offset->set_double_value(event.calibration_offset);
    }
    if (event.calibration_source_first || event.calibration_source_second ||
        event.calibration_scale != 0.0 || event.calibration_offset != 0.0) {
      const auto add_calibration_uint = [&](const char* name,
                                            uint64_t value) {
        auto* annotation = track_event->add_debug_annotations();
        annotation->set_name(annotation_name(event.phase, name));
        annotation->set_uint_value(value);
      };
      add_calibration_uint("calibration_source_first",
                           event.calibration_source_first);
      add_calibration_uint("calibration_host_first_ns",
                           event.calibration_host_first_ns);
      add_calibration_uint("calibration_source_second",
                           event.calibration_source_second);
      add_calibration_uint("calibration_host_second_ns",
                           event.calibration_host_second_ns);
      add_calibration_uint("calibration_valid_begin",
                           event.calibration_valid_begin);
      add_calibration_uint("calibration_valid_end",
                           event.calibration_valid_end);
    }
    if (event.category == static_cast<uint8_t>(Category::Gpu)) {
      const bool command_buffer = event.event_id ==
              static_cast<uint16_t>(EventId::GpuCommandBufferBegin) ||
          event.event_id ==
              static_cast<uint16_t>(EventId::GpuCommandBufferEnd);
      auto* unit = track_event->add_debug_annotations();
      unit->set_name(annotation_name(event.phase, "source_timestamp_unit"));
      unit->set_string_value(command_buffer ? "ns" : "ticks");
      auto* api = track_event->add_debug_annotations();
      api->set_name(annotation_name(event.phase, "source_api"));
      api->set_string_value(command_buffer
          ? (event.phase == static_cast<uint8_t>(Phase::Begin)
                 ? "WMT::CommandBuffer::gpuStartTime"
                 : "WMT::CommandBuffer::gpuEndTime")
          : (event.phase == static_cast<uint8_t>(Phase::Begin)
                 ? "MTLCounterSampleBuffer::timestamp startOfVertex"
                 : "MTLCounterSampleBuffer::timestamp endOfFragment"));
    }
    if (event.category == static_cast<uint8_t>(Category::Pe)) {
      auto* unit = track_event->add_debug_annotations();
      unit->set_name(annotation_name(event.phase, "source_timestamp_unit"));
      unit->set_string_value("ticks");
      auto* api = track_event->add_debug_annotations();
      api->set_name(annotation_name(event.phase, "source_api"));
      api->set_string_value("QueryPerformanceCounter");
    }
    if (event.scope_id) {
      auto* scope = track_event->add_debug_annotations();
      scope->set_name(annotation_name(event.phase, "scope_id"));
      scope->set_uint_value(event.scope_id);
    }
    if (event.uncertainty_ns) {
      auto* uncertainty = track_event->add_debug_annotations();
      uncertainty->set_name(annotation_name(event.phase, "uncertainty_ns"));
      uncertainty->set_uint_value(event.uncertainty_ns);
    }
  });
}

bool write_clock_snapshots(
    std::span<const TraceEvent> events,
    std::chrono::steady_clock::time_point deadline) {
  std::set<uint32_t> clock_ids;
  size_t scanned = 0;
  for (const TraceEvent& event : events) {
    if (((scanned++) & 0xffu) == 0 &&
        std::chrono::steady_clock::now() >= deadline)
      return false;
    if (!clock_ids.insert(event.clock_id).second) continue;
    EngineTraceDataSource::Trace([&](EngineTraceDataSource::TraceContext context) {
      auto packet = context.NewTracePacket();
      auto* snapshot = packet->set_clock_snapshot();
      if (event.clock_id == 3) {
        snapshot->set_primary_trace_clock(
            perfetto::protos::pbzero::BUILTIN_CLOCK_MONOTONIC);
      }
      auto* clock = snapshot->add_clocks();
      clock->set_clock_id(event.clock_id);
      clock->set_timestamp(event.timestamp_ns);
      if (event.clock_id == 3) clock->set_unit_multiplier_ns(1);
    });
  }
  return std::chrono::steady_clock::now() < deadline;
}

}  // namespace

ExportResult export_pftrace(std::span<const TraceEvent> events, const char* path,
                            const char*, uint32_t max_bytes,
                            uint32_t flush_timeout_ms) noexcept {
  ExportResult result{};
  try {
    // Serialize sessions so the quarantine slot is reserved before a session
    // is created. A later rearm is rejected after the slot is occupied.
    std::lock_guard<std::mutex> export_lock(export_mutex());
    {
      SessionQuarantine& quarantine = session_quarantine();
      std::lock_guard<std::mutex> quarantine_lock(quarantine.mutex);
      if (quarantine.session) return result;
    }
    auto retain_timed_out_session = [](
        std::unique_ptr<perfetto::TracingSession>& timed_out) {
      SessionQuarantine& quarantine = session_quarantine();
      std::lock_guard<std::mutex> lock(quarantine.mutex);
      quarantine.session = std::move(timed_out);
    };
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(flush_timeout_ms);
    perfetto::TracingInitArgs args;
    args.backends = perfetto::kInProcessBackend;
    std::call_once(sdk_registration_once, [&] {
      perfetto::Tracing::Initialize(args);
      perfetto::DataSourceDescriptor descriptor;
      descriptor.set_name("dxmt9_engine_trace");
      sdk_registration_ok = EngineTraceDataSource::Register(descriptor);
    });
    if (!sdk_registration_ok) return result;

    perfetto::TraceConfig config;
    const uint32_t size_kb = std::max(1u, max_bytes / 1024u);
    config.add_buffers()->set_size_kb(size_kb);
    auto* source = config.add_data_sources()->mutable_config();
    source->set_name("dxmt9_engine_trace");

    auto session = perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
    session->Setup(config);
    auto started = std::make_shared<std::promise<void>>();
    auto started_future = started->get_future();
    session->SetOnStartCallback([started] { started->set_value(); });
    session->Start();
    if (started_future.wait_until(deadline) != std::future_status::ready) {
      retain_timed_out_session(session);
      return result;
    }
    const uint64_t pid = static_cast<uint64_t>(getpid());
    const RelationBuildResult relation_result = build_relations(events);
    std::map<uint64_t, std::vector<uint64_t>> outgoing_flows;
    std::map<uint64_t, std::vector<uint64_t>> incoming_flows;
    for (const TraceRelation& relation : relation_result.relations) {
      outgoing_flows[relation.source_record].push_back(relation.relation_id);
      incoming_flows[relation.target_record].push_back(relation.relation_id);
    }
    if (!write_clock_snapshots(events, deadline)) {
      retain_timed_out_session(session);
      return result;
    }
    std::map<std::tuple<uint64_t, uint64_t, uint16_t>, uint64_t> counter_tracks;
    std::set<uint64_t> occupied;
    for (const auto& event : events) occupied.insert(event.track_id);
    uint64_t next_counter = UINT64_MAX;
    size_t serialized = 0;
    for (const TraceEvent& event : events) {
      if (((serialized++) & 0xffu) == 0 &&
          std::chrono::steady_clock::now() >= deadline) {
        retain_timed_out_session(session);
        return result;
      }
      const auto outgoing = outgoing_flows.find(event.source_record);
      const auto incoming = incoming_flows.find(event.source_record);
      static const std::vector<uint64_t> no_flows;
      uint64_t counter_track = 0;
      if (event.phase == static_cast<uint8_t>(Phase::Counter)) {
        const auto key = std::make_tuple(event.track_id, event.arg0, event.metric_id);
        auto found = counter_tracks.find(key);
        if (found == counter_tracks.end()) {
          while (occupied.contains(next_counter)) --next_counter;
          found = counter_tracks.emplace(key, next_counter--).first;
        }
        counter_track = found->second;
      }
      write_event(event, pid, counter_track,
                  outgoing == outgoing_flows.end() ? no_flows : outgoing->second,
                  incoming == incoming_flows.end() ? no_flows : incoming->second);
    }
    EngineTraceDataSource::Trace([](EngineTraceDataSource::TraceContext context) {
      context.Flush();
    });
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline ||
        !session->FlushBlocking(static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                .count()))) {
      // A timed-out session may still be using its data source. Keep the
      // session alive instead of letting its destructor extend finalization
      // past the caller's deadline or race pending SDK work.
      retain_timed_out_session(session);
      return result;
    }
    auto stopped = std::make_shared<std::promise<void>>();
    auto stopped_future = stopped->get_future();
    session->SetOnStopCallback([stopped] { stopped->set_value(); });
    session->Stop();
    if (stopped_future.wait_until(deadline) != std::future_status::ready) {
      retain_timed_out_session(session);
      return result;
    }
    result.flush_complete = true;
    auto stats_promise =
        std::make_shared<std::promise<perfetto::TracingSession::GetTraceStatsCallbackArgs>>();
    auto stats_future = stats_promise->get_future();
    session->GetTraceStats([stats_promise](auto args) {
      stats_promise->set_value(std::move(args));
    });
    if (stats_future.wait_until(deadline) != std::future_status::ready) {
      retain_timed_out_session(session);
      return result;
    }
    const auto stats_args = stats_future.get();
    perfetto::protos::gen::TraceStats stats;
    if (!stats_args.success ||
        !stats.ParseFromArray(stats_args.trace_stats_data.data(),
                              static_cast<int>(stats_args.trace_stats_data.size())))
      return result;
    for (const auto& buffer : stats.buffer_stats()) {
      if (buffer.has_bytes_overwritten())
        result.overwritten_bytes += buffer.bytes_overwritten();
      if (buffer.has_chunks_overwritten())
        result.discarded_chunks += buffer.chunks_overwritten();
      if (buffer.has_chunks_discarded())
        result.discarded_chunks += buffer.chunks_discarded();
      if (buffer.has_trace_writer_packet_loss())
        result.lost_packets += buffer.trace_writer_packet_loss();
      if (buffer.has_abi_violations())
        result.lost_packets += buffer.abi_violations();
    }
    if (stats.has_chunks_discarded())
      result.discarded_chunks += stats.chunks_discarded();
    if (stats.has_patches_discarded())
      result.lost_packets += stats.patches_discarded();
    if (stats.has_invalid_packets())
      result.lost_packets += stats.invalid_packets();
    if ((stats.has_invalid_packets() && stats.invalid_packets() != 0) ||
        result.lost_packets != 0 || result.overwritten_bytes != 0 ||
        result.discarded_chunks != 0 ||
        (stats.has_flushes_failed() && stats.flushes_failed() != 0) ||
        (stats.has_final_flush_outcome() &&
         stats.final_flush_outcome() ==
             perfetto::protos::gen::TraceStats_FinalFlushOutcome_FINAL_FLUSH_FAILED))
      return result;
    struct ReadState {
      std::promise<void> done;
      std::vector<char> bytes;
      bool too_large = false;
    };
    auto read_state = std::make_shared<ReadState>();
    auto read_future = read_state->done.get_future();
    session->ReadTrace([read_state, max_bytes](auto args) {
      if (args.size > max_bytes - std::min<size_t>(max_bytes,
                                                   read_state->bytes.size())) {
        read_state->too_large = true;
      } else if (!read_state->too_large && args.size) {
        read_state->bytes.insert(read_state->bytes.end(), args.data,
                                 args.data + args.size);
      }
      if (!args.has_more) read_state->done.set_value();
    });
    if (read_future.wait_until(deadline) != std::future_status::ready) {
      retain_timed_out_session(session);
      return result;
    }
    if (read_state->too_large || read_state->bytes.empty()) return result;
    AtomicArtifact output(path);
    if (!output.open() ||
        !output.write(read_state->bytes.data(), read_state->bytes.size()))
      return result;
    result.success = output.commit();
    return result;
  } catch (...) {
    return result;
  }
}

}  // namespace dxmt9::tracing::detail
