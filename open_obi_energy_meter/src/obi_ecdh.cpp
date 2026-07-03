#include "obi_ecdh.h"
#include <mbedtls/ecp.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/bignum.h>
#include <esp_system.h>

// hardware RNG (esp_fill_random) — fine for an ephemeral ECDH keypair
static int obi_rng(void *, unsigned char *buf, size_t len) { esp_fill_random(buf, len); return 0; }

static mbedtls_ecp_group s_grp;
static mbedtls_mpi       s_d;     // our private scalar
static mbedtls_ecp_point s_Q;     // our public point
static bool              s_init = false;

static void ensure_init() {
  if (s_init) return;
  mbedtls_ecp_group_init(&s_grp);
  mbedtls_mpi_init(&s_d);
  mbedtls_ecp_point_init(&s_Q);
  mbedtls_ecp_group_load(&s_grp, MBEDTLS_ECP_DP_SECP256R1);
  s_init = true;
}

bool obi_ecdh_generate(uint8_t out_pub64[64]) {
  ensure_init();
  if (mbedtls_ecp_gen_keypair(&s_grp, &s_d, &s_Q, obi_rng, nullptr) != 0) return false;
  uint8_t buf[65]; size_t olen = 0;
  if (mbedtls_ecp_point_write_binary(&s_grp, &s_Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                     &olen, buf, sizeof(buf)) != 0) return false;
  if (olen != 65 || buf[0] != 0x04) return false;     // 0x04 || X(32) || Y(32)
  memcpy(out_pub64, buf + 1, 64);                     // strip the 0x04 prefix -> raw X||Y
  return true;
}

bool obi_ecdh_compute(const uint8_t peer_pub64[64], uint8_t out_secret32[32]) {
  ensure_init();
  mbedtls_ecp_point Qp; mbedtls_ecp_point_init(&Qp);
  mbedtls_mpi z;        mbedtls_mpi_init(&z);
  uint8_t buf[65]; buf[0] = 0x04; memcpy(buf + 1, peer_pub64, 64);
  bool ok = false;
  if (mbedtls_ecp_point_read_binary(&s_grp, &Qp, buf, 65) == 0 &&
      mbedtls_ecdh_compute_shared(&s_grp, &z, &Qp, &s_d, obi_rng, nullptr) == 0 &&
      mbedtls_mpi_write_binary(&z, out_secret32, 32) == 0) {
    ok = true;                                         // out_secret32 = shared X (big-endian)
  }
  mbedtls_ecp_point_free(&Qp);
  mbedtls_mpi_free(&z);
  return ok;
}
