#include "X509Bundle.h"

#include <mbedtls/md.h>              // for mbedtls_md, mbedtls_md_get_size
#include <mbedtls/pk.h>              // for mbedtls_pk_can_do, mbedtls_pk_pa...
#include <mbedtls/private_access.h>  // for MBEDTLS_PRIVATE
#include <mbedtls/ssl.h>             // for mbedtls_ssl_conf_ca_chain, mbedt...
#include <mbedtls/x509.h>            // for mbedtls_x509_buf, MBEDTLS_ERR_X5...
#include <stdlib.h>                  // for free, calloc
#include <string.h>                  // for memcmp, memcpy
#include <stdexcept>                 // for runtime_error

#include "BellLogger.h"  // for AbstractLogger, BELL_LOG

using namespace bell::X509Bundle;

typedef struct crt_bundle_t {
  const uint8_t** crts;
  uint16_t num_certs;
  size_t x509_crt_bundle_len;
} crt_bundle_t;

static std::vector<uint8_t> bundleBytes;

static constexpr auto TAG = "X509Bundle";
static constexpr auto CRT_HEADER_OFFSET = 4;
static constexpr auto BUNDLE_HEADER_OFFSET = 2;

static mbedtls_x509_crt s_dummy_crt;
static bool s_should_verify_certs = false;
static crt_bundle_t s_crt_bundle;

#ifndef MBEDTLS_PRIVATE
#define MBEDTLS_PRIVATE(member) member
#endif

int bell::X509Bundle::crtCheckCertificate(mbedtls_x509_crt* child,
                                          const uint8_t* pub_key_buf,
                                          size_t pub_key_len) {
  int ret = 0;
  mbedtls_x509_crt parent;
  const mbedtls_md_info_t* md_info;
  unsigned char hash[MBEDTLS_MD_MAX_SIZE];

  mbedtls_x509_crt_init(&parent);

  if ((ret = mbedtls_pk_parse_public_key(&parent.pk, pub_key_buf,
                                         pub_key_len)) != 0) {
    BELL_LOG(error, TAG, "PK parse failed with error 0x%04x, key len = %d", ret,
             pub_key_len);
    goto cleanup;
  }

  // Fast check to avoid expensive computations when not necessary
  if (!mbedtls_pk_can_do(&parent.pk, child->MBEDTLS_PRIVATE(sig_pk))) {
    BELL_LOG(error, TAG, "Simple compare failed");
    ret = -1;
    goto cleanup;
  }

  md_info = mbedtls_md_info_from_type(child->MBEDTLS_PRIVATE(sig_md));
  if ((ret = mbedtls_md(md_info, child->tbs.p, child->tbs.len, hash)) != 0) {
    BELL_LOG(error, TAG, "Internal mbedTLS error %X", ret);
    goto cleanup;
  }

  if ((ret = mbedtls_pk_verify_ext(
           child->MBEDTLS_PRIVATE(sig_pk), child->MBEDTLS_PRIVATE(sig_opts),
           &parent.pk, child->MBEDTLS_PRIVATE(sig_md), hash,
           mbedtls_md_get_size(md_info), child->MBEDTLS_PRIVATE(sig).p,
           child->MBEDTLS_PRIVATE(sig).len)) != 0) {

    BELL_LOG(error, TAG, "PK verify failed with error %X", ret);
    goto cleanup;
  }
cleanup:
  mbedtls_x509_crt_free(&parent);

  return ret;
}

/* This callback is called for every certificate in the chain. If the chain
 * is proper each intermediate certificate is validated through its parent
 * in the x509_crt_verify_chain() function. So this callback should
 * only verify the first untrusted link in the chain is signed by the
 * root certificate in the trusted bundle
*/
int bell::X509Bundle::crtVerifyCallback(void* buf, mbedtls_x509_crt* crt,
                                        int depth, uint32_t* flags) {
  mbedtls_x509_crt* child = crt;

  /* It's OK for a trusted cert to have a weak signature hash alg.
       as we already trust this certificate */
  uint32_t flags_filtered = *flags & ~(MBEDTLS_X509_BADCERT_BAD_MD);

  if (flags_filtered != MBEDTLS_X509_BADCERT_NOT_TRUSTED) {
    return 0;
  }

  if (s_crt_bundle.crts == NULL) {
    BELL_LOG(error, TAG, "No certificates in bundle");
    return MBEDTLS_ERR_X509_FATAL_ERROR;
  }

  BELL_LOG(debug, TAG, "%d certificates in bundle", s_crt_bundle.num_certs);

  size_t name_len = 0;
  const uint8_t* crt_name;

  bool crt_found = false;
  int start = 0;
  int end = s_crt_bundle.num_certs - 1;
  int middle = (end - start) / 2;

  /* Look for the certificate using binary search on subject name */
  while (start <= end) {
    name_len = s_crt_bundle.crts[middle][0] << 8 | s_crt_bundle.crts[middle][1];
    crt_name = s_crt_bundle.crts[middle] + CRT_HEADER_OFFSET;

    int cmp_res = memcmp(child->issuer_raw.p, crt_name, name_len);
    if (cmp_res == 0) {
      crt_found = true;
      break;
    } else if (cmp_res < 0) {
      end = middle - 1;
    } else {
      start = middle + 1;
    }
    middle = (start + end) / 2;
  }

  int ret = MBEDTLS_ERR_X509_FATAL_ERROR;
  if (crt_found) {
    size_t key_len =
        s_crt_bundle.crts[middle][2] << 8 | s_crt_bundle.crts[middle][3];
    ret = crtCheckCertificate(
        child, s_crt_bundle.crts[middle] + CRT_HEADER_OFFSET + name_len,
        key_len);
  } else {
    BELL_LOG(error, TAG, "Certificate not found in bundle");
  }

  if (ret == 0) {
    BELL_LOG(info, TAG, "Certificate validated");
    *flags = 0;
    return 0;
  }

  BELL_LOG(info, TAG, "Failed to verify certificate");
  return MBEDTLS_ERR_X509_FATAL_ERROR;
}

/* Initialize the bundle into an array so we can do binary search for certs,
   the bundle generated by the python utility is already presorted by subject name
 */
void bell::X509Bundle::init(const uint8_t* x509_bundle, size_t bundle_size) {
  if (bundle_size < BUNDLE_HEADER_OFFSET + CRT_HEADER_OFFSET) {
    throw std::runtime_error("Invalid certificate bundle");
  }

  uint16_t num_certs = (x509_bundle[0] << 8) | x509_bundle[1];

  const uint8_t** crts =
      (const uint8_t**)calloc(num_certs, sizeof(x509_bundle));
  if (crts == NULL) {
    throw std::runtime_error("Unable to allocate memory for bundle");
  }

  bundleBytes.resize(bundle_size);
  memcpy(bundleBytes.data(), x509_bundle, bundle_size);

  const uint8_t* cur_crt;
  /* This is the maximum region that is allowed to access */
  const uint8_t* bundle_end = bundleBytes.data() + bundle_size;
  cur_crt = bundleBytes.data() + BUNDLE_HEADER_OFFSET;

  for (int i = 0; i < num_certs; i++) {
    crts[i] = cur_crt;
    if (cur_crt + CRT_HEADER_OFFSET > bundle_end) {
      free(crts);
      throw std::runtime_error("Invalid certificate bundle");
    }
    size_t name_len = cur_crt[0] << 8 | cur_crt[1];
    size_t key_len = cur_crt[2] << 8 | cur_crt[3];
    cur_crt = cur_crt + CRT_HEADER_OFFSET + name_len + key_len;
  }

  if (cur_crt > bundle_end) {
    free(crts);
    throw std::runtime_error("Invalid certificate bundle");
  }

  /* The previous crt bundle is only updated when initialization of the
     * current crt_bundle is successful */
  /* Free previous crt_bundle */
  free(s_crt_bundle.crts);
  s_crt_bundle.num_certs = num_certs;
  s_crt_bundle.crts = crts;

  // Enable certificate verification
  s_should_verify_certs = true;
}

void bell::X509Bundle::attach(mbedtls_ssl_config* conf) {
  /* point to a dummy certificate
   * This is only required so that the
   * cacert_ptr passes non-NULL check during handshake
   */
  mbedtls_ssl_config* ssl_conf = (mbedtls_ssl_config*)conf;
  mbedtls_x509_crt_init(&s_dummy_crt);
  mbedtls_ssl_conf_ca_chain(ssl_conf, &s_dummy_crt, NULL);
  mbedtls_ssl_conf_verify(ssl_conf, crtVerifyCallback, NULL);
}

bool bell::X509Bundle::shouldVerify() {
  return s_should_verify_certs;
}