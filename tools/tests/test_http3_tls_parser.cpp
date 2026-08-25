#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "quic/quic_crypto.h"
#include "quic/quic_frame.h"
#include "tls/tls_handshake.h"

namespace esp_http3::quic {

size_t BuildTransportParameters(const TransportParameters&, uint8_t*, size_t) { return 0U; }

bool ParseTransportParameters(const uint8_t*, size_t, TransportParameters* params) {
    *params = {};
    return true;
}

bool HkdfExtract(const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*) { return false; }

bool HkdfExpandLabel(const uint8_t*, size_t, const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*, size_t) {
    return false;
}

bool Sha256(const uint8_t*, size_t, uint8_t*) { return false; }

bool HmacSha256(const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*) { return false; }

}  // namespace esp_http3::quic

namespace {

using Bytes = std::vector<uint8_t>;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void AppendUint16(Bytes& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value >> 8U));
    bytes.push_back(static_cast<uint8_t>(value));
}

void AppendExtension(Bytes& extensions, uint16_t type, const Bytes& body) {
    AppendUint16(extensions, type);
    AppendUint16(extensions, static_cast<uint16_t>(body.size()));
    extensions.insert(extensions.end(), body.begin(), body.end());
}

Bytes ServerHello(const Bytes& extra_extensions = {}) {
    Bytes body{0x03U, 0x03U};
    body.insert(body.end(), 32U, 0x42U);
    body.push_back(0U);
    AppendUint16(body, esp_http3::tls::kTls13Aes128GcmSha256);
    body.push_back(0U);

    Bytes extensions;
    AppendExtension(extensions, 43U, {0x03U, 0x04U});
    Bytes key_share{0x00U, 0x1dU, 0x00U, 0x20U};
    key_share.insert(key_share.end(), 32U, 0x55U);
    AppendExtension(extensions, 51U, key_share);
    extensions.insert(extensions.end(), extra_extensions.begin(), extra_extensions.end());
    AppendUint16(body, static_cast<uint16_t>(extensions.size()));
    body.insert(body.end(), extensions.begin(), extensions.end());
    return body;
}

Bytes EncryptedExtensions(const Bytes& extra_extensions = {}) {
    Bytes extensions;
    AppendExtension(extensions, 16U, {0x00U, 0x03U, 0x02U, 'h', '3'});
    AppendExtension(extensions, 0x39U, {});
    extensions.insert(extensions.end(), extra_extensions.begin(), extra_extensions.end());
    Bytes body;
    AppendUint16(body, static_cast<uint16_t>(extensions.size()));
    body.insert(body.end(), extensions.begin(), extensions.end());
    return body;
}

void TestServerHello() {
    esp_http3::tls::ServerHelloData parsed{};
    const Bytes valid = ServerHello();
    Require(esp_http3::tls::ParseServerHello(valid.data(), valid.size(), &parsed), "valid ServerHello must parse");
    Require(parsed.selected_version == esp_http3::tls::kTls13Version, "TLS 1.3 must be selected");
    Require(parsed.key_share_group == esp_http3::tls::kX25519, "X25519 must be selected");

    Bytes duplicate_version;
    AppendExtension(duplicate_version, 43U, {0x03U, 0x04U});
    const Bytes duplicate = ServerHello(duplicate_version);
    parsed = {};
    Require(!esp_http3::tls::ParseServerHello(duplicate.data(), duplicate.size(), &parsed),
            "duplicate supported_versions must fail");

    Bytes missing_key_share = valid;
    missing_key_share.resize(missing_key_share.size() - 40U);
    const uint16_t shortened_extensions =
        static_cast<uint16_t>((missing_key_share[38] << 8U) | missing_key_share[39]) - 40U;
    missing_key_share[38] = static_cast<uint8_t>(shortened_extensions >> 8U);
    missing_key_share[39] = static_cast<uint8_t>(shortened_extensions);
    parsed = {};
    Require(!esp_http3::tls::ParseServerHello(missing_key_share.data(), missing_key_share.size(), &parsed),
            "missing key_share must fail");
}

void TestEncryptedExtensions() {
    esp_http3::tls::EncryptedExtensionsData parsed{};
    const Bytes valid = EncryptedExtensions();
    Require(esp_http3::tls::ParseEncryptedExtensions(valid.data(), valid.size(), &parsed),
            "valid EncryptedExtensions must parse");
    Require(parsed.alpn == "h3" && parsed.has_transport_params, "h3 and QUIC parameters must be present");

    Bytes duplicate_alpn;
    AppendExtension(duplicate_alpn, 16U, {0x00U, 0x03U, 0x02U, 'h', '3'});
    const Bytes duplicate = EncryptedExtensions(duplicate_alpn);
    parsed = {};
    Require(!esp_http3::tls::ParseEncryptedExtensions(duplicate.data(), duplicate.size(), &parsed),
            "duplicate ALPN must fail");

    Bytes trailing = valid;
    trailing.push_back(0U);
    parsed = {};
    Require(!esp_http3::tls::ParseEncryptedExtensions(trailing.data(), trailing.size(), &parsed),
            "trailing EncryptedExtensions data must fail");
}

void TestCertificateAndFinished() {
    const Bytes certificate{0x00U, 0x00U, 0x00U, 0x06U, 0x00U, 0x00U, 0x01U, 0xaaU, 0x00U, 0x00U};
    esp_http3::tls::CertificateData parsed_certificate{};
    Require(esp_http3::tls::ParseCertificate(certificate.data(), certificate.size(), &parsed_certificate),
            "bounded certificate list must parse");
    Require(parsed_certificate.certificates.size() == 1U, "one certificate must be returned");

    Bytes truncated_certificate = certificate;
    truncated_certificate.pop_back();
    parsed_certificate = {};
    Require(!esp_http3::tls::ParseCertificate(truncated_certificate.data(), truncated_certificate.size(),
                                              &parsed_certificate),
            "truncated certificate extensions must fail");

    const Bytes signature{0x04U, 0x03U, 0x00U, 0x02U, 0xaaU, 0xbbU};
    esp_http3::tls::CertificateVerifyData parsed_signature{};
    Require(esp_http3::tls::ParseCertificateVerify(signature.data(), signature.size(), &parsed_signature),
            "exact CertificateVerify must parse");
    Bytes trailing_signature = signature;
    trailing_signature.push_back(0U);
    Require(!esp_http3::tls::ParseCertificateVerify(trailing_signature.data(), trailing_signature.size(),
                                                    &parsed_signature),
            "trailing CertificateVerify data must fail");

    std::array<uint8_t, 32U> finished{};
    esp_http3::tls::FinishedData parsed_finished{};
    Require(esp_http3::tls::ParseFinished(finished.data(), finished.size(), &parsed_finished),
            "32-byte Finished must parse");
    Require(!esp_http3::tls::ParseFinished(finished.data(), finished.size() - 1U, &parsed_finished),
            "short Finished must fail");
}

}  // namespace

int main() {
    TestServerHello();
    TestEncryptedExtensions();
    TestCertificateAndFinished();
    std::cout << "HTTP/3 TLS parser tests passed (16 assertions).\n";
    return 0;
}
