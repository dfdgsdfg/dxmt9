#include "config.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <variant>
#include <string_view>
#include <stdexcept>
#include <vector>

namespace dxmt9::tracing {
namespace {
struct Json {
  using Array = std::vector<Json>;
  using Object = std::map<std::string, Json>;
  std::variant<std::nullptr_t, bool, std::string, Array, Object> value;
  bool number = false;
};

class Parser {
 public:
  explicit Parser(std::string_view input) : input_(input) {}
  bool parse(Json& output) {
    if (!parse_value(output, 0)) return false;
    whitespace();
    return position_ == input_.size();
  }
 private:
  void whitespace() {
    while (position_ < input_.size() &&
           (input_[position_] == ' ' || input_[position_] == '\n' ||
            input_[position_] == '\r' || input_[position_] == '\t'))
      ++position_;
  }
  bool consume(char c) {
    whitespace();
    if (position_ >= input_.size() || input_[position_] != c) return false;
    ++position_;
    return true;
  }
  bool parse_string(std::string& output) {
    if (!consume('"')) return false;
    while (position_ < input_.size()) {
      unsigned char c = static_cast<unsigned char>(input_[position_++]);
      if (c == '"') return true;
      if (c < 0x20) return false;
      if (c != '\\') { output.push_back(static_cast<char>(c)); continue; }
      if (position_ == input_.size()) return false;
      char esc = input_[position_++];
      switch (esc) {
        case '"': output.push_back('"'); break;
        case '\\': output.push_back('\\'); break;
        case '/': output.push_back('/'); break;
        case 'b': output.push_back('\b'); break;
        case 'f': output.push_back('\f'); break;
        case 'n': output.push_back('\n'); break;
        case 'r': output.push_back('\r'); break;
        case 't': output.push_back('\t'); break;
        case 'u': {
          auto read_unit = [&]() -> uint32_t {
            uint32_t unit = 0;
            for (int i = 0; i < 4; ++i) {
              if (position_ == input_.size()) return UINT32_MAX;
              char h = input_[position_++];
              unit <<= 4;
              if (h >= '0' && h <= '9') unit |= h - '0';
              else if (h >= 'a' && h <= 'f') unit |= h - 'a' + 10;
              else if (h >= 'A' && h <= 'F') unit |= h - 'A' + 10;
              else return UINT32_MAX;
            }
            return unit;
          };
          uint32_t cp = read_unit();
          if (cp == UINT32_MAX) return false;
          if (cp >= 0xd800 && cp <= 0xdbff) {
            if (position_ + 2 > input_.size() || input_[position_] != '\\' ||
                input_[position_ + 1] != 'u') return false;
            position_ += 2;
            const uint32_t low = read_unit();
            if (low < 0xdc00 || low > 0xdfff) return false;
            cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
          } else if (cp >= 0xdc00 && cp <= 0xdfff) return false;
          if (cp <= 0x7f) output.push_back(static_cast<char>(cp));
          else if (cp <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            output.push_back(static_cast<char>(0x80 | (cp & 63)));
          } else if (cp <= 0xffff) {
            output.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            output.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
            output.push_back(static_cast<char>(0x80 | (cp & 63)));
          } else {
            output.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            output.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63)));
            output.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
            output.push_back(static_cast<char>(0x80 | (cp & 63)));
          }
          break;
        }
        default: return false;
      }
    }
    return false;
  }
  bool parse_value(Json& output, unsigned depth) {
    if (depth > 32) return false;
    whitespace();
    if (position_ >= input_.size()) return false;
    char c = input_[position_];
    if (c == '"') {
      std::string s;
      if (!parse_string(s)) return false;
      output.value = std::move(s);
      return true;
    }
    if (c == '{') {
      ++position_;
      Json::Object object;
      whitespace();
      if (consume('}')) { output.value = std::move(object); return true; }
      for (;;) {
        std::string key;
        if (!parse_string(key) || !consume(':')) return false;
        Json child;
        if (!parse_value(child, depth + 1) ||
            !object.emplace(std::move(key), std::move(child)).second)
          return false;
        if (consume('}')) break;
        if (!consume(',')) return false;
      }
      output.value = std::move(object);
      return true;
    }
    if (c == '[') {
      ++position_;
      Json::Array array;
      whitespace();
      if (consume(']')) { output.value = std::move(array); return true; }
      for (;;) {
        Json child;
        if (!parse_value(child, depth + 1)) return false;
        array.push_back(std::move(child));
        if (consume(']')) break;
        if (!consume(',')) return false;
      }
      output.value = std::move(array);
      return true;
    }
    if (input_.substr(position_, 4) == "true") {
      position_ += 4; output.value = true; return true;
    }
    if (input_.substr(position_, 5) == "false") {
      position_ += 5; output.value = false; return true;
    }
    if (input_.substr(position_, 4) == "null") {
      position_ += 4; output.value = nullptr; return true;
    }
    size_t start = position_;
    if (input_[position_] == '-') ++position_;
    if (position_ >= input_.size() || !std::isdigit(
            static_cast<unsigned char>(input_[position_]))) return false;
    if (input_[position_] == '0') ++position_;
    else while (position_ < input_.size() &&
                std::isdigit(static_cast<unsigned char>(input_[position_])))
      ++position_;
    if (position_ < input_.size() &&
        (input_[position_] == '.' || input_[position_] == 'e' ||
         input_[position_] == 'E')) return false;
    output.value = std::string(input_.substr(start, position_ - start));
    output.number = true;
    return true;
  }
  std::string_view input_;
  size_t position_ = 0;
};

void canonical_string(std::string_view text, std::string& out) {
  static constexpr char hex[] = "0123456789abcdef";
  auto emit_unit = [&](uint16_t unit) {
    out += "\\u";
    out.push_back(hex[(unit >> 12) & 15]);
    out.push_back(hex[(unit >> 8) & 15]);
    out.push_back(hex[(unit >> 4) & 15]);
    out.push_back(hex[unit & 15]);
  };
  out.push_back('"');
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
    else if (c == '\b') out += "\\b";
    else if (c == '\f') out += "\\f";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (c < 0x20) {
      out += "\\u00"; out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]);
    } else if (c < 0x80) out.push_back(c);
    else {
      uint32_t cp = 0;
      size_t width = 0;
      if ((c & 0xe0) == 0xc0) { cp = c & 31; width = 2; }
      else if ((c & 0xf0) == 0xe0) { cp = c & 15; width = 3; }
      else if ((c & 0xf8) == 0xf0) { cp = c & 7; width = 4; }
      else throw std::runtime_error("invalid UTF-8 in trace request");
      if (i + width > text.size())
        throw std::runtime_error("truncated UTF-8 in trace request");
      for (size_t j = 1; j < width; ++j) {
        const unsigned char next = static_cast<unsigned char>(text[i + j]);
        if ((next & 0xc0) != 0x80)
          throw std::runtime_error("invalid UTF-8 in trace request");
        cp = (cp << 6) | (next & 63);
      }
      i += width;
      if (cp <= 0xffff) emit_unit(static_cast<uint16_t>(cp));
      else {
        cp -= 0x10000;
        emit_unit(static_cast<uint16_t>(0xd800 | (cp >> 10)));
        emit_unit(static_cast<uint16_t>(0xdc00 | (cp & 0x3ff)));
      }
      continue;
    }
    ++i;
  }
  out.push_back('"');
}

void canonical(const Json& json, std::string& out) {
  if (std::holds_alternative<std::nullptr_t>(json.value)) out += "null";
  else if (const auto* boolean = std::get_if<bool>(&json.value))
    out += *boolean ? "true" : "false";
  else if (const auto* text = std::get_if<std::string>(&json.value)) {
    if (json.number) out += *text;
    else canonical_string(*text, out);
  } else if (const auto* array = std::get_if<Json::Array>(&json.value)) {
    out.push_back('[');
    bool first = true;
    for (const Json& item : *array) {
      if (!first) out.push_back(',');
      first = false;
      canonical(item, out);
    }
    out.push_back(']');
  } else {
    const auto& object = std::get<Json::Object>(json.value);
    out.push_back('{');
    bool first = true;
    for (const auto& [key, item] : object) {
      if (!first) out.push_back(',');
      first = false;
      canonical_string(key, out);
      out.push_back(':');
      canonical(item, out);
    }
    out.push_back('}');
  }
}

const Json* field(const Json::Object& object, const char* name) {
  const auto it = object.find(name);
  return it == object.end() ? nullptr : &it->second;
}

bool string_field(const Json::Object& object, const char* name,
                  std::string& out) {
  const Json* item = field(object, name);
  const auto* text = item ? std::get_if<std::string>(&item->value) : nullptr;
  if (!text || item->number) return false;
  out = *text;
  return true;
}

bool uint_field(const Json::Object& object, const char* name, uint32_t maximum,
                uint32_t& out) {
  const Json* item = field(object, name);
  const auto* text = item ? std::get_if<std::string>(&item->value) : nullptr;
  if (!text || !item->number || text->empty() || text->front() == '-') return false;
  uint64_t value = 0;
  for (char c : *text) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    value = value * 10 + static_cast<uint32_t>(c - '0');
    if (value > maximum) return false;
  }
  out = static_cast<uint32_t>(value);
  return true;
}

bool string_array(const Json::Object& object, const char* name,
                  std::vector<std::string>& out) {
  const Json* item = field(object, name);
  const auto* array = item ? std::get_if<Json::Array>(&item->value) : nullptr;
  if (!array) return false;
  for (const Json& element : *array) {
    const auto* text = std::get_if<std::string>(&element.value);
    if (!text || element.number ||
        std::find(out.begin(), out.end(), *text) != out.end())
      return false;
    out.push_back(*text);
  }
  return true;
}

std::string sha256(std::string_view text) {
  unsigned char bytes[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(text.data(), static_cast<CC_LONG>(text.size()), bytes);
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(CC_SHA256_DIGEST_LENGTH * 2);
  for (unsigned char byte : bytes) {
    out.push_back(hex[byte >> 4]);
    out.push_back(hex[byte & 15]);
  }
  return out;
}

bool bool_field(const Json::Object& object, const char* name, bool& result) {
  const Json* item = field(object, name);
  const auto* value = item ? std::get_if<bool>(&item->value) : nullptr;
  if (!value) return false;
  result = *value;
  return true;
}

bool absolute_path(const std::string& path) {
  return !path.empty() && std::filesystem::path(path).is_absolute() &&
         std::filesystem::path(path).lexically_normal().is_absolute();
}
}  // namespace

bool parse_request(const std::string& request_path, ParsedConfig& output,
                   std::string& error) noexcept {
  try {
    std::ifstream input(request_path, std::ios::binary);
    if (!input) { error = "request cannot be opened"; return false; }
    std::string bytes(std::istreambuf_iterator<char>(input), {});
    if (bytes.empty() || bytes.size() > 1024 * 1024) {
      error = "request size outside 1..1048576 bytes";
      return false;
    }
    Json root;
    Parser parser(bytes);
    if (!parser.parse(root)) {
      error = "malformed JSON, duplicate key, or unsupported number";
      return false;
    }
    auto* object = std::get_if<Json::Object>(&root.value);
    if (!object) { error = "request root must be an object"; return false; }
    std::string schema, declared_digest;
    if (!string_field(*object, "schema", schema) ||
        schema != "dxmt9.trace.request.v1" ||
        !string_field(*object, "config_sha256", declared_digest) ||
        declared_digest.size() != 64) {
      error = "unsupported schema or invalid config_sha256";
      return false;
    }
    Json digest_input = root;
    std::get<Json::Object>(digest_input.value).erase("config_sha256");
    std::string canonical_bytes;
    canonical(digest_input, canonical_bytes);
    if (sha256(canonical_bytes) != declared_digest) {
      error = "config_sha256 does not match canonical request";
      return false;
    }
    output.config_sha256 = declared_digest;
    if (!string_field(*object, "run_id", output.run_id) ||
        !string_field(*object, "attempt_id", output.attempt_id) ||
        !string_field(*object, "profile_id", output.profile_id) ||
        !string_field(*object, "question", output.question) ||
        output.run_id.empty() || output.attempt_id.empty() ||
        output.profile_id.empty() || output.question.empty()) {
      error = "run, attempt, profile and question must be nonempty strings";
      return false;
    }
    if (!string_array(*object, "categories", output.categories) ||
        !string_array(*object, "required_categories",
                      output.required_categories) ||
        !string_array(*object, "optional_collectors",
                      output.optional_collectors)) {
      error = "category and optional collector fields must be unique string arrays";
      return false;
    }
    const Json* window_value = field(*object, "window");
    const Json* limits_value = field(*object, "limits");
    const Json* output_value = field(*object, "output");
    const auto* window = window_value
        ? std::get_if<Json::Object>(&window_value->value) : nullptr;
    const auto* limits = limits_value
        ? std::get_if<Json::Object>(&limits_value->value) : nullptr;
    const auto* paths = output_value
        ? std::get_if<Json::Object>(&output_value->value) : nullptr;
    std::string mode;
    if (!window || !limits || !paths ||
        !string_field(*window, "mode", mode) || mode != "duration" ||
        !uint_field(*window, "delay_ms", 1'800'000, output.delay_ms) ||
        !uint_field(*window, "duration_ms", 1'800'000, output.duration_ms) ||
        !uint_field(*limits, "max_events", 1'000'000, output.max_events) ||
        !uint_field(*limits, "max_bytes", 128 * 1024 * 1024, output.max_bytes) ||
        !uint_field(*limits, "finalize_timeout_ms", 30'000,
                    output.finalize_timeout_ms) ||
        !string_field(*paths, "pftrace", output.pftrace_path) ||
        !string_field(*paths, "receipt", output.receipt_path)) {
      error = "window, limits or output fields are missing or invalid";
      return false;
    }
    std::vector<std::string> required_collectors;
    if (!string_array(*object, "required_collectors", required_collectors) ||
        std::find(required_collectors.begin(), required_collectors.end(),
                  "engine-perfetto") == required_collectors.end()) {
      error = "engine-perfetto must be a required collector";
      return false;
    }
    if (!output.duration_ms || !output.max_events || !output.max_bytes ||
        !output.finalize_timeout_ms ||
        output.duration_ms + output.delay_ms > 1'800'000 ||
        output.max_events > output.max_bytes / 64) {
      error = "window and resource limits exceed supported bounds";
      return false;
    }
    if (const Json* policy_value = field(*object, "qualification_policy")) {
      const auto* policy = std::get_if<Json::Object>(&policy_value->value);
      if (!policy || !uint_field(*policy, "max_clock_error_ns", 1'000'000'000,
                                 output.max_clock_error_ns)) {
        error = "qualification_policy.max_clock_error_ns is invalid";
        return false;
      }
    }
    if (!absolute_path(output.pftrace_path) ||
        !absolute_path(output.receipt_path)) {
      error = "output paths must be absolute";
      return false;
    }
    auto normalize = [](const std::string& path) {
      return std::filesystem::path(path).lexically_normal().string();
    };
    output.pftrace_path = normalize(output.pftrace_path);
    output.receipt_path = normalize(output.receipt_path);
    output.raw_path = output.pftrace_path + ".raw.jsonl";
    if (output.pftrace_path == output.receipt_path ||
        output.raw_path == output.receipt_path ||
        output.pftrace_path == request_path ||
        output.receipt_path == request_path) {
      error = "request, receipt, raw trace and pftrace paths collide";
      return false;
    }
    if (std::filesystem::exists(output.pftrace_path) ||
        std::filesystem::exists(output.receipt_path) ||
        std::filesystem::exists(output.raw_path)) {
      error = "trace output already exists";
      return false;
    }
    const Json* gpu_value = field(*object, "gpu_timing");
    const auto* gpu = gpu_value ? std::get_if<Json::Object>(&gpu_value->value) : nullptr;
    const bool gpu_selected =
        std::find(output.categories.begin(), output.categories.end(), "gpu") !=
        output.categories.end();
    output.gpu_requested = gpu_selected;
    if (gpu_selected) {
      if (!gpu || !string_field(*gpu, "level", output.gpu_level) ||
          (output.gpu_level != "command-buffer" &&
           output.gpu_level != "encoder-stage") ||
          !uint_field(*gpu, "capacity", 256, output.gpu_capacity) ||
          !output.gpu_capacity ||
          !bool_field(*gpu, "required", output.gpu_required) ||
          !bool_field(*gpu, "calibration", output.gpu_calibration)) {
        error = "gpu timing configuration is missing or invalid";
        return false;
      }
    }
    return true;
  } catch (...) {
    error = "trace request parser failed";
    return false;
  }
}
}  // namespace dxmt9::tracing
