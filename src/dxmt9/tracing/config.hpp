#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dxmt9::tracing {

struct ParsedConfig {
  std::string run_id;
  std::string attempt_id;
  std::string profile_id;
  std::string question;
  std::string config_sha256;
  std::string pftrace_path;
  std::string receipt_path;
  std::string raw_path;
  std::vector<std::string> categories;
  std::vector<std::string> required_categories;
  std::vector<std::string> optional_collectors;
  uint32_t delay_ms = 0;
  uint32_t duration_ms = 0;
  uint32_t max_events = 0;
  uint32_t max_bytes = 0;
  uint32_t finalize_timeout_ms = 0;
  uint32_t max_clock_error_ns = 1000;
  bool gpu_requested = false;
  bool gpu_required = false;
  uint32_t gpu_capacity = 0;
  std::string gpu_level = "command-buffer";
  bool gpu_calibration = false;
};

// Parses and validates the bounded runner request. Rejects duplicate keys,
// malformed types, digest mismatch, path collisions, and existing outputs.
bool parse_request(const std::string& request_path, ParsedConfig& output,
                   std::string& error) noexcept;

}  // namespace dxmt9::tracing
