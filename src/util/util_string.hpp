#pragma once

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace dxmt9::util::str {

std::string toLowerAscii(std::string value);
std::string trim(std::string_view value);

inline void strlcpy(char* dst, const char* src, size_t count) {
  if (count > 0) {
    std::strncpy(dst, src, count - 1);
    dst[count - 1] = '\0';
  }
}

inline std::vector<std::string_view> split(std::string_view string, std::string_view delims = " ") {
  std::vector<std::string_view> tokens;
  for (size_t start = 0; start < string.size();) {
    const auto end = string.find_first_of(delims, start);
    if (start != end) {
      tokens.emplace_back(string.substr(start, end - start));
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return tokens;
}

inline void format1(std::stringstream&) {}

template <typename T, typename... Tx>
void format1(std::stringstream& stream, const T& arg, const Tx&... args) {
  stream << arg;
  format1(stream, args...);
}

template <typename... Args>
std::string format(const Args&... args) {
  std::stringstream stream;
  format1(stream, args...);
  return stream.str();
}

}  // namespace dxmt9::util::str
