#include "feature_support.h"
#include <cstdio>

int main() {
    using namespace kyty_dlss;
    int failures = 0;
    const auto check = [&](bool ok, const char* reason) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", reason); ++failures; }
    };
    const SupportKey host {0x100, 0x200, L"C:/runtime/verified"};
    FeatureSupportCache cache;
    check(cache.Observe(host, NVSDK_NGX_Result_FAIL_NotImplemented, 0) == SupportDecision::QueryFailed,
          "first unavailable query is not evidence of support");
    // Device extension discovery -> Streamline initializes NGX -> native SR.
    check(cache.Observe(host, NVSDK_NGX_Result_Success, 0) == SupportDecision::Supported,
          "pre-device query records affirmative support");
    check(cache.Observe(host, NVSDK_NGX_Result_FAIL_NotImplemented, 0) == SupportDecision::VerifiedEarlier,
          "post-initialization NotImplemented uses exact earlier support");
    for (const auto& other : {SupportKey {0x101, host.physical, host.runtime},
                             SupportKey {host.instance, 0x201, host.runtime},
                             SupportKey {host.instance, host.physical, L"C:/runtime/other"}}) {
        check(cache.Observe(other, NVSDK_NGX_Result_FAIL_NotImplemented, 0) == SupportDecision::QueryFailed,
              "instance, GPU or runtime mismatch cannot borrow support");
    }
    for (const auto error : {NVSDK_NGX_Result_FAIL_FeatureNotSupported,
                            NVSDK_NGX_Result_FAIL_OutOfDate, NVSDK_NGX_Result_FAIL_InvalidParameter,
                            NVSDK_NGX_Result_FAIL_PlatformError, static_cast<NVSDK_NGX_Result>(0),
                            static_cast<NVSDK_NGX_Result>(42)}) {
        check(cache.Observe(host, error, 0) == SupportDecision::QueryFailed,
              "all other query errors remain fatal despite earlier support");
        check(cache.Observe(host, NVSDK_NGX_Result_FAIL_NotImplemented, 0) == SupportDecision::QueryFailed,
              "query failure invalidates obsolete positive evidence");
        check(cache.Observe(host, NVSDK_NGX_Result_Success, 0) == SupportDecision::Supported,
              "fresh successful query may establish support again");
    }
    for (const uint32_t flags : {1u, 2u, 4u, 0xffffffffu}) {
        check(cache.Observe(host, NVSDK_NGX_Result_Success, 0) == SupportDecision::Supported,
              "fresh positive result before support is revoked");
        check(cache.Observe(host, NVSDK_NGX_Result_Success, flags) == SupportDecision::Unsupported,
              "every nonzero unsupported flag is rejected");
        check(cache.Observe(host, NVSDK_NGX_Result_FAIL_NotImplemented, 0) == SupportDecision::QueryFailed,
              "unsupported result cannot seed subsequent unavailable query");
    }
    if (!failures) std::puts("Feature support lifecycle contracts passed (CPU only; no NGX initialization).");
    return failures ? 1 : 0;
}
