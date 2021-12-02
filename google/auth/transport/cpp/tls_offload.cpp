#include "tls_offload.h"

#include <openssl/crypto.h>
#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/engine.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <string.h>
 
#include <memory>
#include <iostream>
#include <stdlib.h>

#ifdef _WIN32
#include <Python.h>
#endif

namespace {

template <typename T, typename Ret, Ret (*Deleter)(T *)>
struct OpenSSLDeleter {
  void operator()(T *t) const { Deleter(t); }
};
struct OpenSSLFreeDeleter {
  void operator()(unsigned char *buf) const { OPENSSL_free(buf); }
};
template <typename T, void (*Deleter)(T *)>
using OwnedOpenSSLPtr = std::unique_ptr<T, OpenSSLDeleter<T, void, Deleter>>;
template <typename T, int (*Deleter)(T *)>
using OwnedOpenSSLPtrIntRet =
    std::unique_ptr<T, OpenSSLDeleter<T, int, Deleter>>;
using OwnedBIO = OwnedOpenSSLPtrIntRet<BIO, BIO_free>;
using OwnedENGINE = OwnedOpenSSLPtrIntRet<ENGINE, ENGINE_free>;
using OwnedEVP_MD_CTX = OwnedOpenSSLPtr<EVP_MD_CTX, EVP_MD_CTX_free>;
using OwnedEVP_PKEY = OwnedOpenSSLPtr<EVP_PKEY, EVP_PKEY_free>;
using OwnedEVP_PKEY_METHOD =
    OwnedOpenSSLPtr<EVP_PKEY_METHOD, EVP_PKEY_meth_free>;
using OwnedSSL_CTX = OwnedOpenSSLPtr<SSL_CTX, SSL_CTX_free>;
using OwnedSSL = OwnedOpenSSLPtr<SSL, SSL_free>;
using OwnedX509_PUBKEY = OwnedOpenSSLPtr<X509_PUBKEY, X509_PUBKEY_free>;
using OwnedX509 = OwnedOpenSSLPtr<X509, X509_free>;
using OwnedOpenSSLBuffer = std::unique_ptr<uint8_t, OpenSSLFreeDeleter>;

static bool EnableLogging = false;
static int rsa_ex_index = -1, ec_ex_index = -1;
static EVP_PKEY_METHOD *custom_rsa_pkey_method, *custom_ec_pkey_method;
static ENGINE *custom_engine = nullptr;

static int EngineGetMethods(ENGINE *e, EVP_PKEY_METHOD **out_method, const int **out_nids, int nid) {
  if (!out_method) {
    static const int kNIDs[] = {EVP_PKEY_EC, EVP_PKEY_RSA};
    *out_nids = kNIDs;
    return sizeof(kNIDs) / sizeof(kNIDs[0]);
  }
 
  switch (nid) {
    case EVP_PKEY_EC:
      *out_method = custom_ec_pkey_method;
      return 1;
    case EVP_PKEY_RSA:
      *out_method = custom_rsa_pkey_method;
      return 1;
  }
  return 0;
}

void LogInfo(const std::string& message) {
  if (EnableLogging) {
    std::cout << "tls_offload.cpp: " << message << "...." << std::endl;
  }
}

bool InitExData() {
  rsa_ex_index = RSA_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
  ec_ex_index = EC_KEY_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
  if (rsa_ex_index < 0 || ec_ex_index < 0) {
    fprintf(stderr, "Error allocating ex data.\n");
    return false;
  }
  return true;
}
 
bool SetCustomKey(EVP_PKEY *pkey, CustomKey *key) {
  if (EVP_PKEY_id(pkey) == EVP_PKEY_RSA) {
    LogInfo("setting RSA custom key");
    RSA *rsa = EVP_PKEY_get0_RSA(pkey);
    return rsa && RSA_set_ex_data(rsa, rsa_ex_index, key);
  }
  if (EVP_PKEY_id(pkey) == EVP_PKEY_EC) {
    LogInfo("setting EC custom key");
    EC_KEY *ec_key = EVP_PKEY_get0_EC_KEY(pkey);
    return ec_key && EC_KEY_set_ex_data(ec_key, ec_ex_index, key);
  }
  return false;
}

CustomKey *GetCustomKey(EVP_PKEY *pkey) {
  if (EVP_PKEY_id(pkey) == EVP_PKEY_RSA) {
    const RSA *rsa = EVP_PKEY_get0_RSA(pkey);
    return rsa ? static_cast<CustomKey*>(RSA_get_ex_data(rsa, rsa_ex_index)) : nullptr;
  }
  if (EVP_PKEY_id(pkey) == EVP_PKEY_EC) {
    const EC_KEY *ec_key = EVP_PKEY_get0_EC_KEY(pkey);
    return ec_key ? static_cast<CustomKey*>(EC_KEY_get_ex_data(ec_key, ec_ex_index)) : nullptr;
  }
  return nullptr;
}

int CustomDigestSign(EVP_MD_CTX *ctx, unsigned char *sig, size_t *sig_len,
                     const unsigned char *tbs, size_t tbs_len) {
  LogInfo("calling CustomDigestSign");
  EVP_PKEY_CTX *pctx = EVP_MD_CTX_pkey_ctx(ctx);
  EVP_PKEY *pkey = EVP_PKEY_CTX_get0_pkey(pctx);
  if (!pkey) {
    fprintf(stderr, "Could not get EVP_PKEY.\n");
    return 0;
  }
  CustomKey *key = GetCustomKey(EVP_PKEY_CTX_get0_pkey(EVP_MD_CTX_pkey_ctx(ctx)));
  if (!key) {
    fprintf(stderr, "Could not get CustomKey from EVP_PKEY.\n");
    return 0;
  }
  if (EnableLogging) {
    std::cout << "tls_offload.cpp: " << "before calling key->Sign, sig len: " << *sig_len << std::endl;
  }
  int res = key->Sign(sig, sig_len, tbs, tbs_len);
  if (EnableLogging) {
    std::cout << "tls_offload.cpp: after calling key->Sign, sig len: " << *sig_len << ", result: " << res << std::endl;
  }
  return res;  
}

OwnedEVP_PKEY_METHOD MakeCustomMethod(int nid) {
  OwnedEVP_PKEY_METHOD method(EVP_PKEY_meth_new(
      nid, EVP_PKEY_FLAG_SIGCTX_CUSTOM | EVP_PKEY_FLAG_AUTOARGLEN));
  if (!method) {
    return nullptr;
  }

  const EVP_PKEY_METHOD *ossl_method = EVP_PKEY_meth_find(nid);
  if (!ossl_method) {
    return nullptr;
  }
  int (*init)(EVP_PKEY_CTX *);
  EVP_PKEY_meth_get_init(ossl_method, &init);
  EVP_PKEY_meth_set_init(method.get(), init);
  void (*cleanup)(EVP_PKEY_CTX *);
  EVP_PKEY_meth_get_cleanup(ossl_method, &cleanup);
  EVP_PKEY_meth_set_cleanup(method.get(), cleanup);
  int (*ctrl)(EVP_PKEY_CTX *, int, int, void *);
  int (*ctrl_str)(EVP_PKEY_CTX *, const char *, const char *);
  EVP_PKEY_meth_get_ctrl(ossl_method, &ctrl, &ctrl_str);
  EVP_PKEY_meth_set_ctrl(method.get(), ctrl, ctrl_str);
 
  EVP_PKEY_meth_set_digestsign(method.get(), CustomDigestSign);
  return method;
}

OwnedEVP_PKEY ConfigureCertAndCustomKey(CustomKey *custom_key, X509 *cert) {
  unsigned char *spki = nullptr;
  int spki_len = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(cert), &spki);
  if (spki_len < 0) {
    return nullptr;
  }
  OwnedOpenSSLBuffer owned_spki(spki);
  
  const unsigned char *ptr = spki;
  OwnedX509_PUBKEY pubkey(d2i_X509_PUBKEY(nullptr, &ptr, spki_len));
  if (!pubkey) {
    return nullptr;
  }
 
  OwnedEVP_PKEY wrapped(X509_PUBKEY_get(pubkey.get()));
  if (!wrapped ||
      !EVP_PKEY_set1_engine(wrapped.get(), custom_engine) ||
      !SetCustomKey(wrapped.get(), custom_key)) {
    return nullptr;
  }
  return wrapped;
}

static bool InitEngine() {
  custom_rsa_pkey_method = MakeCustomMethod(EVP_PKEY_RSA).release();
  custom_ec_pkey_method = MakeCustomMethod(EVP_PKEY_EC).release();
  if (!custom_rsa_pkey_method || !custom_ec_pkey_method) {
    return false;
  }
 
  OwnedENGINE engine(ENGINE_new());
  if (!engine || !ENGINE_set_pkey_meths(engine.get(), EngineGetMethods)) {
    return false;
  }
  custom_engine = engine.release();
  return true;
}

static bool ServeTLS(CustomKey *custom_key, const char *cert, SSL_CTX *ctx) {
  LogInfo("calling ServeTLS");

  LogInfo("create x509 using the provided cert");
  OwnedBIO bio(BIO_new_mem_buf(cert, strlen(cert)));
  if (!bio) {
    LogInfo("failed to read cert into bio");
    return false;
  }
  OwnedX509 x509 = OwnedX509(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));

  LogInfo("create custom key");
  OwnedEVP_PKEY wrapped_key = ConfigureCertAndCustomKey(custom_key, x509.get());
  if (!wrapped_key) {
    LogInfo("failed to create custom key");
    return false;
  }
  if (!SSL_CTX_use_PrivateKey(ctx, wrapped_key.get())) {
    LogInfo("SSL_CTX_use_PrivateKey failed");
    return false;
  }
  if (!SSL_CTX_use_certificate(ctx, x509.get())) {
    LogInfo("SSL_CTX_use_certificate failed");
    return false;
  }
  if (!SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION)) {
    LogInfo("SSL_CTX_set_min_proto_version failed");
    return false;
  }
  return true;
}

}  // namespace

// Add `extern "C"` to avoid name mangling. 
extern "C"
#ifdef _WIN32
__declspec(dllexport)
#endif
int OffloadSigning(CustomKey *custom_key, const char *cert, SSL_CTX *ctx) {
  char * val = getenv("GOOGLE_AUTH_TLS_OFFLOAD_LOGGING");
  EnableLogging = (val == nullptr)? false : true;
  LogInfo("entering offload function");
  if (!custom_engine) {
    LogInfo("initializing ex data and custom engine");
    if (!InitExData() || !InitEngine()) {
      ERR_print_errors_fp(stderr);
      return 0;
    }
  }
  if (!ServeTLS(custom_key, cert, ctx)) {
    ERR_print_errors_fp(stderr);
    return 0;
  }
  LogInfo("offload function is done");
  return 1;
}

extern "C"
#ifdef _WIN32
__declspec(dllexport)
#endif
CustomKey* CreateCustomKey(SignFunc sign_func) {
  // creating custom key
  CustomKey *key = new CustomKey(sign_func);
  if (EnableLogging) LogInfo("In CreateCustomKey\n");
  return key;
}

extern "C"
#ifdef _WIN32
__declspec(dllexport)
#endif
void DestroyCustomKey(CustomKey *key) {
  // deleting custom key
  if (EnableLogging) LogInfo("In DestroyCustomKey\n");
  delete key;
}

#ifdef _WIN32
PyMODINIT_FUNC PyInit_tls_offload_ext(void) {
    Py_Initialize();
    return PyModule_Create(nullptr);
}
#endif