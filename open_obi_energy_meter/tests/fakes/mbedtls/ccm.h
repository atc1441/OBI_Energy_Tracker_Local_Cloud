#pragma once
#include <openssl/evp.h>
#include <string.h>
#include <stdint.h>

#define MBEDTLS_CIPHER_ID_AES 1
struct mbedtls_ccm_context { uint8_t key[16]; };
inline void mbedtls_ccm_init(mbedtls_ccm_context *) {}
inline void mbedtls_ccm_free(mbedtls_ccm_context *) {}
inline int mbedtls_ccm_setkey(mbedtls_ccm_context *ctx, int type, const uint8_t *key, unsigned bits) {
  if (type != MBEDTLS_CIPHER_ID_AES || bits != 128) return -1;
  memcpy(ctx->key, key, 16); return 0;
}
inline int mbedtls_ccm_encrypt_and_tag(mbedtls_ccm_context *ctx, size_t size,
    const uint8_t *nonce, size_t nonceSize, const uint8_t *, size_t aadSize,
    const uint8_t *plain, uint8_t *cipher, uint8_t *tag, size_t tagSize) {
  if (nonceSize != 13 || tagSize != 4 || aadSize != 0) return -1;
  EVP_CIPHER_CTX *evp = EVP_CIPHER_CTX_new();
  if (!evp) return -1;
  int n = 0;
  bool ok = EVP_EncryptInit_ex(evp, EVP_aes_128_ccm(), nullptr, nullptr, nullptr) == 1 &&
      EVP_CIPHER_CTX_ctrl(evp, EVP_CTRL_CCM_SET_IVLEN, nonceSize, nullptr) == 1 &&
      EVP_CIPHER_CTX_ctrl(evp, EVP_CTRL_CCM_SET_TAG, tagSize, nullptr) == 1 &&
      EVP_EncryptInit_ex(evp, nullptr, nullptr, ctx->key, nonce) == 1 &&
      EVP_EncryptUpdate(evp, nullptr, &n, nullptr, size) == 1 &&
      EVP_EncryptUpdate(evp, cipher, &n, plain, size) == 1 &&
      EVP_EncryptFinal_ex(evp, cipher + n, &n) == 1 &&
      EVP_CIPHER_CTX_ctrl(evp, EVP_CTRL_CCM_GET_TAG, tagSize, tag) == 1;
  EVP_CIPHER_CTX_free(evp);
  return ok ? 0 : -1;
}
