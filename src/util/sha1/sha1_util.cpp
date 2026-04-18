#include "sha1_util.hpp"

namespace dxmt9::util {

std::string Sha1Digest::string() const {
  static const char kNibbles[] = {'0', '1', '2', '3', '4', '5', '6', '7',
                                  '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

  std::string result;
  result.resize(2 * sizeof(data));

  for (uint32_t i = 0; i < sizeof(data); i++) {
    result[2 * i + 0] = kNibbles[(data[i] >> 4) & 0xF];
    result[2 * i + 1] = kNibbles[(data[i] >> 0) & 0xF];
  }

  return result;
}

Sha1HashState::Sha1HashState() {
  SHA1Init(&ctx_);
}

Sha1HashState& Sha1HashState::update(const void* data, size_t size) {
  SHA1Update(&ctx_, reinterpret_cast<const uint8_t*>(data), size);
  return *this;
}

Sha1Digest Sha1HashState::final() {
  Sha1Digest digest{};
  SHA1Final(digest.data, &ctx_);
  return digest;
}

}  // namespace dxmt9::util
