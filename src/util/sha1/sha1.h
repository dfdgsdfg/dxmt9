#pragma once

#include <stddef.h>
#include <stdint.h>

#define SHA1_BLOCK_LENGTH 64
#define SHA1_DIGEST_LENGTH 20
#define SHA1_DIGEST_STRING_LENGTH (SHA1_DIGEST_LENGTH * 2 + 1)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SHA1_CTX {
  uint32_t state[5];
  uint64_t count;
  uint8_t buffer[SHA1_BLOCK_LENGTH];
} SHA1_CTX;

void SHA1Init(SHA1_CTX* context);
void SHA1Pad(SHA1_CTX* context);
void SHA1Transform(uint32_t state[5], const uint8_t* buffer);
void SHA1Update(SHA1_CTX* context, const uint8_t* data, size_t len);
void SHA1Final(uint8_t digest[SHA1_DIGEST_LENGTH], SHA1_CTX* context);

#ifdef __cplusplus
}
#endif
