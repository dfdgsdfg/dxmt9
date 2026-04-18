#pragma once

#include "./sha1.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>

namespace dxmt9::util {

struct Sha1Digest {
  uint8_t data[20];

  bool operator==(const Sha1Digest& other) const {
    return !std::memcmp(data, other.data, sizeof(data));
  }

  bool operator!=(const Sha1Digest& other) const {
    return !this->operator==(other);
  }

  std::string string() const;
};

class Sha1HashState {
 public:
  Sha1HashState();

  Sha1HashState& update(const void* data, size_t size);

  template <typename T>
  Sha1HashState& update(const T& data) {
    return update(&data, sizeof(T));
  }

  Sha1Digest final();

  static Sha1Digest compute(const void* data, size_t size) {
    Sha1HashState hash;
    hash.update(data, size);
    return hash.final();
  }

 private:
  SHA1_CTX ctx_;
};

}  // namespace dxmt9::util

namespace std {

template <>
struct hash<dxmt9::util::Sha1Digest> {
  size_t operator()(const dxmt9::util::Sha1Digest& value) const noexcept {
    return std::hash<std::string_view>{}(std::string_view{
        reinterpret_cast<const char*>(value.data), sizeof(value.data)});
  }
};

template <>
struct equal_to<dxmt9::util::Sha1Digest> {
  bool operator()(const dxmt9::util::Sha1Digest& left,
                  const dxmt9::util::Sha1Digest& right) const {
    return left == right;
  }
};

}  // namespace std
