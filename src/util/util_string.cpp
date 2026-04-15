#include "util_string.hpp"

namespace dxmt9::util::str {

std::string toLowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string trim(std::string_view value) {
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return std::string(value.substr(begin, end - begin));
}

}  // namespace dxmt9::util::str
