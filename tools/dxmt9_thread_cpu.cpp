#include <libproc.h>

#include <mach/mach.h>
#include <mach/thread_info.h>
#include <pthread.h>

#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <time.h>
#include <unistd.h>

namespace {

constexpr int kNoFlavor = 0;
constexpr uint64_t kNanosecondsPerSecond = 1000000000ULL;
constexpr uint64_t kCalibrationToleranceNs = 10000000ULL;

struct QueryFailure {
  int flavor = kNoFlavor;
  int returned_bytes = 0;
  int saved_errno = 0;
};

struct ProcessIdentity {
  uint32_t pid = 0;
  uint64_t start_sec = 0;
  uint64_t start_usec = 0;
};

struct ThreadSample {
  uint64_t user_ns = 0;
  uint64_t system_ns = 0;
  int32_t state = 0;
};

struct MachSample {
  uint64_t user_ns = 0;
  uint64_t system_ns = 0;
  int32_t state = 0;
};

void print_status_error(const char* status, const char* kind, const char* message,
                        const QueryFailure& failure, int expected_bytes) {
  std::printf(
      "{\"schema_version\":1,\"status\":\"");
  std::printf("%s", status);
  std::printf(
      "\",\"units\":\"nanoseconds\","
      "\"error\":{\"kind\":\"");
  std::printf("%s", kind);
  std::printf("\",\"message\":\"");
  std::printf("%s", message);
  std::printf(
      "\",\"flavor\":%d,\"returned_bytes\":%d,\"expected_bytes\":%d,"
      "\"errno\":%d}}\n",
      failure.flavor, failure.returned_bytes, expected_bytes, failure.saved_errno);
}

void print_error(const char* kind, const char* message, const QueryFailure& failure,
                 int expected_bytes) {
  print_status_error("error", kind, message, failure, expected_bytes);
}

bool monotonic_now(uint64_t* value, QueryFailure* failure) {
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    failure->saved_errno = errno;
    return false;
  }
  if (now.tv_sec < 0 || static_cast<uint64_t>(now.tv_sec) >
                            (std::numeric_limits<uint64_t>::max() -
                             static_cast<uint64_t>(now.tv_nsec)) /
                                kNanosecondsPerSecond) {
    failure->saved_errno = EOVERFLOW;
    return false;
  }
  *value = static_cast<uint64_t>(now.tv_sec) * kNanosecondsPerSecond +
           static_cast<uint64_t>(now.tv_nsec);
  return true;
}

bool timeval_to_ns(const time_value_t& value, uint64_t* result) {
  if (value.seconds < 0 || value.microseconds < 0 ||
      static_cast<uint64_t>(value.seconds) >
          (std::numeric_limits<uint64_t>::max() -
           static_cast<uint64_t>(value.microseconds) * 1000ULL) /
              kNanosecondsPerSecond) {
    return false;
  }
  *result = static_cast<uint64_t>(value.seconds) * kNanosecondsPerSecond +
            static_cast<uint64_t>(value.microseconds) * 1000ULL;
  return true;
}

template <typename T>
bool proc_query(pid_t pid, int flavor, uint64_t arg, T* info, QueryFailure* failure) {
  errno = 0;
  const int returned = proc_pidinfo(pid, flavor, arg, info, sizeof(T));
  const int saved_errno = errno;
  if (returned != static_cast<int>(sizeof(T))) {
    failure->flavor = flavor;
    failure->returned_bytes = returned;
    failure->saved_errno = saved_errno;
    return false;
  }
  return true;
}

bool query_identity(pid_t pid, ProcessIdentity* identity, QueryFailure* failure) {
  proc_bsdinfo info{};
  if (!proc_query(pid, PROC_PIDTBSDINFO, 0, &info, failure)) {
    return false;
  }
  identity->pid = info.pbi_pid;
  identity->start_sec = info.pbi_start_tvsec;
  identity->start_usec = info.pbi_start_tvusec;
  return true;
}

bool query_thread(pid_t pid, uint64_t tid, ThreadSample* sample,
                  QueryFailure* failure) {
  proc_threadinfo info{};
  if (!proc_query(pid, PROC_PIDTHREADID64INFO, tid, &info, failure)) {
    return false;
  }
  sample->user_ns = info.pth_user_time;
  sample->system_ns = info.pth_system_time;
  sample->state = info.pth_run_state;
  return true;
}

bool query_mach_thread(MachSample* sample, QueryFailure* failure) {
  mach_port_t thread = mach_thread_self();
  thread_basic_info_data_t info{};
  mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
  const kern_return_t result = thread_info(
      thread, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count);
  mach_port_deallocate(mach_task_self(), thread);
  if (result != KERN_SUCCESS || count < THREAD_BASIC_INFO_COUNT) {
    failure->flavor = THREAD_BASIC_INFO;
    failure->returned_bytes = static_cast<int>(count * sizeof(natural_t));
    failure->saved_errno = static_cast<int>(result);
    return false;
  }
  if (!timeval_to_ns(info.user_time, &sample->user_ns) ||
      !timeval_to_ns(info.system_time, &sample->system_ns)) {
    failure->flavor = THREAD_BASIC_INFO;
    failure->returned_bytes = static_cast<int>(count * sizeof(natural_t));
    failure->saved_errno = EOVERFLOW;
    return false;
  }
  sample->state = info.run_state;
  return true;
}

bool delta(uint64_t after, uint64_t before, uint64_t* result) {
  if (after < before) {
    return false;
  }
  *result = after - before;
  return true;
}

bool within_tolerance(uint64_t first, uint64_t second, uint64_t tolerance) {
  return first <= second ? second - first <= tolerance : first - second <= tolerance;
}

int print_snapshot(pid_t pid, uint64_t tid) {
  uint64_t monotonic_begin = 0;
  uint64_t monotonic_end = 0;
  QueryFailure failure{};
  if (!monotonic_now(&monotonic_begin, &failure)) {
    print_error("clock_gettime", "CLOCK_MONOTONIC query failed", failure, 0);
    return 1;
  }

  ProcessIdentity before{};
  if (!query_identity(pid, &before, &failure)) {
    print_error("proc_pidinfo", "PROC_PIDTBSDINFO query failed", failure,
                PROC_PIDTBSDINFO_SIZE);
    return 1;
  }
  if (before.pid != static_cast<uint32_t>(pid)) {
    failure.flavor = PROC_PIDTBSDINFO;
    failure.returned_bytes = PROC_PIDTBSDINFO_SIZE;
    print_error("process_identity_changed", "returned pid does not match request",
                failure, PROC_PIDTBSDINFO_SIZE);
    return 1;
  }

  ThreadSample thread{};
  if (!query_thread(pid, tid, &thread, &failure)) {
    print_error("proc_pidinfo", "PROC_PIDTHREADID64INFO query failed", failure,
                PROC_PIDTHREADID64INFO_SIZE);
    return 1;
  }

  ProcessIdentity after{};
  if (!query_identity(pid, &after, &failure)) {
    print_error("proc_pidinfo", "PROC_PIDTBSDINFO query failed", failure,
                PROC_PIDTBSDINFO_SIZE);
    return 1;
  }
  if (after.pid != before.pid || after.start_sec != before.start_sec ||
      after.start_usec != before.start_usec) {
    failure.flavor = PROC_PIDTBSDINFO;
    failure.returned_bytes = PROC_PIDTBSDINFO_SIZE;
    print_error("process_identity_changed", "pid start identity changed during query",
                failure, PROC_PIDTBSDINFO_SIZE);
    return 1;
  }
  if (!monotonic_now(&monotonic_end, &failure)) {
    print_error("clock_gettime", "CLOCK_MONOTONIC query failed", failure, 0);
    return 1;
  }

  std::printf(
      "{\"schema_version\":1,\"status\":\"ok\",\"units\":\"nanoseconds\","
      "\"pid\":%d,\"native_tid\":%" PRIu64
      ",\"start_sec\":%" PRIu64 ",\"start_usec\":%" PRIu64
      ",\"monotonic_begin_ns\":%" PRIu64
      ",\"monotonic_end_ns\":%" PRIu64 ",\"user_ns\":%" PRIu64
      ",\"system_ns\":%" PRIu64 ",\"thread_state\":%" PRId32 "}\n",
      pid, tid, before.start_sec, before.start_usec, monotonic_begin, monotonic_end,
      thread.user_ns, thread.system_ns, thread.state);
  return 0;
}

bool busy_for(uint64_t duration_ns, QueryFailure* failure) {
  uint64_t begin = 0;
  if (!monotonic_now(&begin, failure)) {
    return false;
  }
  volatile uint64_t sink = 0;
  uint64_t now = begin;
  for (uint64_t i = 0; now - begin < duration_ns; ++i) {
    sink = sink + (i ^ (sink >> 7));
    if ((i & 0x1fffU) == 0) {
      if (!monotonic_now(&now, failure) || now < begin) {
        return false;
      }
    }
  }
  (void)sink;
  return true;
}

void print_self_check_failure(const char* kind, const char* message,
                              const QueryFailure& failure, int expected_bytes) {
  print_status_error("fail", kind, message, failure, expected_bytes);
}

int self_check() {
  const pid_t pid = getpid();
  uint64_t tid = 0;
  if (pthread_threadid_np(nullptr, &tid) != 0) {
    QueryFailure failure{};
    failure.saved_errno = errno;
    print_self_check_failure("pthread_threadid_np", "native thread id query failed",
                            failure, 0);
    return 1;
  }

  ThreadSample external_before{};
  MachSample mach_before{};
  QueryFailure failure{};
  if (!query_thread(pid, tid, &external_before, &failure)) {
    print_self_check_failure("proc_pidinfo", "self thread query failed", failure,
                            PROC_PIDTHREADID64INFO_SIZE);
    return 1;
  }
  if (!query_mach_thread(&mach_before, &failure)) {
    print_self_check_failure("thread_info", "THREAD_BASIC_INFO query failed", failure,
                            THREAD_BASIC_INFO_COUNT * sizeof(natural_t));
    return 1;
  }

  if (!busy_for(125000000ULL, &failure)) {
    print_self_check_failure("clock_gettime", "busy phase clock failed", failure, 0);
    return 1;
  }

  ThreadSample external_busy{};
  MachSample mach_busy{};
  if (!query_thread(pid, tid, &external_busy, &failure)) {
    print_self_check_failure("proc_pidinfo", "busy self thread query failed", failure,
                            PROC_PIDTHREADID64INFO_SIZE);
    return 1;
  }
  if (!query_mach_thread(&mach_busy, &failure)) {
    print_self_check_failure("thread_info", "busy THREAD_BASIC_INFO query failed",
                            failure, THREAD_BASIC_INFO_COUNT * sizeof(natural_t));
    return 1;
  }

  ThreadSample external_idle_before{};
  MachSample mach_idle_before{};
  if (!query_thread(pid, tid, &external_idle_before, &failure) ||
      !query_mach_thread(&mach_idle_before, &failure)) {
    print_self_check_failure("calibration", "idle baseline query failed", failure,
                            PROC_PIDTHREADID64INFO_SIZE);
    return 1;
  }
  timespec idle_time{0, 125000000};
  nanosleep(&idle_time, nullptr);
  ThreadSample external_idle_after{};
  MachSample mach_idle_after{};
  if (!query_thread(pid, tid, &external_idle_after, &failure) ||
      !query_mach_thread(&mach_idle_after, &failure)) {
    print_self_check_failure("calibration", "idle final query failed", failure,
                            PROC_PIDTHREADID64INFO_SIZE);
    return 1;
  }

  uint64_t busy_external_user = 0;
  uint64_t busy_mach_user = 0;
  uint64_t busy_external_system = 0;
  uint64_t busy_mach_system = 0;
  uint64_t idle_external_user = 0;
  uint64_t idle_mach_user = 0;
  if (!delta(external_busy.user_ns, external_before.user_ns, &busy_external_user) ||
      !delta(mach_busy.user_ns, mach_before.user_ns, &busy_mach_user) ||
      !delta(external_busy.system_ns, external_before.system_ns, &busy_external_system) ||
      !delta(mach_busy.system_ns, mach_before.system_ns, &busy_mach_system) ||
      !delta(external_idle_after.user_ns, external_idle_before.user_ns,
             &idle_external_user) ||
      !delta(mach_idle_after.user_ns, mach_idle_before.user_ns, &idle_mach_user)) {
    print_self_check_failure("calibration", "CPU counters moved backwards", failure, 0);
    return 1;
  }

  const bool busy_positive = busy_external_user >= 20000000ULL &&
                             busy_mach_user >= 20000000ULL;
  const bool busy_calibrated =
      within_tolerance(busy_external_user, busy_mach_user, kCalibrationToleranceNs) &&
      within_tolerance(busy_external_system, busy_mach_system,
                       kCalibrationToleranceNs);
  const bool idle_calibrated =
      within_tolerance(idle_external_user, idle_mach_user, kCalibrationToleranceNs) &&
      idle_external_user <= kCalibrationToleranceNs &&
      idle_mach_user <= kCalibrationToleranceNs;

  proc_threadinfo nonexistent{};
  errno = 0;
  const int nonexistent_returned = proc_pidinfo(
      pid, PROC_PIDTHREADID64INFO, std::numeric_limits<uint64_t>::max(),
      &nonexistent, static_cast<int>(sizeof(nonexistent)));
  const int nonexistent_errno = errno;
  const bool nonexistent_failed =
      nonexistent_returned != static_cast<int>(sizeof(nonexistent));
  if (!busy_positive || !busy_calibrated || !idle_calibrated || !nonexistent_failed) {
    std::printf(
        "{\"schema_version\":1,\"status\":\"fail\",\"units\":\"nanoseconds\","
        "\"pid\":%d,\"native_tid\":%" PRIu64
        ",\"busy_external_user_ns\":%" PRIu64
        ",\"busy_mach_user_ns\":%" PRIu64
        ",\"idle_external_user_ns\":%" PRIu64
        ",\"idle_mach_user_ns\":%" PRIu64
        ",\"nonexistent_tid_returned_bytes\":%d,\"nonexistent_tid_errno\":%d}\n",
        pid, tid, busy_external_user, busy_mach_user, idle_external_user,
        idle_mach_user, nonexistent_returned, nonexistent_errno);
    return 1;
  }

  std::printf(
      "{\"schema_version\":1,\"status\":\"pass\",\"units\":\"nanoseconds\","
      "\"pid\":%d,\"native_tid\":%" PRIu64
      ",\"busy_external_user_ns\":%" PRIu64
      ",\"busy_mach_user_ns\":%" PRIu64
      ",\"busy_external_system_ns\":%" PRIu64
      ",\"busy_mach_system_ns\":%" PRIu64
      ",\"idle_external_user_ns\":%" PRIu64
      ",\"idle_mach_user_ns\":%" PRIu64
      ",\"tolerance_ns\":%" PRIu64
      ",\"nonexistent_tid_query_failed\":true}\n",
      pid, tid, busy_external_user, busy_mach_user, busy_external_system,
      busy_mach_system, idle_external_user, idle_mach_user,
      kCalibrationToleranceNs);
  return 0;
}

bool parse_u64(const char* text, uint64_t* value) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  const char* digits = text;
  int base = 10;
  if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    digits += 2;
    base = 16;
    if (*digits == '\0') {
      return false;
    }
  }
  for (const char* cursor = digits; *cursor != '\0'; ++cursor) {
    const bool decimal = *cursor >= '0' && *cursor <= '9';
    const bool hexadecimal = (*cursor >= 'a' && *cursor <= 'f') ||
                             (*cursor >= 'A' && *cursor <= 'F');
    if (!decimal && (base != 16 || !hexadecimal)) {
      return false;
    }
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(text, &end, base);
  if (errno == ERANGE || end == text || *end != '\0') {
    return false;
  }
  *value = static_cast<uint64_t>(parsed);
  return true;
}

int usage_error(const char* message) {
  QueryFailure failure{};
  failure.saved_errno = EINVAL;
  print_error("usage", message, failure, 0);
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  bool self_check_mode = false;
  bool have_pid = false;
  bool have_tid = false;
  uint64_t parsed_pid = 0;
  uint64_t parsed_tid = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--self-check") == 0) {
      if (self_check_mode || have_pid || have_tid) {
        return usage_error("--self-check cannot be combined with snapshot options");
      }
      self_check_mode = true;
    } else if (std::strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
      if (self_check_mode || have_pid || !parse_u64(argv[++i], &parsed_pid) ||
          parsed_pid == 0 || parsed_pid > static_cast<uint64_t>(std::numeric_limits<pid_t>::max())) {
        return usage_error("--pid requires a positive unsigned pid");
      }
      have_pid = true;
    } else if (std::strcmp(argv[i], "--tid") == 0 && i + 1 < argc) {
      if (self_check_mode || have_tid || !parse_u64(argv[++i], &parsed_tid) || parsed_tid == 0) {
        return usage_error("--tid requires a nonzero uint64 thread id");
      }
      have_tid = true;
    } else {
      return usage_error("expected --pid N --tid uint64 or --self-check");
    }
  }

  if (self_check_mode) {
    return self_check();
  }
  if (!have_pid || !have_tid) {
    return usage_error("both --pid and --tid are required");
  }
  return print_snapshot(static_cast<pid_t>(parsed_pid), parsed_tid);
}
