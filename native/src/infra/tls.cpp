#include "infra/tls.hpp"

#include <mutex>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#include <openssl/x509.h>
#endif

namespace di::infra {

namespace ssl = boost::asio::ssl;

boost::asio::ssl::context& tls_client_context() {
    static ssl::context ctx(ssl::context::tls_client);
    static std::once_flag once;
    std::call_once(once, [] {
        ctx.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::no_sslv3 |
                        ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);
        ctx.set_verify_mode(ssl::verify_peer);
        boost::system::error_code ec;
        ctx.set_default_verify_paths(ec);
#ifdef _WIN32
        if (HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT")) {
            X509_STORE* x509_store = SSL_CTX_get_cert_store(ctx.native_handle());
            PCCERT_CONTEXT cert = nullptr;
            while ((cert = CertEnumCertificatesInStore(store, cert)) != nullptr) {
                const unsigned char* data = cert->pbCertEncoded;
                X509* x509 = d2i_X509(nullptr, &data, static_cast<long>(cert->cbCertEncoded));
                if (x509) {
                    X509_STORE_add_cert(x509_store, x509);
                    X509_free(x509);
                }
            }
            CertCloseStore(store, 0);
        }
#endif
    });
    return ctx;
}

} // namespace di::infra
