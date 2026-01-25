#include "tls.hpp"
#include "certificate.h"
#include <string.h>
#include <stdbool.h>
#include <memory>
#include <cstdio>

#include <logging/log.hpp>
#include <unique_file_ptr.hpp>
#include <common/heap.h>
#include <common/conserve_cpu.hpp>

#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>

#include <lwip/mem.h>

using http::Error;
using std::unique_ptr;

LOG_COMPONENT_REF(mqtt);

namespace {

class EntropyDeleter {
public:
    void operator()(mbedtls_entropy_context *ctx) {
        if (ctx != nullptr) {
            mbedtls_entropy_free(ctx);
            // Unlike stdlib's free, mem_free seems to get annoyed in logs about NULL pointers.
            mem_free(ctx);
        }
    }
};

struct InitContexts {
    mbedtls_x509_crt x509_certificate;
    unique_ptr<mbedtls_entropy_context, EntropyDeleter> entropy_context;
    mbedtls_ctr_drbg_context drbg_context;
    InitContexts() {
        mbedtls_x509_crt_init(&x509_certificate);
        constexpr size_t entropy_size = sizeof(mbedtls_entropy_context);
        // We want to "hit" the 512B pool, as that one is also used by DHCP and DHCP is "rare", it's likely going to be free.
        static_assert(entropy_size <= 512);
        static_assert(entropy_size >= 128);
        entropy_context.reset(reinterpret_cast<mbedtls_entropy_context *>(mem_malloc(entropy_size)));
        if (entropy_context) {
            mbedtls_entropy_init(entropy_context.get());
        }
        mbedtls_ctr_drbg_init(&drbg_context);
    }
    InitContexts(const InitContexts &others) = delete;
    InitContexts(InitContexts &&others) = delete;
    InitContexts &operator=(const InitContexts &others) = delete;
    InitContexts &operator=(InitContexts &&others) = delete;
    ~InitContexts() {
        mbedtls_ctr_drbg_free(&drbg_context);
        // entropy done by unique_ptr
        mbedtls_x509_crt_free(&x509_certificate);
    }

    bool is_valid() const {
        return !!entropy_context;
    }
};

void bytes_to_hex(const uint8_t *data, size_t size, char *out, size_t out_len) {
    static constexpr char kHex[] = "0123456789abcdef";
    if (out_len == 0) {
        return;
    }
    size_t i = 0;
    for (; i < size && (i * 2 + 1) < out_len; ++i) {
        out[i * 2] = kHex[(data[i] >> 4) & 0x0f];
        out[i * 2 + 1] = kHex[data[i] & 0x0f];
    }
    if (i * 2 < out_len) {
        out[i * 2] = '\0';
    } else {
        out[out_len - 1] = '\0';
    }
}

void log_cert_fingerprint(const char *label, const uint8_t *data, size_t size) {
    uint8_t digest[32] = {};
    if (mbedtls_sha256_ret(data, size, digest, 0) != 0) {
        log_info(mqtt, "%s sha256=<error>", label);
        return;
    }
    char hex[65] = {};
    bytes_to_hex(digest, sizeof(digest), hex, sizeof(hex));
    log_info(mqtt, "%s sha256=%s size=%u", label, hex, static_cast<unsigned>(size));
}

void log_verify_flags(uint32_t flags) {
    if (flags == 0) {
        log_info(mqtt, "tls_verify_flags=0x0");
        return;
    }
    char info[256] = {};
    mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
    log_info(mqtt, "tls_verify_flags=0x%08x (%s)", static_cast<unsigned>(flags), info);
}

} // namespace

namespace buddy::mqtt {

tls::tls(uint8_t timeout_s, bool custom_cert)
    : Connection(timeout_s)
    , net_context(timeout_s)
    , custom_cert(custom_cert) {
    mbedtls_net_init(&net_context);
    mbedtls_ssl_init(&ssl_context);
    mbedtls_ssl_config_init(&ssl_config);
}

tls::~tls() {
    // We currently do _not_ do this on purpose even though it's a good practice, because:
    // * This'll create and try to send another packet over the TCP.
    // * Nevertheless, we do this in case we think the TCP connection is _dead_.
    //
    // So the likely effect is just allocating more memory for the packet
    // that'll retransmit multiple times and sit there for a long time, instead
    // just getting rid of it. Even in the case where we _would_ send it
    // successfully, it doesn't serve any particular purpose for us (we are
    // between full request-response exchanges at this point, so there's
    // nothing to effectively close/confirm).
    // mbedtls_ssl_close_notify(&ssl_context);
    mbedtls_net_free(&net_context);
    mbedtls_ssl_free(&ssl_context);
    mbedtls_ssl_config_free(&ssl_config);
}

std::optional<Error> tls::connection(const char *connection_host, uint16_t connection_port, const char *destination_host, uint16_t destination_port) {

    log_debug(mqtt, "Starting SSL handshake");
    int status;
    InitContexts ctxs;

    // Ask for other subsystems to save CPU if possible until we are done with
    // TLS handshake. That's CPU intensive and there's a risk we won't make it
    // in time for the server not to close the connection.
    //
    // (for example, prevents rolling texts from rolling).
    buddy::ConserveCpu::Guard request_cpu_limiting;

    if (!ctxs.is_valid()) {
        log_error(mqtt, "Not enough mem for SSL");
        return Error::Memory;
    }

    if ((status = mbedtls_ctr_drbg_seed(&ctxs.drbg_context, mbedtls_entropy_func, ctxs.entropy_context.get(), NULL, 0)) != 0) {
        return Error::InternalError;
    }

    mbedtls_ssl_conf_rng(&ssl_config, mbedtls_ctr_drbg_random, &ctxs.drbg_context);
    class FreeDeleter {
    public:
        void operator()(void *p) {
            free(p);
        }
    };
    unique_ptr<void, FreeDeleter> der_buffer;
    if (custom_cert) {
        // Note that mbedtls offers the parse_path / parse_file variants, but
        // these expect a PEM file and we do not want to support PEM too (extra
        // code size).
        //
        // TODO: Unify the path somewhere
        const char *cert_path = "/internal/connect/connect.der";
        log_info(mqtt, "custom_cert=true path=%s", cert_path);
        unique_file_ptr cert(fopen(cert_path, "rb"));
        if (!cert) {
            // Missing cert
            log_info(mqtt, "custom_cert missing");
            return Error::Tls;
        }

        if (fseek(cert.get(), 0, SEEK_END) != 0) {
            return Error::InternalError;
        }

        long fsize = ftell(cert.get());
        if (fsize == -1) {
            return Error::InternalError;
        }

        rewind(cert.get());

        der_buffer.reset(malloc_fallible(fsize));
        if (!der_buffer) {
            return Error::InternalError;
        }

        size_t read = fread(der_buffer.get(), 1, fsize, cert.get());
        if (read != static_cast<size_t>(fsize) || ferror(cert.get())) {
            log_info(mqtt, "custom_cert read failed: read=%u expected=%u", static_cast<unsigned>(read),
                static_cast<unsigned>(fsize));
            return Error::InternalError;
        }

        log_cert_fingerprint("custom_ca", static_cast<const uint8_t *>(der_buffer.get()), fsize);
        status = mbedtls_x509_crt_parse_der_nocopy(
            &ctxs.x509_certificate,
            static_cast<const uint8_t *>(der_buffer.get()),
            fsize);
        if (status != 0) {
            // Wrong file content
            log_info(mqtt, "custom_cert parse failed: %d (0x%x)", status, static_cast<unsigned>(-status));
            return Error::Tls;
        }
    } else {
        size_t idx = 0;
        for (const auto &cert : certificates) {
            char label[24] = {};
            snprintf(label, sizeof(label), "builtin_ca[%u]", static_cast<unsigned>(idx));
            log_cert_fingerprint(label, cert.data(), cert.size());
            if ((status = mbedtls_x509_crt_parse_der_nocopy(&ctxs.x509_certificate, cert.data(), cert.size())) != 0) {
                return Error::InternalError;
            }
            ++idx;
        }
    }
    log_debug(mqtt, "Loaded certs");

    mbedtls_ssl_conf_ca_chain(&ssl_config, &ctxs.x509_certificate, NULL);

    if ((status = mbedtls_ssl_config_defaults(&ssl_config,
             MBEDTLS_SSL_IS_CLIENT,
             MBEDTLS_SSL_TRANSPORT_STREAM,
             MBEDTLS_SSL_PRESET_DEFAULT))
        != 0) {
        return Error::InternalError;
    }

    // Only use TLS 1.2
    mbedtls_ssl_conf_max_version(&ssl_config, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_min_version(&ssl_config, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    // Strictly ensure that certificates are signed by the CA
    mbedtls_ssl_conf_authmode(&ssl_config, MBEDTLS_SSL_VERIFY_REQUIRED);

    // set cipher suite to use
    static const int tls_cipher_suites[2] = { MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, 0 };
    mbedtls_ssl_conf_ciphersuites(&ssl_config, tls_cipher_suites);

    mbedtls_ssl_set_hostname(&ssl_context, destination_host);

    if ((status = mbedtls_ssl_setup(&ssl_context, &ssl_config)) != 0) {
        return Error::InternalError;
    }

    mbedtls_ssl_set_bio(&ssl_context, &net_context, mbedtls_net_send, mbedtls_net_recv, NULL);

    if ((status = mbedtls_plain_connect(&net_context, connection_host, connection_port)) != 0) {
        log_info(mqtt, "ssl handshake failed with: %d", status);
        return Error::Connect;
    }

    // Really a pointer compare, not strcmp.
    if (destination_host != connection_host || destination_port != connection_port) {
        log_info(mqtt, "proxy connect not supported");
        return Error::Proxy;
    }

    while ((status = mbedtls_ssl_handshake(&ssl_context)) != 0) {
        if (status != MBEDTLS_ERR_SSL_WANT_READ && status != MBEDTLS_ERR_SSL_WANT_WRITE) {
            log_info(mqtt, "ssl handshake failed with: %d", status);
            return Error::Tls;
        }

        if (net_context.timeout_happened) {
            log_info(mqtt, "SSL timeout");
            // Timeouts are mapped to ERR_SSL_WANT_(READ|WRITE). But possibly
            // there are other things that are mapped to that too? Not sure.
            // Therefore, we smuggle the timeouts in this side channel.
            //
            // This is set in mbedtls_net_recv/mbedtls_net_send in
            // net_sockets.cpp
            return Error::Timeout;
        }
    }

    if (const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&ssl_context); peer != nullptr) {
        log_cert_fingerprint("server_cert", peer->raw.p, peer->raw.len);
    } else {
        log_info(mqtt, "server_cert missing");
    }

    const uint32_t verify_flags = mbedtls_ssl_get_verify_result(&ssl_context);
    log_verify_flags(verify_flags);
    if (verify_flags != 0) {
        log_info(mqtt, "SSL error");
        return Error::Tls;
    }

    log_debug(mqtt, "SSL done");

    return std::nullopt;
}

void tls::set_io_timeout_s(uint8_t timeout_s) {
    net_context.plain_conn.set_timeout_s(timeout_s);
}

std::variant<size_t, Error> tls::tx(const uint8_t *send_buffer, size_t data_len) {
    size_t bytes_sent = 0;

    int status = mbedtls_ssl_write(&ssl_context, (const unsigned char *)send_buffer, data_len);

    if (status <= 0) {
        log_info(mqtt, "ssl write failed with: %d", status);
        if (net_context.timeout_happened) {
            return Error::Timeout;
        } else {
            return Error::Network;
        }
    }

    bytes_sent = (size_t)status;
    return bytes_sent;
}

std::variant<size_t, Error> tls::rx(uint8_t *read_buffer, size_t buffer_len, [[maybe_unused]] bool nonblock) {
    // Non-blocking reading is not supported on TLS sockets right now
    // (it probably _can_ be done, we just didn't need it).
    assert(!nonblock);
    size_t bytes_received = 0;

    int status = mbedtls_ssl_read(&ssl_context, (unsigned char *)read_buffer, buffer_len);

    if (status <= 0) {
        if (net_context.timeout_happened) {
            return Error::Timeout;
        } else {
            return Error::Network;
        }
    }

    bytes_received = (size_t)status;

    return bytes_received;
}

bool tls::poll_readable(uint32_t timeout) {
    return mbedtls_ssl_check_pending(&ssl_context) || net_context.plain_conn.poll_readable(timeout);
}

} // namespace buddy::mqtt
