#pragma once

#include <nvsdk_ngx_defs.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace kyty_dlss {
struct SupportKey {
    uintptr_t instance {}, physical {};
    std::wstring runtime;
    bool operator==(const SupportKey&) const = default;
};

enum class SupportDecision { Supported, VerifiedEarlier, Unsupported, QueryFailed };

// All calls are protected by the backend's global lifecycle mutex. Store only
// affirmative SDK evidence, never infer support from extensions or an FG device.
class FeatureSupportCache {
public:
    SupportDecision Observe(const SupportKey& key, NVSDK_NGX_Result result, uint32_t support_flags) {
        const auto found = std::find(verified.begin(), verified.end(), key);
        if (result == NVSDK_NGX_Result_FAIL_NotImplemented) {
            return found != verified.end() ? SupportDecision::VerifiedEarlier : SupportDecision::QueryFailed;
        }
        if (result != NVSDK_NGX_Result_Success || support_flags != 0) {
            // New negative evidence supersedes any earlier positive result.
            if (found != verified.end()) verified.erase(found);
            return result != NVSDK_NGX_Result_Success ? SupportDecision::QueryFailed : SupportDecision::Unsupported;
        }
        if (found == verified.end()) verified.push_back(key);
        return SupportDecision::Supported;
    }
private:
    std::vector<SupportKey> verified;
};
} // namespace kyty_dlss
