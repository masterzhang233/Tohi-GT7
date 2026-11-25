#pragma once

#include <windows.h>
#include <stdexcept>

// Internal configuration shared with the SDK-free diagnostic host. This does
// not change the public C ABI. Read once before initializing each NGX context;
// never switch a persistent feature's model partway through its history.
namespace kyty_dlss {
inline constexpr wchar_t PresetEnvironment[] = L"KYTY_GT7_DLSS_PRESET";
struct ModelPreset {
    char letter;
    bool explicitly_requested;
    const char* generation() const { return letter == 'K' ? "DLSS 4.0" : "DLSS 4.5"; }
};
inline ModelPreset ReadModelPreset() {
    wchar_t value[8] {};
    SetLastError(ERROR_SUCCESS);
    const auto length = GetEnvironmentVariableW(PresetEnvironment, value, 8);
    if (!length && (GetLastError() == ERROR_ENVVAR_NOT_FOUND || GetLastError() == ERROR_SUCCESS))
        return {'K', false};
    if (length == 1 && (value[0] == L'K' || value[0] == L'M' || value[0] == L'L'))
        return {static_cast<char>(value[0]), true};
    throw std::invalid_argument("KYTY_GT7_DLSS_PRESET must be K, M or L (unset defaults to K)");
}
} // namespace kyty_dlss
