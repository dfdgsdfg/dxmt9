#include "dxmt9_trace.hpp"

#include "tracing/config.hpp"
#include "tracing/metrics.hpp"
#include "tracing/artifact.hpp"
#include "tracing/perfetto_export.hpp"
#include "tracing/relations.hpp"
#include "tracing/session_state.hpp"
#include "dxmt9_trace_bindings.hpp"
#include "dxmt9_trace_gpu_bridge.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

#ifndef DXMT9_HAS_PERFETTO
#define DXMT9_HAS_PERFETTO 0
#endif

namespace dxmt9::tracing {

std::atomic<bool> g_trace_enabled{false};
std::atomic<bool> g_trace_detail_enabled{false};
std::atomic<uint64_t> g_trace_owner_registration_generation{0};

namespace {

constexpr uint32_t kMaxThreads = 256;
[[maybe_unused]] constexpr uint32_t kMaxEvents = 1'000'000;
[[maybe_unused]] constexpr uint32_t kMaxBytes = 128 * 1024 * 1024;
constexpr uint32_t kDeadlineMs = 30'000;
constexpr char kSdkRevision[] = "33a4fd07897a9a648664926ea27769278a19ff13";
constexpr char kSdkDigest[] =
    "01f81730839c1d833c4988fc2830ca9c9b0231b0476a4f9f87b5d63f80d77e69:"
    "33a93a2a1f02f3442b8384214d5bb466177fbbe75ba6324f09656b140d4efd19";

void report_start_rejection(const char* reason,
                            const char* detail = nullptr) noexcept {
  std::fprintf(stderr, "dxmt9 tracing: session start rejected: %s%s%s\n",
      reason ? reason : "unknown reason",
      detail ? ": " : "", detail ? detail : "");
}

struct ThreadBuffer {
  TraceEvent* events = nullptr;
  uint32_t capacity = 0;
  uint32_t count = 0;
  uint64_t dropped = 0;
  uint64_t native_tid = 0;
  uint64_t incarnation = 0;
  uint64_t track_id = 0;
};

struct ThreadCache {
  const void* owner = nullptr;
  uint64_t generation = 0;
  uint32_t index = UINT32_MAX;
};
thread_local ThreadCache tls_cache;

struct ScopeEntry {
  std::atomic<uint8_t> state{0}; // free, initializing/ending, active
  std::atomic<uint64_t> token{0};
  uint64_t generation = 0;
  uint64_t track_id = 0;
  uint64_t work_id = 0;
  uint64_t owner_id = 0;
  uint64_t related_owner_id = 0;
  uint16_t end_event = 0;
  uint8_t category = 0;
};
constexpr uint32_t kMaxScopes = 65'536;
std::array<ScopeEntry, kMaxScopes> scope_entries{};
std::atomic<uint32_t> scope_cursor{0};

struct Session {
  std::atomic<TraceState> state{TraceState::Idle};
  std::atomic<uint64_t> generation{0};
  // Closed bit, generation and active writer count share one CAS domain so a
  // delayed emitter cannot increment after close and write into reused data.
  std::atomic<uint64_t> admission_gate{
      makeAdmissionGate(0, 0, true)};
  std::atomic<uint32_t> next_thread{0};
  std::atomic<uint64_t> source_order{0};
  std::atomic<uint64_t> total_dropped{0};
  // Session-local evidence that distinguishes observed GPU work from a
  // timestamp pair that was mapped and appended successfully.
  std::atomic<uint64_t> gpu_valid_samples{0};
  std::atomic<uint64_t> gpu_calibrated_samples{0};
  std::atomic<bool> storage_sealed{false};
  std::atomic<bool> finalizer_claimed{false};
  std::array<ThreadBuffer, kMaxThreads> threads{};
  std::unique_ptr<TraceEvent[]> storage;
  uint32_t max_events = 0;
  uint32_t max_bytes = 0;
  uint32_t max_threads = 0;
  bool gpu_requested = false;
  bool gpu_required = false;
  uint32_t gpu_capacity = 0;
  uint32_t category_mask = 0;
  bool gpu_calibration = false;
  trace::gpu::Level gpu_level = trace::gpu::Level::CommandBuffer;
  bool gpu_initialized = false;
  bool gpu_attach_attempted = false;
  uint32_t delay_ms = 0;
  uint32_t duration_ms = 5000;
  uint32_t deadline_ms = kDeadlineMs;
  uint32_t max_clock_error_ns = 1000;
  uint64_t started_ns = 0;
  uint64_t stopped_ns = 0;
  bool capture_started = false;
  uint32_t process_id = 0;
  std::string run_id, attempt_id, profile_id, question, config_sha256;
  std::string receipt_path, raw_path, pftrace_path, categories_json;
  std::string required_categories_json;
  std::vector<std::string> categories;
  std::vector<std::string> required_categories;
  std::vector<std::string> optional_collectors;
  std::mutex control;
  std::condition_variable condition;
  std::thread controller;
};

Session& session() {
  // Intentionally retained for process lifetime so late writers cannot touch
  // destroyed diagnostic state during loader/process teardown.
  static Session* instance = new Session();
  return *instance;
}

uint64_t native_tid() noexcept {
  uint64_t value = 0;
  (void)pthread_threadid_np(nullptr, &value);
  return value;
}

std::string escape_json(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c < 0x20) {
      static constexpr char hex[] = "0123456789abcdef";
      out += "\\u00";
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

[[maybe_unused]] std::string json_array(const std::vector<std::string>& values) {
  std::string result = "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) result.push_back(',');
    result.push_back('"');
    result += escape_json(values[i]);
    result.push_back('"');
  }
  result.push_back(']');
  return result;
}

std::string sha256_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  CC_SHA256_CTX context;
  CC_SHA256_Init(&context);
  std::array<char, 65536> bytes{};
  while (input) {
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const auto count = input.gcount();
    if (count > 0) CC_SHA256_Update(&context, bytes.data(), static_cast<CC_LONG>(count));
  }
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256_Final(digest, &context);
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(CC_SHA256_DIGEST_LENGTH * 2);
  for (unsigned char c : digest) {
    out.push_back(hex[c >> 4]);
    out.push_back(hex[c & 15]);
  }
  return out;
}

uint32_t thread_slot(Session& s) {
  const uint64_t gen = s.generation.load(std::memory_order_acquire);
  if (tls_cache.owner == &s && tls_cache.generation == gen &&
      tls_cache.index < s.max_threads)
    return tls_cache.index;
  const uint32_t index = s.next_thread.fetch_add(1, std::memory_order_relaxed);
  if (index >= s.max_threads) {
    s.total_dropped.fetch_add(1, std::memory_order_relaxed);
    return UINT32_MAX;
  }
  ThreadBuffer& buffer = s.threads[index];
  buffer.native_tid = native_tid();
  buffer.incarnation = gen;
  buffer.track_id = (gen << 32) | (static_cast<uint64_t>(index) + 1);
  tls_cache = {&s, gen, index};
  return index;
}

const char* category_name(uint8_t category) {
  static constexpr const char* names[] = {"import", "replay", "queue",
      "encode", "wait", "submit", "completion", "pe", "gpu", "detail"};
  return category < std::size(names) ? names[category] : "unknown";
}

const char* phase_name(uint8_t phase) {
  switch (static_cast<Phase>(phase)) {
    case Phase::Instant: return "instant";
    case Phase::Begin: return "begin";
    case Phase::End: return "end";
    case Phase::Counter: return "counter";
  }
  return "instant";
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
    case EventId::SubmitBegin: return "submit_begin";
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
    case EventId::PeChunkCommitBegin: return "pe_chunk_commit_begin";
    case EventId::PeChunkCommitEnd: return "pe_chunk_commit_end";
    case EventId::PeClockCalibration: return "pe_clock_calibration";
    case EventId::Metric: return "metric";
    default: return "unknown";
  }
}

uint64_t gpu_track_id(uint64_t generation) noexcept {
  return (generation << 32) | 0xfffffff0u;
}

bool assign_gpu_tracks(std::vector<TraceEvent>& events, uint64_t generation,
                       std::chrono::steady_clock::time_point deadline) {
  using GroupKey = std::tuple<uint64_t, uint64_t, uint8_t>;
  using ScopeKey = std::tuple<uint64_t, uint64_t, uint8_t, uint64_t, uint64_t>;
  struct Interval {
    GroupKey group{};
    uint64_t begin = 0;
    uint64_t end = 0;
    uint64_t scope = 0;
    uint64_t work = 0;
    size_t color = 0;
    std::vector<size_t> members;
  };
  std::map<ScopeKey, Interval> intervals;
  std::set<uint64_t> occupied;
  for (const TraceEvent& event : events) {
    if (event.category != static_cast<uint8_t>(Category::Gpu) &&
        event.track_id)
      occupied.insert(event.track_id);
  }
  const auto group_key = [](const TraceEvent& event) {
    const uint8_t kind = event.event_id ==
            static_cast<uint16_t>(EventId::GpuStageBegin) ||
            event.event_id == static_cast<uint16_t>(EventId::GpuStageEnd)
        ? 1u : 0u;
    const uint64_t queue = event.lane_id ? event.lane_id
        : event.related_owner_id;
    return GroupKey{event.device_generation, queue, kind};
  };
  for (size_t i = 0; i < events.size(); ++i) {
    if ((i & 0xffu) == 0 &&
        std::chrono::steady_clock::now() >= deadline)
      return false;
    TraceEvent& event = events[i];
    if (event.category != static_cast<uint8_t>(Category::Gpu)) continue;
    const GroupKey group = group_key(event);
    const auto [device, queue, kind] = group;
    const ScopeKey key{device, queue, kind, event.scope_id, event.work_id};
    const bool is_begin = event.phase == static_cast<uint8_t>(Phase::Begin);
    auto [it, inserted] = intervals.try_emplace(key);
    Interval& interval = it->second;
    if (inserted) {
      interval.group = group;
      interval.scope = event.scope_id;
      interval.work = event.work_id;
      interval.begin = event.timestamp_ns;
      interval.end = event.timestamp_ns;
    }
    if (is_begin) {
      interval.begin = event.timestamp_ns;
    } else {
      interval.end = event.timestamp_ns;
    }
    interval.members.push_back(i);
  }

  std::map<GroupKey, std::vector<Interval*>> by_group;
  for (auto& [key, interval] : intervals) {
    (void)key;
    by_group[interval.group].push_back(&interval);
  }
  std::map<std::pair<GroupKey, size_t>, uint64_t> track_by_lane;
  uint64_t serial = 1;
  for (auto& [group, lane_intervals] : by_group) {
    std::sort(lane_intervals.begin(), lane_intervals.end(),
              [](const Interval* left, const Interval* right) {
                return std::tie(left->begin, left->end, left->scope,
                                left->work) <
                       std::tie(right->begin, right->end, right->scope,
                                right->work);
              });
    std::vector<uint64_t> color_ends;
    size_t colored = 0;
    for (Interval* interval : lane_intervals) {
      if (((colored++) & 0xffu) == 0 &&
          std::chrono::steady_clock::now() >= deadline)
        return false;
      size_t color = 0;
      while (color < color_ends.size() &&
             color_ends[color] > interval->begin)
        ++color;
      if (color == color_ends.size()) color_ends.push_back(interval->end);
      else color_ends[color] = interval->end;
      interval->color = color;
      auto [track, inserted] = track_by_lane.try_emplace(
          std::make_pair(group, color), 0);
      if (inserted) {
        uint64_t candidate = (uint64_t{0xd9f9} << 48) |
            ((generation & 0xffffu) << 32) | serial++;
        while (!candidate || occupied.contains(candidate)) ++candidate;
        occupied.insert(candidate);
        track->second = candidate;
      }
      for (size_t member : interval->members)
        events[member].track_id = track->second;
    }
  }
  return std::chrono::steady_clock::now() < deadline;
}

bool write_raw(Session& s, std::chrono::steady_clock::time_point deadline) {
  struct GuestIdentity {
    uint64_t process_id;
    uint64_t thread_id;
    uint64_t process_created;
    uint64_t thread_created;
  };
  std::ostringstream out;
  out << std::setprecision(17);
  const uint32_t pid = s.process_id;
  const uint32_t count = std::min(s.next_thread.load(), s.max_threads);
  const uint64_t generation = s.generation.load();
  std::vector<GuestIdentity> guestIdentities;
  std::vector<std::pair<uint64_t, uint64_t>> guestProcesses;
  std::vector<std::pair<uint64_t, uint64_t>> peHostThreads;
  std::set<std::tuple<uint64_t, uint64_t, uint64_t, uint64_t>> seenGuests;
  std::set<std::pair<uint64_t, uint64_t>> seenGuestProcesses;
  std::set<std::pair<uint64_t, uint64_t>> seenPeHostThreads;
  std::vector<TraceEvent> rawEvents;
  size_t rawEventCount = 0;
  size_t scan = 0;
  for (uint32_t i = 0; i < count; ++i) rawEventCount += s.threads[i].count;
  rawEvents.reserve(rawEventCount);
  for (uint32_t i = 0; i < count; ++i) {
    const ThreadBuffer& t = s.threads[i];
    rawEvents.insert(rawEvents.end(), t.events, t.events + t.count);
    for (uint32_t j = 0; j < t.count; ++j) {
      if (((scan++) & 0xffu) == 0 &&
          std::chrono::steady_clock::now() >= deadline)
        return false;
      const TraceEvent& e = t.events[j];
      if (e.category != static_cast<uint8_t>(Category::Pe)) continue;
      const std::pair<uint64_t, uint64_t> hostThread{
          e.track_id ? e.track_id : t.track_id,
          e.thread_id ? e.thread_id : t.native_tid};
      if (seenPeHostThreads.insert(hostThread).second)
        peHostThreads.push_back(hostThread);
      if (!e.guest_process_id || !e.guest_thread_id ||
          !e.process_create_time100ns || !e.thread_create_time100ns)
        continue;
      const GuestIdentity identity{e.guest_process_id, e.guest_thread_id,
          e.process_create_time100ns, e.thread_create_time100ns};
      if (seenGuests.emplace(identity.process_id, identity.thread_id,
                             identity.process_created,
                             identity.thread_created).second)
        guestIdentities.push_back(identity);
      const std::pair<uint64_t, uint64_t> guestProcess{
          identity.process_id, identity.process_created};
      if (seenGuestProcesses.insert(guestProcess).second)
        guestProcesses.push_back(guestProcess);
    }
  }
  if (!assign_gpu_tracks(rawEvents, generation, deadline)) return false;
  const detail::RelationBuildResult relationBuild =
      detail::build_relations(rawEvents);
  out << "{\"schema\":\"dxmt9.trace.raw.v1\",\"run_id\":\""
      << escape_json(s.run_id) << "\",\"attempt_id\":\""
      << escape_json(s.attempt_id) << "\",\"profile_id\":\""
      << escape_json(s.profile_id) << "\",\"question\":\""
      << escape_json(s.question) << "\",\"collector\":\"engine-perfetto\","
      << "\"config_sha256\":\"" << s.config_sha256
      << "\",\"processes\":[{\"process_id\":\"" << pid
      << "\",\"pid\":" << pid << ",\"incarnation\":\""
      << generation << "\",\"identity_quality\":\"qualified\"}";
  for (const auto& [guestPid, created] : guestProcesses) {
    out << ",{\"process_id\":\"guest:" << guestPid << ':'
        << created << "\",\"pid\":\""
        << guestPid << "\",\"incarnation\":\""
        << created
        << "\",\"identity_quality\":\"qualified\",\"pid_namespace\":\"guest\",\"guest_process_id\":\""
        << guestPid << "\",\"creation_time100ns\":\""
        << created << "\"}";
  }
  out << "],\"threads\":[";
  bool firstTrackDescriptor = true;
  const auto startTrackDescriptor = [&] {
    if (!firstTrackDescriptor) out << ',';
    firstTrackDescriptor = false;
  };
  for (uint32_t i = 0; i < count; ++i) {
    startTrackDescriptor();
    const ThreadBuffer& t = s.threads[i];
    out << "{\"thread_id\":\"" << t.track_id << "\",\"native_tid\":\""
        << t.native_tid << "\",\"track_id\":\"" << t.track_id
        << "\",\"incarnation\":\"" << t.incarnation
        << "\",\"process_id\":\"" << pid
        << "\",\"identity_quality\":\"qualified\",\"track_kind\":\"thread\"}";
  }
  std::set<uint64_t> gpuTracks;
  std::map<uint64_t, const TraceEvent*> gpuTrackEvents;
  for (const TraceEvent& event : rawEvents)
    if (event.category == static_cast<uint8_t>(Category::Gpu) &&
        event.track_id) {
      gpuTracks.insert(event.track_id);
      gpuTrackEvents.try_emplace(event.track_id, &event);
    }
  for (uint64_t track : gpuTracks) {
    startTrackDescriptor();
    const TraceEvent& event = *gpuTrackEvents.at(track);
    const bool stage = event.event_id ==
            static_cast<uint16_t>(EventId::GpuStageBegin) ||
        event.event_id == static_cast<uint16_t>(EventId::GpuStageEnd);
    out << "{\"thread_id\":\"" << track << "\",\"native_tid\":null"
        << ",\"track_id\":\"" << track << "\",\"incarnation\":\""
        << generation << "\",\"process_id\":\"" << pid
        << "\",\"identity_quality\":\"qualified\",\"track_kind\":\"gpu\""
        << ",\"device_generation\":\"" << event.device_generation
        << "\",\"queue_id\":\"" << event.lane_id
        << "\",\"gpu_lane_kind\":\"" << (stage ? "stage" : "command_buffer")
        << "\"}";
  }
  for (const auto& [track, nativeTid] : peHostThreads) {
    startTrackDescriptor();
    out << "{\"thread_id\":\"" << track << "\",\"native_tid\":\""
        << nativeTid << "\",\"track_id\":\"" << track
        << "\",\"incarnation\":\"host-thread:" << nativeTid
        << "\",\"process_id\":\"" << pid
        << "\",\"identity_quality\":\"qualified\",\"track_kind\":\"thread\"}";
  }
  for (const GuestIdentity& identity : guestIdentities) {
    startTrackDescriptor();
    const std::string guestProcess = "guest:" + std::to_string(identity.process_id) +
        ':' + std::to_string(identity.process_created);
    const std::string guestThread = "guest:" + std::to_string(identity.process_id) +
        ':' + std::to_string(identity.process_created) + ':' +
        std::to_string(identity.thread_id) + ':' +
        std::to_string(identity.thread_created);
    out << "{\"thread_id\":\"" << guestThread
        << "\",\"native_tid\":null,\"track_id\":\"" << guestThread
        << "\",\"incarnation\":\"" << identity.thread_created
        << "\",\"process_id\":\"" << guestProcess
        << "\",\"identity_quality\":\"qualified\",\"tid_namespace\":\"guest\",\"guest_tid\":\""
        << identity.thread_id << "\"}";
  }
  uint64_t nativeCount = 0;
  uint64_t peCount = 0;
  uint64_t gpuCount = 0;
  size_t countScan = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const ThreadBuffer& t = s.threads[i];
    for (uint32_t j = 0; j < t.count; ++j) {
      if (((countScan++) & 0xffu) == 0 &&
          std::chrono::steady_clock::now() >= deadline)
        return false;
      switch (static_cast<Category>(t.events[j].category)) {
        case Category::Pe: ++peCount; break;
        case Category::Gpu: ++gpuCount; break;
        default: ++nativeCount; break;
      }
    }
  }
  const mach_timebase_info_data_t timebase = [] {
    mach_timebase_info_data_t value{};
    mach_timebase_info(&value);
    return value;
  }();
  const uint64_t resolutionNs = timebase.denom
      ? std::max<uint64_t>(1, (timebase.numer + timebase.denom - 1) /
                                  timebase.denom)
      : 1;
  out << "],\"clocks\":[{\"clock_id\":\"3\",\"unit\":\"ns\","
         "\"kind\":\"monotonic\",\"api\":\"mach_absolute_time\","
         "\"source\":\"mach_absolute_time\",\"domain\":\"native-monotonic\","
         "\"resolution_ns\":\"" << resolutionNs
      << "\",\"uncertainty_ns\":\"" << resolutionNs
      << "\",\"alignment\":\"qualified\"}";
  std::set<uint32_t> seenClockIds{0, 3};
  size_t clockScan = 0;
  for (const TraceEvent& e : rawEvents) {
    if (((clockScan++) & 0xffu) == 0 &&
        std::chrono::steady_clock::now() >= deadline)
      return false;
    if (!seenClockIds.insert(e.clock_id).second) continue;
    out << ",{\"clock_id\":\"" << e.clock_id << "\",\"unit\":\"ticks\","
        << "\"kind\":\"external_counter\",\"api\":\"external\","
        << "\"source\":\"external\",\"domain\":\"external-counter\","
        << "\"resolution_ns\":\"unknown\",\"uncertainty_ns\":\""
        << e.uncertainty_ns << "\",\"frequency\":\""
        << e.clock_frequency << "\",\"alignment\":\"unqualified\"}";
  }
  out << "],\"clock_calibrations\":[";
  bool firstCalibration = true;
  std::set<std::pair<uint64_t, uint64_t>> seenGpuCalibrations;
  size_t calibrationScan = 0;
  for (const TraceEvent& e : rawEvents) {
      if (((calibrationScan++) & 0xffu) == 0 &&
          std::chrono::steady_clock::now() >= deadline)
        return false;
      const bool peCalibration = e.event_id ==
          static_cast<uint16_t>(EventId::PeClockCalibration);
      const bool gpuCalibration = e.event_id ==
              static_cast<uint16_t>(EventId::GpuStageBegin) ||
          e.event_id ==
              static_cast<uint16_t>(EventId::GpuCommandBufferBegin);
      if (gpuCalibration && !e.calibration_id &&
          e.calibration_scale == 0.0 && e.calibration_offset == 0.0)
        continue;
      if (!peCalibration && !gpuCalibration) continue;
      if (gpuCalibration) {
        if (!seenGpuCalibrations.emplace(e.source_clock_id,
                                         e.calibration_id).second)
          continue;
      }
      if (!firstCalibration) out << ',';
      firstCalibration = false;
      out << "{\"kind\":\"" << (peCalibration ? "qpc" : "gpu")
          << "\",\"mapping_kind\":\""
          << (peCalibration ? "bracket" :
              (e.event_id == static_cast<uint16_t>(
                   EventId::GpuCommandBufferBegin)
                   ? "host_api_identity" : "two_anchor"))
          << "\",\"calibration_id\":\"" << e.calibration_id
          << "\",\"source_order\":\"" << e.work_id
          << "\",\"source_clock_id\":\"" << e.source_clock_id
          << "\",\"source_ticks\":\"" << e.source_timestamp
          << "\",\"source_before\":\""
          << (peCalibration ? e.arg0 : e.calibration_source_first)
          << "\",\"source_after\":\""
          << (peCalibration ? e.arg1 : e.calibration_source_second)
          << "\",\"host_ns\":\"" << e.timestamp_ns
          << "\",\"host_clock_id\":\"" << e.clock_id
          << "\",\"host_before_ns\":\""
          << (peCalibration ? e.timestamp_ns : e.calibration_host_first_ns)
          << "\",\"host_after_ns\":\""
          << (peCalibration ? e.timestamp_ns : e.calibration_host_second_ns)
          << "\",\"frequency\":\"" << e.clock_frequency
          << "\",\"scale\":\"" << e.calibration_scale
          << "\",\"offset_ns\":\"" << e.calibration_offset
          << "\",\"valid_begin\":\""
          << (peCalibration ? e.arg0 : e.calibration_valid_begin)
          << "\",\"valid_end\":\""
          << (peCalibration ? e.arg1 : e.calibration_valid_end)
          << "\",\"error_ns\":\"" << e.uncertainty_ns << "\"}";
  }
  out << "],\"qualification_policy\":{\"max_clock_error_ns\":\""
      << s.max_clock_error_ns << "\"},"
      << "\"window\":{\"start_ns\":\""
      << s.started_ns << "\",\"end_ns\":\"" << s.stopped_ns
      << "\",\"clock_id\":\"3\""
      << ",\"closed\":true},\"loss\":{\"total\":\""
      << s.total_dropped.load() << "\"},\"coverage\":{"
         "\"native_engine\":{\"observed\":\"" << nativeCount
      << "\",\"population\":\"" << nativeCount << "\"},"
      << "\"pe\":{\"observed\":\"" << peCount
      << "\",\"population\":\"" << peCount << "\"},"
      << "\"gpu\":{\"observed\":\"" << gpuCount
      << "\",\"population\":\"" << gpuCount << "\"}},\"relations\":[";
  for (size_t i = 0; i < relationBuild.relations.size(); ++i) {
    const detail::TraceRelation& relation = relationBuild.relations[i];
    if (i) out << ',';
    out << "{\"relation_id\":\"" << relation.relation_id
        << "\",\"source_event\":\"" << relation.source_record
        << "\",\"target_event\":\"" << relation.target_record << '"';
    if (relation.evidence_record)
      out << ",\"evidence_record\":\"" << relation.evidence_record << '"';
    out << ",\"type\":\"" << detail::relation_type_name(relation.type)
        << "\",\"evidence\":\"observed_binding\",\"flow\":true}";
  }
  const detail::RelationDiagnostics& diagnostics = relationBuild.diagnostics;
  out << "],\"relation_requirements\":[";
  bool firstRelationRequirement = true;
  const auto addRelationRequirement = [&](bool applicable, const char* name) {
    if (!applicable) return;
    if (!firstRelationRequirement) out << ',';
    firstRelationRequirement = false;
    out << '"' << name << '"';
  };
  const auto categorySelected = [&](Category category) {
    return (s.category_mask & (uint32_t{1} << static_cast<uint8_t>(category))) != 0;
  };
  addRelationRequirement(categorySelected(Category::Replay) &&
                             categorySelected(Category::Queue),
                         "replay_queue");
  addRelationRequirement(categorySelected(Category::Queue) &&
                             categorySelected(Category::Encode),
                         "queue_encode");
  addRelationRequirement(s.gpu_requested && s.gpu_initialized,
                         "submit_gpu");
  out << "],\"relation_diagnostics\":{\"unmatched_replay_publish\":\""
      << diagnostics.unmatched_replay_publish
      << "\",\"ambiguous_replay_publish\":\""
      << diagnostics.ambiguous_replay_publish
      << "\",\"unmatched_queue_publish\":\""
      << diagnostics.unmatched_queue_publish
      << "\",\"ambiguous_queue_publish\":\""
      << diagnostics.ambiguous_queue_publish
      << "\",\"unmatched_encode_begin\":\""
      << diagnostics.unmatched_encode_begin
      << "\",\"unmatched_submit_begin\":\""
      << diagnostics.unmatched_submit_begin
      << "\",\"ambiguous_submit_begin\":\""
      << diagnostics.ambiguous_submit_begin
      << "\",\"unmatched_gpu_command_buffer\":\""
      << diagnostics.unmatched_gpu_command_buffer
      << "\",\"ambiguous_gpu_command_buffer\":\""
      << diagnostics.ambiguous_gpu_command_buffer
      << "\",\"duplicate_source_records\":\""
      << diagnostics.duplicate_source_records << "\"}}\n";
  size_t eventScan = 0;
  for (const TraceEvent& e : rawEvents) {
      if (((eventScan++) & 0xffu) == 0 &&
          std::chrono::steady_clock::now() >= deadline)
        return false;
      out << "{\"event_id\":\"" << e.source_record
          << "\",\"collector\":\"engine-perfetto\",\"track_kind\":\""
          << (e.category == static_cast<uint8_t>(Category::Gpu) ? "gpu" : "thread")
          << "\","
          << "\"source_record\":\"" << e.source_record
          << "\",\"source_order\":\"" << e.source_record
          << "\",\"generation\":\"" << generation
          << "\",\"process_id\":\"" << (e.process_id ? e.process_id : pid)
          << "\",\"thread_id\":\"" << (e.track_id ? e.track_id : e.thread_id)
        << "\",\"native_tid\":";
      if (e.category == static_cast<uint8_t>(Category::Gpu)) out << "null";
      else out << '"' << e.thread_id << '"';
      out
          << ",\"clock_id\":\"" << e.clock_id
          << "\",\"timestamp_ns\":\"" << e.timestamp_ns
          << "\",\"event_type\":" << e.event_id
          << ",\"category\":\"" << category_name(e.category)
          << "\",\"name\":\"" << event_name(e.event_id)
          << "\",\"phase\":\"" << phase_name(e.phase)
          << "\",\"scope_id\":\"" << e.scope_id
          << "\",\"work_ids\":[";
      if (e.work_id) out << '"' << e.work_id << '"';
      out << "],\"track_id\":\"" << (e.track_id ? e.track_id : e.thread_id)
          << "\",\"timestamp\":\"" << e.timestamp_ns
          << "\",\"kind\":\"" << phase_name(e.phase)
          << "\",\"attrs\":{\"name\":\"" << event_name(e.event_id)
          << "\",\"category\":\"" << category_name(e.category)
          << "\",\"native_tid\":";
      if (e.category == static_cast<uint8_t>(Category::Gpu)) out << "null";
      else out << '"' << e.thread_id << '"';
      out
          << ",\"observation_store\":\"observations.sqlite\""
          << ",\"arg0\":\"" << e.arg0
          << "\",\"arg1\":\"" << e.arg1 << "\"";
      if (e.phase == static_cast<uint8_t>(Phase::Counter))
        out << ",\"counter_value\":\"" << e.metric_value
            << "\",\"metric_id\":\"" << e.metric_id
            << "\",\"unit\":\"" << metric_unit(e.metric_id)
            << "\",\"track_name\":\"" << metric_track_name(e) << "\"";
      out << "},\"owner_id\":\"" << e.arg0 << "\",\"related_id\":\"" << e.arg1
          << "\",\"uncertainty_ns\":\"" << e.uncertainty_ns
          << "\",\"clock_frequency\":\"" << e.clock_frequency
          << "\",\"source_clock_id\":\"" << e.source_clock_id
          << "\",\"calibration_id\":\"" << e.calibration_id
          << "\"";
      if (e.related_owner_id)
        out << ",\"relation_queue_identity\":\""
            << e.related_owner_id << '"';
      uint64_t queueId = e.related_owner_id;
      switch (static_cast<EventId>(e.event_id)) {
        case EventId::QueuePublish:
        case EventId::EncodeBegin:
        case EventId::EncodeEnd:
        case EventId::SubmitBegin:
        case EventId::Submit:
        case EventId::SubmitMember:
        case EventId::Retire:
          if (!queueId) queueId = e.arg0;
          break;
        default:
          break;
      }
      if (queueId) out << ",\"queue_id\":\"" << queueId << '"';
      if (e.device_generation)
        out << ",\"device_generation\":\"" << e.device_generation << '"';
      if (e.lane_id) out << ",\"lane_id\":\"" << e.lane_id << '"';
      if (e.guest_process_id)
        out << ",\"guest_process_id\":\"" << e.guest_process_id << '"';
      if (e.guest_thread_id)
        out << ",\"guest_thread_id\":\"" << e.guest_thread_id << '"';
      if (e.process_create_time100ns)
        out << ",\"process_create_time100ns\":\""
            << e.process_create_time100ns << '"';
      if (e.thread_create_time100ns)
        out << ",\"thread_create_time100ns\":\""
            << e.thread_create_time100ns << '"';
      if (e.source_timestamp)
        out << ",\"source_timestamp\":\"" << e.source_timestamp << '"';
      if (e.calibration_scale != 0.0)
        out << ",\"calibration_scale\":\"" << e.calibration_scale << '"';
      if (e.calibration_offset != 0.0)
        out << ",\"calibration_offset\":\"" << e.calibration_offset << '"';
      if (e.calibration_source_first || e.calibration_source_second ||
          e.calibration_scale != 0.0 || e.calibration_offset != 0.0) {
        out << ",\"calibration_source_first\":\""
            << e.calibration_source_first
            << "\",\"calibration_host_first_ns\":\""
            << e.calibration_host_first_ns
            << "\",\"calibration_source_second\":\""
            << e.calibration_source_second
            << "\",\"calibration_host_second_ns\":\""
            << e.calibration_host_second_ns
            << "\",\"calibration_valid_begin\":\""
            << e.calibration_valid_begin
            << "\",\"calibration_valid_end\":\""
            << e.calibration_valid_end << '"';
      }
      if (e.category == static_cast<uint8_t>(Category::Gpu)) {
        const bool commandBuffer = e.event_id == static_cast<uint16_t>(
                EventId::GpuCommandBufferBegin) ||
            e.event_id == static_cast<uint16_t>(
                EventId::GpuCommandBufferEnd);
        out << ",\"source_timestamp_unit\":\""
            << (commandBuffer ? "ns" : "ticks") << '"';
        if (commandBuffer)
          out << ",\"source_api\":\"WMT::CommandBuffer::"
              << (e.phase == static_cast<uint8_t>(Phase::Begin)
                      ? "gpuStartTime" : "gpuEndTime")
              << "\"";
        else
          out << ",\"source_api\":\"MTLCounterSampleBuffer::timestamp "
              << (e.phase == static_cast<uint8_t>(Phase::Begin)
                      ? "startOfVertex" : "endOfFragment")
              << "\"";
      }
      if (e.category == static_cast<uint8_t>(Category::Pe)) {
        out << ",\"source_timestamp_unit\":\"ticks\","
            << "\"source_api\":\"QueryPerformanceCounter\","
            << "\"native_observation\":{\"source_order\":\""
            << e.work_id << "\",\"host_process_id\":\""
            << (e.process_id ? e.process_id : pid)
            << "\",\"host_thread_id\":\""
            << e.thread_id << "\"}";
      }
      const char* bindingKind = e.event_id ==
              static_cast<uint16_t>(EventId::ReplayPublish)
          ? "raw_publication"
          : (e.event_id == static_cast<uint16_t>(EventId::SubmitMember)
                 ? "completion_source_membership" : nullptr);
      if (bindingKind) {
        out << ",\"binding_kind\":\"" << bindingKind
            << '"';
      }
      out << "}\n";
  }
  const std::string text = out.str();
  if (std::chrono::steady_clock::now() >= deadline) return false;
  detail::AtomicArtifact artifact(s.raw_path);
  return artifact.open() && artifact.write(text.data(), text.size()) &&
      artifact.commit();
}

bool publish_receipt(Session& s, const char* status, const char* flush_status,
                     uint64_t sdk_lost_packets = 0,
                     uint64_t sdk_overwritten_bytes = 0,
                     uint64_t sdk_discarded_chunks = 0,
                     bool include_buffer_counts = true) noexcept {
  try {
    struct stat artifact_stat{};
    struct stat raw_stat{};
    const bool artifact_exists = stat(s.pftrace_path.c_str(), &artifact_stat) == 0;
    const bool raw_exists = stat(s.raw_path.c_str(), &raw_stat) == 0;
    const uint32_t thread_count = include_buffer_counts
        ? std::min(s.next_thread.load(), s.max_threads) : 0;
    std::array<uint64_t, static_cast<size_t>(Category::Count)> category_counts{};
    uint64_t event_count = 0;
    uint64_t event_drops = 0;
    if (include_buffer_counts) {
      for (uint32_t i = 0; i < thread_count; ++i) {
        const ThreadBuffer& t = s.threads[i];
        event_count += t.count;
        event_drops += t.dropped;
        for (uint32_t j = 0; j < t.count; ++j) {
          const uint8_t category = t.events[j].category;
          if (category < category_counts.size()) ++category_counts[category];
        }
      }
    }
    const trace::gpu::Counters gpu = s.gpu_initialized
        ? trace::gpu::processGpuTraceCounters() : trace::gpu::Counters{};
    const trace::gpu::StageCounters stage = s.gpu_initialized
        ? trace::gpu::processGpuStageCounters() : trace::gpu::StageCounters{};
    const uint64_t gpu_valid_samples =
        s.gpu_valid_samples.load(std::memory_order_relaxed);
    const uint64_t gpu_calibrated_samples =
        s.gpu_calibrated_samples.load(std::memory_order_relaxed);
    const char* metal_timing_status =
        s.gpu_initialized && gpu_calibrated_samples != 0
        ? "complete" : (gpu_valid_samples != 0 ? "failed" : "unavailable");
    const uint64_t gpu_loss = gpu.lostCapacity + gpu.invalid + gpu.stale +
        stage.lostCapacity + stage.invalid + stage.stale;
    const uint64_t loss_total = s.total_dropped.load() + gpu_loss +
        sdk_lost_packets + sdk_overwritten_bytes + sdk_discarded_chunks;
    std::vector<std::string> resolved;
    std::ostringstream applicability;
    applicability << '{';
    for (size_t i = 0; i < s.categories.size(); ++i) {
      const std::string& category = s.categories[i];
      uint64_t count = 0;
      if (category == "gpu") count = category_counts[static_cast<size_t>(Category::Gpu)];
      else if (category == "pe") count = category_counts[static_cast<size_t>(Category::Pe)];
      else if (category == "import") count = category_counts[static_cast<size_t>(Category::Import)];
      else if (category == "replay") count = category_counts[static_cast<size_t>(Category::Replay)];
      else if (category == "queue") count = category_counts[static_cast<size_t>(Category::Queue)];
      else if (category == "encode") count = category_counts[static_cast<size_t>(Category::Encode)];
      else if (category == "wait") count = category_counts[static_cast<size_t>(Category::Wait)];
      else if (category == "submit") count = category_counts[static_cast<size_t>(Category::Submit)];
      else if (category == "completion") count = category_counts[static_cast<size_t>(Category::Completion)];
      else if (category == "detail") count = category_counts[static_cast<size_t>(Category::Detail)];
      const bool required = std::find(s.required_categories.begin(),
                                      s.required_categories.end(), category) !=
                            s.required_categories.end();
      const bool required_gpu_missing = required && category == "gpu" && count == 0;
      if (!required_gpu_missing) resolved.push_back(category);
      if (i) applicability << ',';
      applicability << '"' << escape_json(category) << "\":{\"status\":\""
                    << (required_gpu_missing ? "failed" : (count ? "observed" : "not_applicable"))
                    << "\",\"count\":" << count << '}';
    }
    applicability << '}';
    std::ostringstream optional;
    optional << '{';
    for (size_t i = 0; i < s.optional_collectors.size(); ++i) {
      if (i) optional << ',';
      const auto& collector = s.optional_collectors[i];
      const bool observed = collector == "metal-timing"
          ? gpu_calibrated_samples != 0
          : (collector == "pe-events"
              ? category_counts[static_cast<size_t>(Category::Pe)] != 0 : false);
      const char* collector_status = collector == "metal-timing"
          ? metal_timing_status : (observed ? "complete" : "unavailable");
      optional << '"' << escape_json(collector) << "\":{\"status\":\""
               << collector_status
               << "\"}";
    }
    optional << '}';
    std::ostringstream out;
    out << "{\"schema\":\"dxmt9.trace.receipt.v1\",\"run_id\":\""
        << escape_json(s.run_id) << "\",\"attempt_id\":\""
        << escape_json(s.attempt_id) << "\",\"profile_id\":\""
        << escape_json(s.profile_id) << "\",\"config_sha256\":\""
        << s.config_sha256 << "\",\"status\":\"" << status
        << "\",\"started\":\"" << s.started_ns << "\",\"stopped\":\""
        << s.stopped_ns << "\",\"window\":{\"closed\":true,\"mode\":\"duration\"}"
        << ",\"counts\":{\"engine_events\":";
    if (include_buffer_counts) out << '"' << event_count << '"';
    else out << "null";
    out << ",\"threads\":\"" << s.next_thread.load()
        << "\",\"pe_events\":";
    if (include_buffer_counts) out << '"' << category_counts[static_cast<size_t>(Category::Pe)] << '"';
    else out << "null";
    out << ",\"gpu_events\":";
    if (include_buffer_counts) out << '"' << category_counts[static_cast<size_t>(Category::Gpu)] << '"';
    else out << "null";
    out << "},\"resource_bounds\":{\"event_storage_bytes\":\""
        << static_cast<uint64_t>(s.max_events) * sizeof(TraceEvent)
        << "\",\"sdk_buffer_bytes\":\"" << s.max_bytes
        << "\",\"cold_event_copy_count\":\"2\",\"events_per_copy_max\":\""
        << s.max_events << "\",\"thread_slots_max\":\"" << s.max_threads
        << "\",\"cold_auxiliary_items_max\":\"" << s.max_events
        << "\"},\"loss\":{\"total\":\"" << loss_total
        << "\",\"event_overflow\":";
    if (include_buffer_counts) out << '"' << event_drops << '"';
    else out << "null";
    out << ",\"thread_overflow\":\""
        << (s.next_thread.load() > s.max_threads ? 1 : 0)
        << "\",\"sdk_lost_packets\":";
    if (include_buffer_counts) out << '"' << sdk_lost_packets << '"';
    else out << "null";
    out << ",\"sdk_overwritten_bytes\":";
    if (include_buffer_counts) out << '"' << sdk_overwritten_bytes << '"';
    else out << "null";
    out << ",\"sdk_discarded_chunks\":";
    if (include_buffer_counts) out << '"' << sdk_discarded_chunks << '"';
    else out << "null";
    out << ",\"gpu_capacity\":\"" << (gpu.lostCapacity + stage.lostCapacity)
        << "\",\"gpu_invalid\":\"" << (gpu.invalid + stage.invalid)
        << "\",\"gpu_stale\":\"" << (gpu.stale + stage.stale)
        << "\"},\"categories_requested\":" << json_array(s.categories)
        << ",\"categories_resolved\":" << json_array(resolved)
        << ",\"applicability\":" << applicability.str()
        << ",\"collector_status\":{\"engine-perfetto\":{\"status\":\""
        << ((std::strcmp(status, "complete") == 0) ? "complete" : "failed")
        << "\"},\"metal-timing\":{\"status\":\""
        << metal_timing_status << "\"}},\"optional_collectors\":" << optional.str()
        << ",\"sdk\":{\"enabled\":" << (DXMT9_HAS_PERFETTO ? "true" : "false")
        << ",\"revision\":\"" << kSdkRevision << "\",\"digest\":\""
        << kSdkDigest << "\",\"license\":\"Apache-2.0\"}"
        << ",\"gpu\":{\"requested\":" << (s.gpu_requested ? "true" : "false")
        << ",\"required\":" << (s.gpu_required ? "true" : "false")
        << ",\"attached\":" << (s.gpu_initialized ? "true" : "false")
        << ",\"status\":\""
        << (!s.gpu_requested ? "not_requested"
            : (s.gpu_initialized ? "attached"
               : (s.gpu_attach_attempted ? "unavailable" : "not_attached")))
        << "\"},\"flush\":{\"status\":\"" << flush_status << "\"}"
        << ",\"artifact\":{\"path\":\"" << escape_json(s.pftrace_path)
        << "\",\"bytes\":" << (artifact_exists ? static_cast<uint64_t>(artifact_stat.st_size) : 0)
        << ",\"sha256\":\"" << (artifact_exists ? sha256_file(s.pftrace_path) : "")
        << "\"},\"raw_artifact\":{\"path\":\"" << escape_json(s.raw_path)
        << "\",\"bytes\":" << (raw_exists ? static_cast<uint64_t>(raw_stat.st_size) : 0)
        << ",\"sha256\":\"" << (raw_exists ? sha256_file(s.raw_path) : "")
        << "\"}}\n";
    const std::string text = out.str();
    detail::AtomicArtifact artifact(s.receipt_path);
    return artifact.open() && artifact.write(text.data(), text.size()) &&
        artifact.commit();
  } catch (...) {
    return false;
  }
}

void append_gpu_sample(void* context, const trace::gpu::Sample& sample) noexcept;
void append_gpu_stage_sample(void* context,
                             const trace::gpu::StageSample& sample) noexcept;
void poll_and_drain_gpu(Session& s) noexcept;

void poll_and_drain_gpu(Session& s) noexcept {
  if (!s.gpu_initialized) return;
  (void)trace::gpu::pollProcessGpuTrace();
  (void)trace::gpu::drainProcessGpuTrace(append_gpu_sample, &s);
  (void)trace::gpu::drainProcessGpuStageSamples(append_gpu_stage_sample, &s);
}

bool finalize(Session& s) noexcept {
  bool expected = false;
  if (!s.finalizer_claimed.compare_exchange_strong(expected, true)) return false;
  disable_owner_registration();
  closeAdmission(s.admission_gate);
  s.state.store(TraceState::Closing, std::memory_order_release);
  g_trace_enabled.store(false, std::memory_order_release);
  g_trace_detail_enabled.store(false, std::memory_order_relaxed);
  s.stopped_ns = now_ns();
  s.state.store(TraceState::Draining, std::memory_order_release);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(s.deadline_ms);
  while (admissionWriters(s.admission_gate.load(std::memory_order_acquire)) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  if (!maySeal(TraceState::Draining,
               admissionWriters(s.admission_gate.load(std::memory_order_acquire)))) {
    s.state.store(TraceState::Failed, std::memory_order_release);
    publish_receipt(s, "incomplete", "incomplete", 0, 0, 0, false);
    return false;
  }
  if (s.gpu_required && !s.gpu_initialized) {
    s.state.store(TraceState::Failed, std::memory_order_release);
    publish_receipt(s, "incomplete", "incomplete", 0, 0, 0, false);
    return false;
  }
  if (s.gpu_initialized) {
    trace::gpu::closeProcessGpuTrace();
    trace::gpu::closeProcessGpuStageSamples();
    do {
      poll_and_drain_gpu(s);
      if (trace::gpu::processGpuTraceDrained() &&
          trace::gpu::processGpuStageSamplesDrained()) break;
      std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    if (!trace::gpu::processGpuTraceDrained() ||
        !trace::gpu::processGpuStageSamplesDrained()) {
      s.state.store(TraceState::Failed, std::memory_order_release);
      publish_receipt(s, "incomplete", "incomplete", 0, 0, 0, false);
      return false;
    }
  }
  s.storage_sealed.store(true, std::memory_order_release);
  try {
  const uint32_t count = std::min(s.next_thread.load(), s.max_threads);
  std::vector<TraceEvent> events;
  size_t total = 0;
  for (uint32_t i = 0; i < count; ++i) total += s.threads[i].count;
  events.reserve(total);
  for (uint32_t i = 0; i < count; ++i) {
    const ThreadBuffer& t = s.threads[i];
    events.insert(events.end(), t.events, t.events + t.count);
  }
  std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
    if (a.timestamp_ns != b.timestamp_ns) return a.timestamp_ns < b.timestamp_ns;
    return a.source_record < b.source_record;
  });
  if (!assign_gpu_tracks(events,
                         s.generation.load(std::memory_order_relaxed),
                         deadline)) {
    s.state.store(TraceState::Failed, std::memory_order_release);
    publish_receipt(s, "incomplete", "incomplete", 0, 0, 0, false);
    return false;
  }
  s.state.store(TraceState::Finalizing, std::memory_order_release);
  const bool raw_ok = write_raw(s, deadline);
#if DXMT9_HAS_PERFETTO
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now()).count();
  const uint32_t remaining_ms = remaining > 0
      ? static_cast<uint32_t>(std::min<int64_t>(remaining, UINT32_MAX)) : 0;
  const detail::ExportResult export_result = detail::export_pftrace(
      events, s.pftrace_path.c_str(), kSdkRevision, s.max_bytes,
      remaining_ms);
  const bool trace_ok = export_result.success;
#else
  const detail::ExportResult export_result{};
  const bool trace_ok = false;
#endif
  const trace::gpu::Counters gpu_counters = s.gpu_initialized
      ? trace::gpu::processGpuTraceCounters() : trace::gpu::Counters{};
  const trace::gpu::StageCounters stage_counters = s.gpu_initialized
      ? trace::gpu::processGpuStageCounters() : trace::gpu::StageCounters{};
  const bool no_gpu_loss = gpu_counters.lostCapacity == 0 &&
      gpu_counters.invalid == 0 && gpu_counters.stale == 0 &&
      stage_counters.lostCapacity == 0 && stage_counters.invalid == 0 &&
      stage_counters.stale == 0;
  const bool no_sdk_loss = export_result.lost_packets == 0 &&
      export_result.overwritten_bytes == 0 && export_result.discarded_chunks == 0;
  const bool ok = s.capture_started && raw_ok && trace_ok &&
      s.total_dropped.load() == 0 &&
      no_gpu_loss && no_sdk_loss;
  s.state.store(ok ? TraceState::Sealed : TraceState::Failed,
                std::memory_order_release);
  const bool receipt_ok = publish_receipt(s, ok ? "complete" : "incomplete",
                  export_result.flush_complete ? "complete" : "incomplete",
                  export_result.lost_packets,
                  export_result.overwritten_bytes,
                  export_result.discarded_chunks);
  if (!receipt_ok) s.state.store(TraceState::Failed, std::memory_order_release);
  return ok && receipt_ok;
  } catch (...) {
    s.state.store(TraceState::Failed, std::memory_order_release);
    s.stopped_ns = now_ns();
    publish_receipt(s, "incomplete", "failed", 0, 0, 0, false);
    return false;
  }
}

}  // namespace

uint64_t now_ns() noexcept {
  static mach_timebase_info_data_t scale = [] {
    mach_timebase_info_data_t value{};
    mach_timebase_info(&value);
    return value;
  }();
  const __uint128_t nanoseconds =
      (static_cast<__uint128_t>(mach_absolute_time()) * scale.numer) /
      scale.denom;
  return nanoseconds > UINT64_MAX ? UINT64_MAX
                                  : static_cast<uint64_t>(nanoseconds);
}

uint64_t native_thread_id() noexcept {
  return native_tid();
}

namespace {

struct WriterLease {
  std::atomic<uint64_t>& gate;
  ~WriterLease() { releaseWriter(gate); }
};

void record_loss_for_generation(uint64_t generation, uint64_t count) noexcept;

bool append_finalizer_event(Session& s, TraceEvent event) noexcept {
  const uint32_t index = thread_slot(s);
  if (index == UINT32_MAX) return false;
  ThreadBuffer& thread = s.threads[index];
  if (thread.count == thread.capacity) {
    ++thread.dropped;
    s.total_dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!event.source_record)
    event.source_record = s.source_order.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!event.thread_id) event.thread_id = thread.native_tid;
  if (!event.track_id) event.track_id = thread.track_id;
  if (!event.process_id) event.process_id = s.process_id;
  thread.events[thread.count++] = event;
  return true;
}

void append_gpu_sample(void* context, const trace::gpu::Sample& sample) noexcept {
  auto& s = *static_cast<Session*>(context);
  if (sample.validity != trace::gpu::ResultValidity::Valid ||
      sample.gpuStartNs == 0 || sample.gpuEndNs <= sample.gpuStartNs) {
    s.total_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const bool selectedLevel = sample.level == s.gpu_level;
  if (selectedLevel)
    s.gpu_valid_samples.fetch_add(1, std::memory_order_relaxed);
  if (!sample.calibrationApplies || !sample.calibration.valid ||
      sample.calibration.scale <= 0.0) return;
  const uint64_t scopeId = sample.work.commandBufferId
      ? sample.work.commandBufferId : sample.work.sequenceId;
  TraceEvent begin{};
  begin.timestamp_ns = sample.gpuStartNs;
  begin.clock_id = 3;
  begin.source_timestamp = sample.gpuStartNs;
  begin.work_id = sample.work.sequenceId;
  begin.scope_id = scopeId;
  begin.arg0 = sample.work.commandBufferId;
  begin.related_owner_id = sample.work.queueGeneration;
  begin.device_generation = sample.work.deviceGeneration;
  begin.lane_id = sample.work.queueGeneration;
  begin.track_id = gpu_track_id(s.generation.load(std::memory_order_relaxed));
  begin.thread_id = 0;
  begin.uncertainty_ns = sample.calibration.uncertaintyNs;
  begin.source_clock_id = sample.sourceClockId;
  begin.calibration_id = sample.calibration.mappingId;
  begin.calibration_scale = sample.calibration.scale;
  begin.calibration_offset = sample.calibration.offset;
  begin.calibration_source_first = sample.calibration.firstSourceObservation;
  begin.calibration_host_first_ns = sample.calibration.firstHostObservationNs;
  begin.calibration_source_second = sample.calibration.secondSourceObservation;
  begin.calibration_host_second_ns = sample.calibration.secondHostObservationNs;
  begin.calibration_valid_begin = sample.calibration.validBegin;
  begin.calibration_valid_end = sample.calibration.validEnd;
  begin.event_id = static_cast<uint16_t>(EventId::GpuCommandBufferBegin);
  begin.category = static_cast<uint8_t>(Category::Gpu);
  begin.phase = static_cast<uint8_t>(Phase::Begin);
  TraceEvent end = begin;
  end.timestamp_ns = sample.gpuEndNs;
  end.source_timestamp = sample.gpuEndNs;
  end.event_id = static_cast<uint16_t>(EventId::GpuCommandBufferEnd);
  end.phase = static_cast<uint8_t>(Phase::End);
  if (append_finalizer_event(s, begin) && append_finalizer_event(s, end) &&
      selectedLevel)
    s.gpu_calibrated_samples.fetch_add(1, std::memory_order_relaxed);
}

void append_gpu_stage_sample(void* context,
                             const trace::gpu::StageSample& sample) noexcept {
  auto& s = *static_cast<Session*>(context);
  if (sample.result.validity != trace::gpu::ResultValidity::Valid) {
    s.total_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const bool selectedLevel = s.gpu_level == trace::gpu::Level::EncoderStage;
  if (selectedLevel)
    s.gpu_valid_samples.fetch_add(1, std::memory_order_relaxed);
  if (!sample.calibrationApplies || sample.calibration.scale <= 0.0) return;
  const auto toNs = [&](uint64_t tick) {
    const long double ns = static_cast<long double>(sample.calibration.scale) *
        tick + static_cast<long double>(sample.calibration.offset);
    return ns <= 0 || ns >= std::ldexp(1.0L, 64)
        ? uint64_t{0} : static_cast<uint64_t>(ns);
  };
  const uint64_t start = toNs(sample.result.startTimestamp);
  const uint64_t end = toNs(sample.result.endTimestamp);
  if (!start || end <= start) {
    s.total_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // Stage pair indices are unique while reserved. Give their bounded index a
  // disjoint scope namespace instead of hashing slot generations, which can
  // collide with command-buffer scopes.
  const uint64_t scopeId = (uint64_t{1} << 63) | sample.pairIndex;
  TraceEvent begin{};
  begin.timestamp_ns = start;
  begin.clock_id = 3;
  begin.source_timestamp = sample.result.startTimestamp;
  begin.work_id = sample.work.sequenceId;
  begin.scope_id = scopeId;
  begin.arg0 = sample.work.commandBufferId;
  begin.related_owner_id = sample.work.queueGeneration;
  begin.device_generation = sample.work.deviceGeneration;
  begin.lane_id = sample.work.queueGeneration;
  begin.track_id = gpu_track_id(s.generation.load(std::memory_order_relaxed));
  begin.thread_id = 0;
  begin.uncertainty_ns = sample.calibration.uncertaintyNs;
  begin.source_clock_id = sample.calibration.sourceClockId
      ? sample.calibration.sourceClockId : sample.sourceClockId;
  begin.calibration_id = sample.calibration.mappingId;
  begin.calibration_scale = sample.calibration.scale;
  begin.calibration_offset = sample.calibration.offset;
  begin.calibration_source_first = sample.calibration.firstSourceObservation;
  begin.calibration_host_first_ns = sample.calibration.firstHostObservationNs;
  begin.calibration_source_second = sample.calibration.secondSourceObservation;
  begin.calibration_host_second_ns = sample.calibration.secondHostObservationNs;
  begin.calibration_valid_begin = sample.calibration.validBegin;
  begin.calibration_valid_end = sample.calibration.validEnd;
  begin.event_id = static_cast<uint16_t>(EventId::GpuStageBegin);
  begin.category = static_cast<uint8_t>(Category::Gpu);
  begin.phase = static_cast<uint8_t>(Phase::Begin);
  TraceEvent finish = begin;
  finish.timestamp_ns = end;
  finish.source_timestamp = sample.result.endTimestamp;
  finish.event_id = static_cast<uint16_t>(EventId::GpuStageEnd);
  finish.phase = static_cast<uint8_t>(Phase::End);
  if (append_finalizer_event(s, begin) && append_finalizer_event(s, finish) &&
      selectedLevel)
    s.gpu_calibrated_samples.fetch_add(1, std::memory_order_relaxed);
}

bool emit_batch_for_generation(std::span<const TraceEvent> inputs,
                               uint64_t requestedGeneration,
                               uint64_t lossCount = 0) noexcept {
  if (inputs.empty()) return true;
  Session& s = session();
  const TraceState state = s.state.load(std::memory_order_acquire);
  if (!tryAcquireWriter(s.admission_gate, state, requestedGeneration)) {
    return false;
  }
  WriterLease writer{s.admission_gate};
  if (s.generation.load(std::memory_order_relaxed) != requestedGeneration ||
      s.storage_sealed.load(std::memory_order_acquire)) {
    return false;
  }
  if (lossCount) s.total_dropped.fetch_add(lossCount, std::memory_order_relaxed);
  size_t accepted = 0;
  for (const TraceEvent& input : inputs) {
    if (input.category < static_cast<uint8_t>(Category::Count) &&
        (s.category_mask & (uint32_t{1} << input.category)))
      ++accepted;
  }
  if (accepted == 0) return true;
  const uint32_t index = thread_slot(s);
  if (index == UINT32_MAX) return false;
  ThreadBuffer& thread = s.threads[index];
  if (accepted > thread.capacity - thread.count) {
    ++thread.dropped;
    s.total_dropped.fetch_add(accepted, std::memory_order_relaxed);
    return false;
  }
  for (const TraceEvent& input : inputs) {
    if (input.category >= static_cast<uint8_t>(Category::Count) ||
        !(s.category_mask & (uint32_t{1} << input.category)))
      continue;
    TraceEvent event = input;
    if (!event.timestamp_ns) event.timestamp_ns = now_ns();
    if (!event.clock_id) event.clock_id = 3;
    if (!event.thread_id) event.thread_id = thread.native_tid;
    if (!event.track_id) event.track_id = thread.track_id;
    if (!event.process_id) event.process_id = s.process_id;
    if (!event.source_record)
      event.source_record = s.source_order.fetch_add(1, std::memory_order_relaxed) + 1;
    thread.events[thread.count++] = event;
  }
  return true;
}

}  // namespace

bool prepare_and_start(const TraceConfig& config) noexcept {
#if !DXMT9_HAS_PERFETTO
  (void)config;
  report_start_rejection("Perfetto SDK is disabled in this build");
  return false;
#else
  const char* env = std::getenv("DXMT9_TRACE_CONFIG");
  const std::string request_path = config.request_path ? config.request_path : (env ? env : "");
  if (request_path.empty()) {
    report_start_rejection("DXMT9_TRACE_CONFIG is unset");
    return false;
  }
  Session& s = session();
  std::unique_lock<std::mutex> lock(s.control);
  try {
  const TraceState old = s.state.load();
  if (old != TraceState::Idle && old != TraceState::Sealed && old != TraceState::Failed) {
    if (old != TraceState::Preparing)
      report_start_rejection("session is already active");
    return false;
  }
  if (admissionWriters(s.admission_gate.load(std::memory_order_acquire)) != 0) {
    report_start_rejection("session still has admitted writers");
    return false;
  }
  s.state.store(TraceState::Preparing, std::memory_order_release);
  struct PreparationGuard {
    Session& session;
    ~PreparationGuard() {
      if (session.state.load(std::memory_order_acquire) == TraceState::Preparing)
        session.state.store(TraceState::Failed, std::memory_order_release);
    }
  } preparationGuard{s};
  std::thread previousController;
  if (s.controller.joinable()) previousController = std::move(s.controller);
  s.condition.notify_all();
  lock.unlock();
  if (previousController.joinable()) previousController.join();
  lock.lock();
  if (s.state.load(std::memory_order_acquire) != TraceState::Preparing ||
      admissionWriters(s.admission_gate.load(std::memory_order_acquire)) != 0) {
    report_start_rejection("session preparation was interrupted");
    return false;
  }
  ParsedConfig parsed;
  std::string parseError;
  if (!parse_request(request_path, parsed, parseError)) {
    report_start_rejection("invalid trace request", parseError.c_str());
    return false;
  }
  s.run_id = parsed.run_id;
  s.attempt_id = parsed.attempt_id;
  s.profile_id = parsed.profile_id;
  s.question = parsed.question;
  s.config_sha256 = parsed.config_sha256;
  s.categories_json = json_array(parsed.categories);
  s.required_categories_json = json_array(parsed.required_categories);
  s.categories = parsed.categories;
  s.required_categories = parsed.required_categories;
  s.category_mask = 0;
  for (const auto& category : parsed.categories) {
    if (category == "import") s.category_mask |= 1u << static_cast<uint8_t>(Category::Import);
    else if (category == "replay") s.category_mask |= 1u << static_cast<uint8_t>(Category::Replay);
    else if (category == "queue") s.category_mask |= 1u << static_cast<uint8_t>(Category::Queue);
    else if (category == "encode") s.category_mask |= 1u << static_cast<uint8_t>(Category::Encode);
    else if (category == "wait") s.category_mask |= 1u << static_cast<uint8_t>(Category::Wait);
    else if (category == "submit") s.category_mask |= 1u << static_cast<uint8_t>(Category::Submit);
    else if (category == "completion") s.category_mask |= 1u << static_cast<uint8_t>(Category::Completion);
    else if (category == "pe") s.category_mask |= 1u << static_cast<uint8_t>(Category::Pe);
    else if (category == "gpu") s.category_mask |= 1u << static_cast<uint8_t>(Category::Gpu);
    else if (category == "detail") s.category_mask |= 1u << static_cast<uint8_t>(Category::Detail);
  }
  s.optional_collectors = parsed.optional_collectors;
  s.pftrace_path = config.pftrace_path ? config.pftrace_path : parsed.pftrace_path;
  s.receipt_path = config.receipt_path ? config.receipt_path : parsed.receipt_path;
  s.raw_path = config.raw_path ? config.raw_path : parsed.raw_path;
  s.max_bytes = std::min(parsed.max_bytes, kMaxBytes);
  s.max_events = std::min(parsed.max_events, kMaxEvents);
  s.max_events = std::min(s.max_events,
                          std::max(1u, s.max_bytes /
                              static_cast<uint32_t>(sizeof(TraceEvent))));
  s.max_threads = std::min(kMaxThreads, std::max(1u, s.max_events));
  s.deadline_ms = std::min(parsed.finalize_timeout_ms, kDeadlineMs);
  s.max_clock_error_ns = parsed.max_clock_error_ns;
  s.delay_ms = parsed.delay_ms;
  s.duration_ms = parsed.duration_ms;
  s.gpu_requested = parsed.gpu_requested;
  s.gpu_required = parsed.gpu_required;
  s.gpu_capacity = parsed.gpu_capacity;
  s.gpu_calibration = parsed.gpu_calibration;
  s.gpu_level = parsed.gpu_level == "encoder-stage"
      ? trace::gpu::Level::EncoderStage : trace::gpu::Level::CommandBuffer;
  s.gpu_initialized = false;
  s.gpu_attach_attempted = false;
  s.gpu_valid_samples.store(0, std::memory_order_relaxed);
  s.gpu_calibrated_samples.store(0, std::memory_order_relaxed);
  s.started_ns = 0;
  s.stopped_ns = 0;
  s.capture_started = false;
  if (s.max_events < s.max_threads || s.max_bytes < sizeof(TraceEvent)) {
    report_start_rejection("configured event storage limits are too small");
    return false;
  }
  try {
    s.storage = std::make_unique<TraceEvent[]>(s.max_events);
  } catch (...) {
    report_start_rejection("event storage allocation failed");
    return false;
  }
  const uint32_t base = s.max_events / s.max_threads;
  uint32_t offset = 0;
  for (uint32_t i = 0; i < s.max_threads; ++i) {
    ThreadBuffer& t = s.threads[i];
    t = {};
    t.events = s.storage.get() + offset;
    t.capacity = base + (i < s.max_events % s.max_threads ? 1 : 0);
    offset += t.capacity;
  }
  s.process_id = static_cast<uint32_t>(getpid());
  const uint64_t previousGeneration = s.generation.load(std::memory_order_relaxed);
  if (previousGeneration >= kAdmissionGateGenerationMask) {
    report_start_rejection("session generation space exhausted");
    return false;
  }
  const uint64_t generation = previousGeneration + 1;
  s.generation.store(generation, std::memory_order_relaxed);
  s.next_thread.store(0);
  s.source_order.store(0);
  s.total_dropped.store(0);
  s.admission_gate.store(makeAdmissionGate(generation, 0, true),
                         std::memory_order_relaxed);
  s.storage_sealed.store(false);
  s.finalizer_claimed.store(false);
  enable_owner_registration(generation);
  s.state.store(TraceState::Armed, std::memory_order_release);
  g_trace_detail_enabled.store(false, std::memory_order_relaxed);
  const bool detail_enabled = std::find(parsed.categories.begin(),
                                        parsed.categories.end(), "detail") !=
                              parsed.categories.end();
  try {
    s.controller = std::thread([&s, generation, detail_enabled] {
      std::unique_lock<std::mutex> lock(s.control);
      if (s.condition.wait_for(lock, std::chrono::milliseconds(s.delay_ms), [&] {
            return s.generation.load() != generation ||
                   s.state.load() != TraceState::Armed;
          }))
        return;
      s.started_ns = now_ns();
      s.capture_started = true;
      s.state.store(TraceState::Collecting, std::memory_order_release);
      openAdmission(s.admission_gate, generation);
      g_trace_detail_enabled.store(detail_enabled, std::memory_order_relaxed);
      g_trace_enabled.store(true, std::memory_order_release);
      const auto collectionEnd = std::chrono::steady_clock::now() +
          std::chrono::milliseconds(s.duration_ms);
      for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= collectionEnd) break;
        const auto pollInterval = std::chrono::milliseconds(10);
        const auto remaining = collectionEnd - now;
        const auto interval = std::min(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(pollInterval),
            remaining);
        if (s.condition.wait_for(lock, interval, [&] {
              return s.generation.load() != generation ||
                     s.state.load() != TraceState::Collecting;
            }))
          return;
        poll_and_drain_gpu(s);
      }
      (void)finalize(s);
    });
  } catch (...) {
    disable_owner_registration();
    report_start_rejection("controller thread creation failed");
    s.state.store(TraceState::Failed, std::memory_order_release);
    return false;
  }
  return true;
  } catch (...) {
    disable_owner_registration();
    report_start_rejection("exception while preparing trace session");
    g_trace_enabled.store(false, std::memory_order_release);
    g_trace_detail_enabled.store(false, std::memory_order_relaxed);
    s.state.store(TraceState::Failed, std::memory_order_release);
    return false;
  }
#endif
}

bool initialize_from_environment() noexcept {
  const char* env = std::getenv("DXMT9_TRACE_CONFIG");
  if (!env || !*env) return false;
  Session& s = session();
  const TraceState state = s.state.load(std::memory_order_acquire);
  if (state == TraceState::Armed || state == TraceState::Collecting) return true;
  if (prepare_and_start()) return true;
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(kDeadlineMs);
  for (;;) {
    const TraceState after = s.state.load(std::memory_order_acquire);
    if (after == TraceState::Armed || after == TraceState::Collecting)
      return true;
    if (after != TraceState::Preparing ||
        std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::yield();
  }
}

bool session_configured() noexcept {
  const TraceState state = session().state.load(std::memory_order_acquire);
  return state == TraceState::Armed || state == TraceState::Collecting;
}

bool attach_metal_device(uintptr_t device_handle) noexcept {
  if (!device_handle) return false;
  Session& s = session();
  std::lock_guard<std::mutex> lock(s.control);
  const TraceState state = s.state.load(std::memory_order_acquire);
  if (!s.gpu_requested || s.gpu_attach_attempted ||
      (state != TraceState::Armed && state != TraceState::Collecting)) {
    return false;
  }
  s.gpu_attach_attempted = true;
  trace::gpu::SessionConfig gpuConfig{};
  gpuConfig.sessionGeneration = s.generation.load(std::memory_order_relaxed);
  gpuConfig.capacity = std::min<uint32_t>(
      trace::gpu::kMaxSamples, std::max(1u, s.gpu_capacity));
  gpuConfig.level = s.gpu_level;
  gpuConfig.required = s.gpu_required;
  if (s.gpu_calibration) {
    gpuConfig.calibration = trace::gpu::commandBufferHostTimeCalibration(
        gpuConfig.sessionGeneration, s.started_ns, UINT64_MAX, 0);
  }
  s.gpu_initialized = trace::gpu::initializeProcessGpuTraceHandle(
      static_cast<std::uintptr_t>(device_handle), gpuConfig);
  if (!s.gpu_initialized && s.gpu_required)
    report_start_rejection("required Metal GPU trace attachment failed");
  return s.gpu_initialized || !s.gpu_required;
}

void counter(Category category, Metric metric, uint64_t value,
             uint64_t owner, uint64_t work) noexcept {
  if (value > static_cast<uint64_t>(INT64_MAX)) {
    record_loss(0);
    return;
  }
  (void)emit({.work_id = work, .arg0 = owner, .metric_value = value,
      .metric_id = static_cast<uint16_t>(metric),
      .event_id = static_cast<uint16_t>(EventId::Metric),
      .category = static_cast<uint8_t>(category),
      .phase = static_cast<uint8_t>(Phase::Counter)});
}

bool emit(const TraceEvent& input) noexcept {
  return emit_batch(std::span<const TraceEvent>(&input, 1));
}

bool emit_batch(std::span<const TraceEvent> inputs, uint64_t lossCount) noexcept {
  if (inputs.empty()) return true;
  Session& s = session();
  const uint64_t requestedGeneration =
      captureAdmissionGeneration(s.admission_gate);
  return requestedGeneration != 0 &&
      emit_batch_for_generation(inputs, requestedGeneration, lossCount);
}

uint64_t capture_generation() noexcept {
  return captureAdmissionGeneration(session().admission_gate);
}

bool emit_batch_captured(std::span<const TraceEvent> inputs,
                         uint64_t generation,
                         uint64_t lossCount) noexcept {
  return generation != 0 &&
      emit_batch_for_generation(inputs, generation, lossCount);
}

TraceScope::TraceScope(uint8_t category, uint16_t begin_event,
                       uint16_t end_event, uint64_t track_id,
                       uint64_t work_id, uint64_t owner_id,
                       uint64_t related_owner_id) noexcept {
  if (!enabled()) return;
  Session& s = session();
  const uint64_t generation = captureAdmissionGeneration(s.admission_gate);
  if (!generation) return;
  uint32_t index = UINT32_MAX;
  const uint32_t start = scope_cursor.fetch_add(1, std::memory_order_relaxed);
  for (uint32_t attempt = 0; attempt < kMaxScopes; ++attempt) {
    const uint32_t candidate = (start + attempt) % kMaxScopes;
    uint8_t expected = 0;
    if (scope_entries[candidate].state.compare_exchange_strong(
            expected, 1, std::memory_order_acquire,
            std::memory_order_relaxed)) {
      index = candidate;
      break;
    }
  }
  if (index == UINT32_MAX) {
    record_loss_for_generation(generation, 1);
    return;
  }
  ScopeEntry& scope = scope_entries[index];
  uint64_t token = scope.token.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((token & 0x0000ffffffffffffull) == 0) {
    token = scope.token.fetch_add(1, std::memory_order_relaxed) + 1;
  }
  scope.generation = generation;
  scope.track_id = track_id;
  scope.work_id = work_id;
  scope.owner_id = owner_id;
  scope.related_owner_id = related_owner_id;
  scope.end_event = end_event;
  scope.category = category;
  handle = ((token & 0x0000ffffffffffffull) << 16) | index;
  scope.state.store(2, std::memory_order_release);
  TraceEvent event{};
  event.timestamp_ns = now_ns();
  event.clock_id = 3;
  event.track_id = track_id;
  event.work_id = work_id;
  event.scope_id = handle;
  event.arg0 = owner_id;
  event.arg1 = related_owner_id;
  event.event_id = begin_event;
  event.category = category;
  event.phase = static_cast<uint8_t>(Phase::Begin);
  (void)emit_batch_for_generation(std::span<const TraceEvent>(&event, 1),
                                  generation);
}

TraceScope::TraceScope(TraceScope&& other) noexcept
    : handle(std::exchange(other.handle, 0)) {}

TraceScope& TraceScope::operator=(TraceScope&& other) noexcept {
  if (this != &other) {
    if (handle) end_scope(handle);
    handle = std::exchange(other.handle, 0);
  }
  return *this;
}

void end_scope(uint64_t handle) noexcept {
  if (!handle) return;
  const uint32_t index = static_cast<uint32_t>(handle & 0xffffu);
  const uint64_t token = handle >> 16;
  if (index >= kMaxScopes || token == 0) return;
  ScopeEntry& entry = scope_entries[index];
  uint8_t expected = 2;
  if (!entry.state.compare_exchange_strong(expected, 1,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed)) return;
  const uint64_t currentToken = entry.token.load(std::memory_order_relaxed) &
      0x0000ffffffffffffull;
  if (currentToken != token) {
    entry.state.store(2, std::memory_order_release);
    return;
  }
  ScopeEntry scope;
  scope.generation = entry.generation;
  scope.track_id = entry.track_id;
  scope.work_id = entry.work_id;
  scope.owner_id = entry.owner_id;
  scope.related_owner_id = entry.related_owner_id;
  scope.end_event = entry.end_event;
  scope.category = entry.category;
  const bool canEmit = enabled() &&
      scope.generation == session().generation.load();
  if (!canEmit) {
    entry.state.store(0, std::memory_order_release);
    return;
  }
  TraceEvent event{};
  event.timestamp_ns = now_ns();
  event.clock_id = 3;
  event.track_id = scope.track_id;
  event.work_id = scope.work_id;
  event.scope_id = handle;
  event.arg0 = scope.owner_id;
  event.arg1 = scope.related_owner_id;
  event.event_id = scope.end_event;
  event.category = scope.category;
  event.phase = static_cast<uint8_t>(Phase::End);
  (void)emit_batch_for_generation(std::span<const TraceEvent>(&event, 1),
                                  scope.generation);
  entry.state.store(0, std::memory_order_release);
}

void record_loss(uint32_t, uint64_t count) noexcept {
  if (!count) return;
  record_loss_for_generation(capture_generation(), count);
}

namespace {

void record_loss_for_generation(uint64_t requestedGeneration,
                                uint64_t count) noexcept {
  if (!count || !requestedGeneration) return;
  Session& s = session();
  if (!requestedGeneration) return;
  const TraceState state = s.state.load(std::memory_order_acquire);
  if (!tryAcquireWriter(s.admission_gate, state, requestedGeneration)) return;
  WriterLease writer{s.admission_gate};
  if (s.generation.load(std::memory_order_relaxed) == requestedGeneration &&
      !s.storage_sealed.load(std::memory_order_acquire))
    s.total_dropped.fetch_add(count, std::memory_order_relaxed);
}

}  // namespace

bool stop_and_finalize() noexcept {
  return stop_and_finalize(0);
}

bool stop_and_finalize(uint64_t expectedGeneration) noexcept {
  Session& s = session();
  s.condition.notify_all();
  bool result = false;
  std::thread controller;
  {
    std::lock_guard<std::mutex> lock(s.control);
    if (expectedGeneration &&
        s.generation.load(std::memory_order_acquire) != expectedGeneration)
      return false;
    const TraceState state = s.state.load(std::memory_order_acquire);
    if (state != TraceState::Armed && state != TraceState::Collecting)
      return state == TraceState::Sealed;
    if (state == TraceState::Armed) {
      s.started_ns = now_ns();
      s.capture_started = false;
    }
    result = finalize(s);
    if (s.controller.joinable() &&
        s.controller.get_id() != std::this_thread::get_id())
      controller = std::move(s.controller);
  }
  s.condition.notify_all();
  if (controller.joinable()) controller.join();
  return result;
}

}  // namespace dxmt9::tracing
