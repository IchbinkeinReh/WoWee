// The system's trust decision on Apple platforms (core/tls_trust.hpp).
//
// Its own unit, apart from tls_trust.cpp: Security's headers and OpenSSL's
// are not included together anywhere, so neither can trip over the other's
// names. This side sees DER bytes and a host name only.

#include "core/tls_trust.hpp"

// AssertMacros.h, which Security brings in, otherwise defines check(),
// verify() and require() as macros.
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

namespace wowee::core {

bool appleTrustsChain(const DerChain& chain, const std::string& host) {
    // No name, no answer: SecPolicyCreateSSL with none would skip the name
    // check, and a certificate for any host at all would then do.
    if (chain.empty() || host.empty()) return false;

    CFMutableArrayRef certs =
        CFArrayCreateMutable(kCFAllocatorDefault, static_cast<CFIndex>(chain.size()),
                             &kCFTypeArrayCallBacks);
    if (certs == nullptr) return false;
    bool complete = true;
    for (const auto& der : chain) {
        CFDataRef data = CFDataCreate(kCFAllocatorDefault, der.data(), static_cast<CFIndex>(der.size()));
        SecCertificateRef cert =
            data != nullptr ? SecCertificateCreateWithData(kCFAllocatorDefault, data) : nullptr;
        if (data != nullptr) CFRelease(data);
        if (cert == nullptr) {
            complete = false;
            break;
        }
        CFArrayAppendValue(certs, cert);
        CFRelease(cert);
    }

    bool trusted = false;
    CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, host.c_str(), kCFStringEncodingUTF8);
    SecPolicyRef policy = name != nullptr ? SecPolicyCreateSSL(true, name) : nullptr;
    if (complete && policy != nullptr) {
        SecTrustRef trust = nullptr;
        if (SecTrustCreateWithCertificates(certs, policy, &trust) == errSecSuccess && trust != nullptr) {
            CFErrorRef error = nullptr;
            trusted = SecTrustEvaluateWithError(trust, &error);
            if (error != nullptr) CFRelease(error);
            CFRelease(trust);
        }
    }
    if (policy != nullptr) CFRelease(policy);
    if (name != nullptr) CFRelease(name);
    CFRelease(certs);
    return trusted;
}

}  // namespace wowee::core
