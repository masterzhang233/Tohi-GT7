#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cfenv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace Libs::Graphics::Gt7NativeDepthProjection {

using Matrix = std::array<float, 16>;
using Slots = std::array<uint32_t, 16>;
constexpr uint32_t Missing = std::numeric_limits<uint32_t>::max();

enum class Status : uint32_t {
    NotAttempted, MissingPlan, Layout, GpuFills, UserData, Pointer, ConflictingSlots,
    BindlessPatch, MissingSlots, Ready, Witness, Feedback, Unmapped, DirtyBefore,
    ReadFailed, SnapshotChanged, DirtyAfter, NotEquivalent, Accepted
};

[[nodiscard]] constexpr const char* Name(Status status) {
    switch (status) {
        case Status::NotAttempted: return "not-attempted";
        case Status::MissingPlan: return "missing-runtime-plan";
        case Status::Layout: return "unproved-layout";
        case Status::GpuFills: return "gpu-filled-snapshot";
        case Status::UserData: return "missing-user-registers";
        case Status::Pointer: return "invalid-camera-pointer";
        case Status::ConflictingSlots: return "conflicting-flat-slots";
        case Status::BindlessPatch: return "bindless-patch-overlaps-matrix";
        case Status::MissingSlots: return "missing-matrix-flat-slots";
        case Status::Ready: return "snapshot-ready";
        case Status::Witness: return "input-witness-changed";
        case Status::Feedback: return "matrix-aliases-attachment";
        case Status::Unmapped: return "matrix-unmapped";
        case Status::DirtyBefore: return "matrix-backing-stale-before-read";
        case Status::ReadFailed: return "matrix-read-failed";
        case Status::SnapshotChanged: return "matrix-live-differs-from-bound-snapshot";
        case Status::DirtyAfter: return "matrix-backing-stale-after-read";
        case Status::NotEquivalent: return "effective-projection-not-equivalent";
        case Status::Accepted: return "equivalent-bound-projection";
    }
    return "unknown";
}

// These exact guest programs were audited through both prefix paths: at their
// merge, arbitrary model/source values feed one row vector times this camera's
// +80 float4x4 matrix in all four POS0 coordinates. No other position exit.
// This identifies a layout only; the draw's actual snapshot and effective
// projection still have to pass every check below.
[[nodiscard]] constexpr uint32_t CameraRegister(uint64_t shader) {
    switch (shader) {
        case 0x00fd05f507fd74f6ULL:
        case 0x1a52d4c406d6d7f7ULL:
        case 0xf7216d37fc69d3efULL:
        // Additional fixed programs have the same independently audited +80
        // layout and unchanged s12:s13 pointer through their prefix merge.
        case 0x717e8c021328b920ULL:
        case 0x1780d42d87d4bc0cULL:
        case 0x581995fe06924027ULL:
        case 0x6e0f21fc579dfb30ULL:
        case 0x06b929c5899a8b92ULL:
        // Nurburgring's additional material variants retain the original
        // s12:s13 through every prefix path and use +80 for the sole POS0.
        case 0x7707b1065ac8ecfcULL:
        case 0x59f19f9489ddc7cfULL:
        case 0xe2039ae1d652169bULL:
        case 0x0ad9fe700afe99a9ULL: return 12;
        case 0x3ea0d2b80b30e375ULL: return 10;
        default: return Missing;
    }
}

struct Attempt {
    bool native = false;
    bool candidate_valid = false;
    bool candidate_rejected = false;
    bool witness_pending = false;
    bool witness_matches = false;
    bool exact_depth = false;
    bool writes_depth = false;
    bool clear = false;
    bool trusted_vertex_raster = false;
    bool all_viewports_match = false;
};

[[nodiscard]] constexpr bool Eligible(const Attempt& a) {
    return a.native && a.candidate_valid && !a.candidate_rejected && !a.witness_pending &&
        a.witness_matches && a.exact_depth && a.writes_depth && !a.clear &&
        a.trusted_vertex_raster && a.all_viewports_match;
}

[[nodiscard]] constexpr bool PatchTouches(const Slots& slots, uint32_t offset) {
    for (auto slot : slots)
        if (slot == offset || (offset != Missing && slot == offset + 1u)) return true;
    return false;
}

// Slots come from the retained ResourcePlan's scalar-address expressions, never
// a shader-hash-dependent offset into flat data or a search by matching values.
[[nodiscard]] inline bool ReadSnapshot(const Slots& slots, std::span<const uint32_t> flat,
                                       bool gpu_fills, Matrix& result) {
    if (gpu_fills) return false;
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i] == Missing || slots[i] >= flat.size()) return false;
        for (size_t j = 0; j < i; ++j) if (slots[i] == slots[j]) return false;
        result[i] = std::bit_cast<float>(flat[slots[i]]);
    }
    return true;
}

[[nodiscard]] inline bool SameBits(const Matrix& a, const Matrix& b) {
    for (size_t i = 0; i < a.size(); ++i)
        if (std::bit_cast<uint32_t>(a[i]) != std::bit_cast<uint32_t>(b[i])) return false;
    return true;
}

struct DepthRange { float minimum, maximum; };

namespace Detail {
constexpr double UnitRoundoff = 0x1p-24;
constexpr double Gamma4 = 4.0 * UnitRoundoff / (1.0 - 4.0 * UnitRoundoff);
constexpr size_t MaxCandidates = 8;

[[nodiscard]] inline bool EqualTerms(double az, double aw, double bz, double bw) {
    const double ae = az + aw, be = bz + bw;
    const double participating = Gamma4 * (std::abs(az) + std::abs(aw) + std::abs(bz) + std::abs(bw));
    const double effective = Gamma4 * (std::abs(ae) + std::abs(be));
    return std::abs(ae - be) <= std::min(participating, effective);
}

struct Interval { double lower, upper; };
struct FloatCandidates { std::array<float, MaxCandidates> values {}; size_t count = 0; };
struct Factors { float a, b; };
struct FactorCandidates { std::array<Factors, MaxCandidates> values {}; size_t count = 0; };

// Inclusive midpoint cells include both sides of a tie. Exact forward rounding
// below resolves ties-to-even; interval arithmetic only constructs a superset.
[[nodiscard]] inline Interval RoundingCell(float value) {
    const float previous = std::nextafter(value, -std::numeric_limits<float>::infinity());
    const float next = std::nextafter(value, std::numeric_limits<float>::infinity());
    return {(double(previous) + double(value)) * 0.5, (double(value) + double(next)) * 0.5};
}

// Positive normal binary32 bit patterns are ordered. Enumerate the COMPLETE
// interval, or fail closed before looping; never search an arbitrary ULP radius.
[[nodiscard]] inline bool Enumerate(Interval interval, FloatCandidates& result) {
    result.count = 0;
    if (!std::isfinite(interval.lower) || !std::isfinite(interval.upper) ||
        interval.lower < double(std::numeric_limits<float>::min()) ||
        interval.upper > double(std::numeric_limits<float>::max()) || interval.lower > interval.upper) return false;
    float first = float(interval.lower), last = float(interval.upper);
    if (double(first) < interval.lower) first = std::nextafter(first, std::numeric_limits<float>::infinity());
    if (double(last) > interval.upper) last = std::nextafter(last, -std::numeric_limits<float>::infinity());
    if (first > last) return true;
    const uint32_t begin = std::bit_cast<uint32_t>(first), end = std::bit_cast<uint32_t>(last);
    const uint64_t count = uint64_t(end) - uint64_t(begin) + 1;
    if (count > MaxCandidates) return false;
    for (uint64_t i = 0; i < count; ++i) result.values[result.count++] = std::bit_cast<float>(begin + uint32_t(i));
    return true;
}

[[nodiscard]] inline bool NormalOrZero(double value) {
    return std::isfinite(value) && (value == 0.0 || std::abs(value) >= double(std::numeric_limits<float>::min()));
}

// Sufficient canonical reverse-Z construction: Zxyz = fl(-a * Wxyz),
// Z3 = fl(-a * W3 + b) or fl(fl(-a * W3) + b), with positive normal
// binary32 a,b. This separates view/projection construction rounding from the
// physical depth coefficients, without claiming the guest used either routine.
// Subnormal/overflow inputs and ambiguous wide intervals are intentionally not
// supported. The same zero-sign equivalence as the outer matrix check applies.
[[nodiscard]] inline bool Recover(const Matrix& matrix, FactorCandidates& result) {
    result.count = 0;
    if (std::fegetround() != FE_TONEAREST) return false;
    Interval range {double(std::numeric_limits<float>::min()), double(std::numeric_limits<float>::max())};
    bool nonzero = false;
    for (size_t row = 0; row < 3; ++row) {
        const float z = matrix[row * 4 + 2], w = matrix[row * 4 + 3];
        if (w == 0.0f) { if (z != 0.0f) return false; continue; }
        if (!std::isnormal(z) || !std::isnormal(w)) return false;
        nonzero = true;
        const auto cell = RoundingCell(z);
        double lower = cell.lower / -double(w), upper = cell.upper / -double(w);
        if (lower > upper) std::swap(lower, upper);
        // Division may round in double. One outward step contains the exact
        // rational endpoints; final binary32 reconstruction removes extras.
        range.lower = std::max(range.lower, std::nextafter(lower, -std::numeric_limits<double>::infinity()));
        range.upper = std::min(range.upper, std::nextafter(upper, std::numeric_limits<double>::infinity()));
    }
    if (!nonzero) return false;
    FloatCandidates a_values;
    if (!Enumerate(range, a_values)) return false;
    const float z = matrix[14], w = matrix[15];
    if (!NormalOrZero(z) || !NormalOrZero(w)) return false;
    const auto cell = RoundingCell(z);
    for (size_t ai = 0; ai < a_values.count; ++ai) {
        const float a = a_values.values[ai];
        bool matches = true;
        for (size_t row = 0; row < 3; ++row) {
            const double product = -double(a) * double(matrix[row * 4 + 3]);
            matches &= NormalOrZero(product) && std::abs(product) <= double(std::numeric_limits<float>::max()) &&
                float(product) == matrix[row * 4 + 2];
        }
        if (!matches) continue;
        // The product of two binary32 values is exact in double. The two
        // construction modes yield a union of b intervals; its hull may include
        // extra values, but none can survive the exact reconstruction check.
        const double product = -double(a) * double(w);
        if (!NormalOrZero(product) || std::abs(product) > double(std::numeric_limits<float>::max())) return false;
        const float rounded_product = float(product);
        Interval b_range {
            std::nextafter(std::min(cell.lower - product, cell.lower - double(rounded_product)),
                           -std::numeric_limits<double>::infinity()),
            std::nextafter(std::max(cell.upper - product, cell.upper - double(rounded_product)),
                           std::numeric_limits<double>::infinity())};
        FloatCandidates b_values;
        if (!Enumerate(b_range, b_values)) return false;
        for (size_t bi = 0; bi < b_values.count; ++bi) {
            const float b = b_values.values[bi];
            const double fused_sum = product + double(b), separate_sum = double(rounded_product) + double(b);
            const bool fused = NormalOrZero(fused_sum) && std::abs(fused_sum) <= double(std::numeric_limits<float>::max()) &&
                std::fma(-a, w, b) == z;
            const bool separate = NormalOrZero(separate_sum) && std::abs(separate_sum) <= double(std::numeric_limits<float>::max()) &&
                float(separate_sum) == z;
            if (!fused && !separate) continue;
            if (result.count == MaxCandidates) return false;
            result.values[result.count++] = {a,b};
        }
    }
    return result.count != 0;
}

[[nodiscard]] inline bool CanonicalEquivalent(const Matrix& actual, DepthRange a,
                                               const Matrix& saved, DepthRange b) {
    FactorCandidates aa, bb;
    if (!Recover(actual, aa) || !Recover(saved, bb)) return false;
    const double as = double(a.maximum) - double(a.minimum), bs = double(b.maximum) - double(b.minimum);
    // Require ALL possible constructions to agree, rather than selecting a
    // convenient b from a translation-cancellation interval. At most 8x8 pairs.
    for (size_t i = 0; i < aa.count; ++i) {
        for (size_t j = 0; j < bb.count; ++j) {
            if (!EqualTerms(as * double(aa.values[i].a), -double(a.minimum),
                            bs * double(bb.values[j].a), -double(b.minimum)) ||
                !EqualTerms(as * double(aa.values[i].b), 0.0, bs * double(bb.values[j].b), 0.0)) return false;
        }
    }
    return true;
}
} // namespace Detail

[[nodiscard]] inline bool Equivalent(const Matrix& actual, DepthRange a,
                                      const Matrix& saved, DepthRange b) {
    const auto valid = [](DepthRange r) {
        return std::isfinite(r.minimum) && std::isfinite(r.maximum) &&
            0.0f <= r.minimum && r.minimum < r.maximum && r.maximum <= 1.0f;
    };
    if (!valid(a) || !valid(b)) return false;
    for (size_t i = 0; i < actual.size(); ++i) {
        if (!std::isfinite(actual[i]) || !std::isfinite(saved[i])) return false;
        // Keep pose and jitter strict. Only the depth column may compensate a
        // different viewport affine. Numerically equal signed zero is harmless.
        if (i % 4 != 2 && actual[i] != saved[i]) return false;
    }
    const double as = double(a.maximum) - double(a.minimum);
    const double bs = double(b.maximum) - double(b.minimum);
    bool direct = true;
    for (size_t row = 0; row < 4; ++row) {
        if (!Detail::EqualTerms(as * double(actual[row * 4 + 2]), double(a.minimum) * double(actual[row * 4 + 3]),
                                bs * double(saved[row * 4 + 2]), double(b.minimum) * double(saved[row * 4 + 3]))) {
            direct = false;
            break;
        }
    }
    // Preserve the existing cheap path. Only a failed depth-column comparison
    // attempts the bounded canonical model; all eligibility/resource gates and
    // exact XY/W comparisons remain prerequisites. Neither path claims identical
    // per-pixel float rasterization or verifies an arbitrary shader's semantics.
    return direct || Detail::CanonicalEquivalent(actual, a, saved, b);
}

} // namespace Libs::Graphics::Gt7NativeDepthProjection
