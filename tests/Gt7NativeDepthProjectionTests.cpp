#include "graphics/host_gpu/renderer/gt7NativeDepthProjection.h"
#include "graphics/host_gpu/renderer/gt7FrameGenInputWitness.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
using namespace Libs::Graphics::Gt7NativeDepthProjection;
namespace Witness = Libs::Graphics::Gt7FrameGenInputWitness;
int checks = 0;
void Require(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

Matrix Perspective(double near_plane, double far_plane, float jitter = 0.0f) {
    // Independent reverse-Z perspective construction, row-vector convention.
    return {1.1f,0,0,0, 0,1.9f,0,0, jitter,0,
        float(near_plane / (far_plane - near_plane)),-1, 0,0,
        float(near_plane * far_plane / (far_plane - near_plane)),0};
}

Matrix Product(const Matrix& view, const Matrix& projection) {
    Matrix result {};
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            double value = 0;
            for (size_t k = 0; k < 4; ++k) value += double(view[row*4+k])*projection[k*4+column];
            result[row*4+column] = float(value);
        }
    }
    return result;
}
}

int main() {
    const DepthRange observed {std::bit_cast<float>(uint32_t {0x3606a8c9}), 1.0f};
    // The exact logged value, not a decimal epsilon chosen to make a test pass.
    Require(observed.minimum == 2.0065801891178126e-6f, "observed offset bits");
    const DepthRange saved_range {0,1};
    const auto source_projection = Perspective(.01, 3800);
    const auto saved_projection = Perspective(.01, 16000);
    const Matrix identity {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const Matrix rotated {.8f,0,.6f,0, 0,1,0,0, -.6f,0,.8f,0, 100,200,-300,1};
    const Matrix translated {.8f,0,.6f,0, 0,1,0,0, -.6f,0,.8f,0, 10000,-20000,6000,1};
    for (const auto& view : {identity, rotated, translated}) {
        const auto actual = Product(view, source_projection);
        const auto saved = Product(view, saved_projection);
        Require(Equivalent(actual, observed, saved, saved_range), "far-plane compensation accepts bound VP with same pose and jitter");
        Require(!Equivalent(saved, observed, saved, saved_range), "raw viewport bias without projection compensation rejects");
        Require(!Equivalent(actual, saved_range, saved, saved_range), "different far projection without compensating viewport rejects");
        Require(!Equivalent(Product(view, Perspective(.01001,3800)), observed, saved, saved_range), "different near plane rejects");
        Require(!Equivalent(Product(view, Perspective(.01,3900)), observed, saved, saved_range), "different far plane rejects");
        Require(!Equivalent(Product(view, Perspective(.01,3800,1e-6f)), observed, saved, saved_range), "different representable jitter rejects");
        auto moved = view;
        moved[12] += 1.0f;
        Require(!Equivalent(Product(moved, source_projection), observed, saved, saved_range), "different representable camera position rejects");
        auto turned = view;
        turned[0] += .01f;
        Require(!Equivalent(Product(turned, source_projection), observed, saved, saved_range), "different camera rotation rejects");
        Require(Equivalent(saved, saved_range, saved, saved_range), "identical finite projection accepts");
    }
    // A large offset makes the participating terms enormous compared with the
    // reverse-Z result. The result-relative cap must reject this false equality.
    auto cancellation = saved_projection;
    cancellation[10] = float((double(saved_projection[10]) + .5) / .5);
    cancellation[14] = float(double(saved_projection[14]) / .5);
    Require(!Equivalent(cancellation, {.5f,1}, saved_projection, saved_range), "large cancellation cannot swallow a real far-plane discrepancy");
    auto near_zero = identity;
    near_zero[14] = -16000;
    Require(Equivalent(Product(near_zero, source_projection), observed,
                       Product(near_zero, saved_projection), saved_range),
            "canonical construction resolves nearzero translated coefficient without an absolute epsilon");
    auto large_translation = identity;
    large_translation[14] = -100000.0f;
    Require(!Detail::CanonicalEquivalent(Product(large_translation, source_projection), observed,
                        Product(large_translation, saved_projection), saved_range),
            "canonical wide translation ambiguity refuses instead of truncating candidates");
    large_translation[14] = -1e20f;
    Require(!Detail::CanonicalEquivalent(Product(large_translation, source_projection), observed,
                        Product(large_translation, saved_projection), saved_range),
            "canonical enormous translation has bounded work and cannot conceal the near coefficient");
    cancellation[10] = float((double(saved_projection[10]) + double(std::nextafter(1.0f,0.0f))) /
                              (1.0-double(std::nextafter(1.0f,0.0f))));
    cancellation[14] = float(double(saved_projection[14]) / (1.0-double(std::nextafter(1.0f,0.0f))));
    Require(!Equivalent(cancellation, {std::nextafter(1.0f,0.0f),1}, saved_projection, saved_range),
            "near-one viewport offset cannot conceal a real far-plane discrepancy");
    Matrix zero {}, tiny {};
    tiny[2] = 1e-30f;
    Require(!Equivalent(tiny, saved_range, zero, saved_range), "no absolute epsilon floor accepts a tiny nonzero depth coefficient");
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()}) {
        for (size_t i = 0; i < 16; ++i) {
            auto bad = source_projection;
            bad[i] = invalid;
            Require(!Equivalent(bad, observed, saved_projection, saved_range), "nonfinite actual matrix rejects");
            bad = saved_projection;
            bad[i] = invalid;
            Require(!Equivalent(source_projection, observed, bad, saved_range), "nonfinite saved matrix rejects");
        }
        Require(!Equivalent(source_projection, {invalid,1}, saved_projection, saved_range), "nonfinite viewport rejects");
    }
    Require(!Equivalent(source_projection, {1,0}, saved_projection, saved_range), "reversed viewport rejects");
    Require(!Equivalent(source_projection, {0,0}, saved_projection, saved_range), "degenerate viewport rejects");
    Require(!Equivalent(source_projection, {-.1f,1}, saved_projection, saved_range), "out-of-range viewport rejects");


    // Completed 20261125-205259-fd1dc4f4 run: all 30 sampled bound snapshots.
    // %.9g diagnostic values round-trip to these exact binary32 words. Only Z/W
    // were logged: X/Y below are identical synthetic placeholders, NOT a claim
    // that the unlogged full runtime matrices match. Thirteen Z pairs failed
    // the previous row-wise bound (e.g. frame7238), so these are regression cases.
    struct LoggedDepth { uint32_t frame; std::array<uint32_t,12> zw; };
    const std::array<LoggedDepth,30> logged {{
        {6042, {0xb463118eu, 0x3241ca68u, 0xb63007aeu, 0x3c5e088fu, 0xb357b6f6u, 0x313819cau, 0xb5273a68u, 0x3c31a931u, 0x3da493c0u, 0xbb8c7527u, 0x3f7f2b74u, 0xc4a8b5ffu}},
        {6042, {0xb463118eu, 0x3241ca68u, 0xb63007aeu, 0x3c5e088fu, 0xb357b6f6u, 0x313819cau, 0xb5273a68u, 0x3c31a931u, 0x3da493c0u, 0xbb8c7527u, 0x3f7f2b74u, 0xc4a8b5ffu}},
        {6043, {0xb462f092u, 0x33bfd0efu, 0xb62fee1eu, 0x3c5dccf3u, 0xb35797a0u, 0x32b63996u, 0xb527221fu, 0x3c319b09u, 0x3da47bd8u, 0xbd0b06cau, 0x3f7f0666u, 0xc4a8092du}},
        {7075, {0x35ed7dd5u, 0xb4272123u, 0xb6024d8fu, 0x3c61809du, 0x34e19dd1u, 0xb31ec5ccu, 0xb4f7933au, 0x3c327c1bu, 0xbf2c21b1u, 0x3d724486u, 0x3f3ce27cu, 0xc4b2c49cu}},
        {7078, {0x35f659b8u, 0xb42173aau, 0xb5fc4e53u, 0x3c60c48fu, 0x34ea084eu, 0xb31960ffu, 0xb4efb0adu, 0x3c324f71u, 0xbf328d6fu, 0x3d6a0998u, 0x3f36de74u, 0xc4b0a368u}},
        {7079, {0x35f7dc7du, 0xb422450eu, 0xb5fad04au, 0x3c60aed6u, 0x34eb77bdu, 0xb31a27ebu, 0xb4ee45bfu, 0x3c324a49u, 0xbf33a5c3u, 0x3d6b3920u, 0x3f35c98fu, 0xc4b0646fu}},
        {7231, {0x35a0a9c8u, 0xb43074bbu, 0xb61ce33du, 0x3c6bc242u, 0x3498a136u, 0xb327a201u, 0xb5150affu, 0x3c34ebb2u, 0xbee8e4f4u, 0x3d7fc988u, 0x3f636bdeu, 0xc4d080c7u}},
        {7238, {0x35973ab3u, 0xb42ccae3u, 0xb61f3afcu, 0x3c6c3182u, 0x348faae3u, 0xb324270eu, 0xb51744c1u, 0x3c35061eu, 0xbedb3818u, 0x3d7a7a0eu, 0x3f66d13fu, 0xc4d1c34eu}},
        {7371, {0x36302317u, 0xb42dcc43u, 0x33d9a902u, 0x3c4119a9u, 0x35275472u, 0xb3251b90u, 0x32cec6d9u, 0x3c2aca0fu, 0xbf7f5330u, 0x3d7bef24u, 0xbd1dc20fu, 0xc429a865u}},
        {7478, {0x361a288fu, 0xb3c0092bu, 0xb5abe747u, 0x3c655a92u, 0x35127340u, 0xb2b66f02u, 0xb4a34ed3u, 0x3c336645u, 0xbf5f7713u, 0x3d0b2f8cu, 0x3ef9301du, 0xc4bdef0eu}},
        {7611, {0x35fc130du, 0xb413b836u, 0x35f6b8cbu, 0x3c10efb0u, 0x34ef785eu, 0xb30c5553u, 0x34ea62a1u, 0x3c1f59b2u, 0xbf36b37eu, 0x3d5621a9u, 0xbf32d258u, 0x43db3990u}},
        {7768, {0x362189b4u, 0x339b3c64u, 0x358e6a3au, 0x3c329248u, 0x351975efu, 0x3293794au, 0x34874b3eu, 0x3c2756b5u, 0xbf6a298bu, 0xbce106eau, 0xbece7127u, 0xc3aad470u}},
        {7885, {0x35a5efabu, 0x348088a1u, 0xb61b11e8u, 0x3c71e71du, 0x349da39au, 0x337436aau, 0xb51350efu, 0x3c36613fu, 0xbef089b9u, 0xbdba51edu, 0x3f60c955u, 0xc4e250beu}},
        {8012, {0x361a51cbu, 0x348d7d23u, 0xb5a80ff3u, 0x3c71c78cu, 0x35129a6du, 0x33866a02u, 0xb49fa8aau, 0x3c3659c0u, 0xbf5fb2d9u, 0xbdcd1979u, 0x3ef39eb3u, 0xc4e1f53au}},
        {8238, {0x362e43eau, 0x33c52fadu, 0x34dfbccfu, 0x3c573a7eu, 0x35258d3bu, 0x32bb5398u, 0x33d48cdbu, 0x3c300b74u, 0xbf7c9c96u, 0xbd0eeb31u, 0xbe2229b0u, 0xc494fb78u}},
        {8421, {0x36219c6du, 0x33dc416eu, 0xb58dbf21u, 0x3c8090c1u, 0x351987b9u, 0x32d13e0cu, 0xb486a8b3u, 0x3c39ff1du, 0xbf6a44afu, 0xbd1fa3a0u, 0x3ecd7922u, 0xc5073b69u}},
        {8599, {0x35c8f8fbu, 0xb2816660u, 0x36113959u, 0x3c17e221u, 0x34beec6eu, 0xb175dbfcu, 0x3509f668u, 0x3c210013u, 0xbf11a9bcu, 0x3bbb935eu, 0xbf528394u, 0x438aa998u}},
        {8782, {0x353299efu, 0x337beb71u, 0x362ad1dcu, 0x3bf7e2dau, 0x3429abbfu, 0x326f52beu, 0x35224748u, 0x3c1a5d50u, 0xbe8172d3u, 0xbcb696c9u, 0xbf779dfau, 0x4467567eu}},
        {8976, {0x36306b46u, 0xb344b492u, 0x33ee33c8u, 0x3c766804u, 0x35279905u, 0xb23adea4u, 0x32e24aabu, 0x3c37730fu, 0xbf7fbbd3u, 0x3c8e91f7u, 0xbd2ca591u, 0xc4ef5f2fu}},
        {9146, {0x34a9fea5u, 0x343e1ce0u, 0x362eeaa6u, 0x3becbc2cu, 0x33a17ea0u, 0x33349b55u, 0x35262ba1u, 0x3c190a52u, 0xbdf66bcdu, 0xbd89cabdu, 0xbf7d8e48u, 0x4483d557u}},
        {9286, {0x360fbb8du, 0x345d0925u, 0x35cb5ba5u, 0x3c513735u, 0x35088bb3u, 0x3351fbc6u, 0x34c1308fu, 0x3c2e9de0u, 0xbf505a22u, 0xbda03460u, 0xbf136456u, 0xc4838cd4u}},
        {9290, {0x36118f63u, 0x345539e0u, 0x35c6395eu, 0x3c53242fu, 0x350a4824u, 0x334a9079u, 0x34bc5000u, 0x3c2f12f5u, 0xbf53004cu, 0xbd9a8b53u, 0xbf0fabc3u, 0xc489220bu}},
        {9466, {0x360ce289u, 0x3388542cu, 0x35d4cee1u, 0x3c58a45eu, 0x3505d723u, 0x32818318u, 0x34ca2ad3u, 0x3c306166u, 0xbf4c395eu, 0xbcc59ea0u, 0xbf1a3dc6u, 0xc499149au}},
        {9660, {0x35e13e3bu, 0xb4aecc9fu, 0x3606443bu, 0x3c4cc3c3u, 0x34d5fb01u, 0xb3a60f1au, 0x34ff1b19u, 0x3c2d8f41u, 0xbf23410au, 0x3dfd62c1u, 0xbf42a155u, 0xc46d4ac5u}},
        {9841, {0xb22f11d1u, 0xb2763cbdu, 0x3630994du, 0x3c05e9e7u, 0xb12650d6u, 0xb169ecc6u, 0x3527c4bfu, 0x3c1cbb86u, 0x3b7dc70fu, 0x3bb2786eu, 0xbf7ffe8bu, 0x442d8668u}},
        {10027, {0xb557d357u, 0xb3227c79u, 0x362823a3u, 0x3bda2b52u, 0xb44d08aau, 0xb21a5c91u, 0x351fbb5eu, 0x3c16d5ebu, 0x3e9c6da6u, 0x3c6b8975u, 0xbf73bb3du, 0x449ebefcu}},
        {10222, {0x339e305fu, 0x3316e998u, 0x3630848fu, 0x3c1a7353u, 0x32964779u, 0x320f5dd7u, 0x3527b10au, 0x3c219c29u, 0xbce54ec6u, 0xbc5ac287u, 0xbf7fe07au, 0x4359c8b4u}},
        {10428, {0x357ceadeu, 0x3494ec96u, 0x3623d76eu, 0x3c4a5c4cu, 0x34704565u, 0x338d7a48u, 0x351ba62cu, 0x3c2cfd15u, 0xbeb74feau, 0xbdd7e0adu, 0xbf6d8066u, 0xc45f5a1cu}},
        {11237, {0xb5dd27a6u, 0x34487d38u, 0x360920d3u, 0x3bb51d78u, 0xb4d218c1u, 0x333e76dbu, 0x35024583u, 0x3c126f7au, 0x3f204a7cu, 0xbd915008u, 0xbf46c749u, 0x44d4757eu}},
        {11513, {0x360899a9u, 0x3354832du, 0xb5dfc44bu, 0x3c676922u, 0x3501c51bu, 0x3249e2e9u, 0xb4d493f7u, 0x3c33e354u, 0xbf46035bu, 0xbc9a06e8u, 0x3f222f1du, 0xc4c3e5a4u}},
    }};
    for (const auto& row : logged) {
        auto actual = saved_projection, saved = saved_projection;
        for (size_t i = 0; i < 4; ++i) {
            actual[i*4+2] = std::bit_cast<float>(row.zw[i]);
            saved[i*4+2] = std::bit_cast<float>(row.zw[i+4]);
            actual[i*4+3] = saved[i*4+3] = std::bit_cast<float>(row.zw[i+8]);
        }
        if (!Equivalent(actual, observed, saved, saved_range)) {
            std::fprintf(stderr, "logged depth regression frame %u\n", row.frame);
            Require(false, "measured canonical compensation rejects");
        }
        Require(Detail::CanonicalEquivalent(actual, observed, saved, saved_range),
                "every measured Z/W pair reconstructs and every candidate agrees");
        Require(Equivalent(saved, saved_range, actual, observed), "compensation comparison is symmetric across native candidate ranges");
        auto no_compensation = saved;
        Require(!Equivalent(no_compensation, observed, saved, saved_range), "measured viewport without compensation rejects");
        auto wrong_pose = actual;
        wrong_pose[15] = std::nextafter(wrong_pose[15], std::numeric_limits<float>::infinity());
        Require(!Equivalent(wrong_pose, observed, saved, saved_range), "measured W one-ULP mismatch rejects");
        auto wrong_jitter = actual;
        wrong_jitter[0] = std::nextafter(wrong_jitter[0], std::numeric_limits<float>::infinity());
        Require(!Equivalent(wrong_jitter, observed, saved, saved_range), "measured X one-ULP mismatch rejects");
        auto noncanonical = actual;
        noncanonical[2] *= 1.01f;
        Require(!Equivalent(noncanonical, observed, saved, saved_range), "anisotropic Z change is not canonical compensation");
    }

    Detail::FloatCandidates enumerated;
    const float eighth = std::bit_cast<float>(std::bit_cast<uint32_t>(1.0f)+7u);
    const float ninth = std::bit_cast<float>(std::bit_cast<uint32_t>(1.0f)+8u);
    Require(Detail::Enumerate({1.0,double(eighth)},enumerated) && enumerated.count == 8,
            "candidate bound includes complete eight-value interval");
    Require(!Detail::Enumerate({1.0,double(ninth)},enumerated), "nine-value interval refuses rather than truncates");
    const auto one_cell = Detail::RoundingCell(1.0f);
    Require(Detail::Enumerate(one_cell,enumerated) && enumerated.count == 1 && enumerated.values[0] == 1.0f,
            "asymmetric power-of-two rounding cell yields exact representable candidate");
    Require(!Detail::Enumerate({0.0,1.0},enumerated), "interval extending to underflow domain rejects");
    Require(!Detail::Enumerate({1.0,std::numeric_limits<double>::infinity()},enumerated), "unbounded interval rejects");
    Detail::FactorCandidates factors;
    auto subnormal = source_projection;
    subnormal[10] = std::numeric_limits<float>::denorm_min();
    Require(!Detail::Recover(subnormal,factors), "subnormal projection product is outside canonical model");
    auto underflow = source_projection;
    underflow[10] = 0.0f;
    Require(!Detail::Recover(underflow,factors), "nonzero W cannot hide an underflowed Z product");
    auto undetermined = source_projection;
    undetermined[11] = undetermined[10] = 0.0f;
    Require(!Detail::Recover(undetermined,factors), "no nonzero W direction cannot determine projection");
    const int rounding = std::fegetround();
    if (std::fesetround(FE_DOWNWARD) == 0) {
        Require(!Detail::Recover(source_projection,factors), "non-nearest rounding model conservatively rejects");
        Require(std::fesetround(rounding) == 0, "restore original rounding mode");
    }
    // Multiple b values can produce the same translated Z. One compatible pair
    // is insufficient: this example has both compatible and incompatible b's.
    auto ambiguous = source_projection, ambiguous_saved = saved_projection;
    ambiguous[10] = std::bit_cast<float>(uint32_t {0x36309a4e});
    ambiguous[14] = -0.03210535645484924f;
    ambiguous_saved[14] = 1.8189894035458565e-10f;
    ambiguous[15] = ambiguous_saved[15] = 16000.0f;
    Require(Detail::Recover(ambiguous,factors) && factors.count > 1, "ambiguous b fixture has multiple exact reconstructions");
    Detail::FactorCandidates saved_factors;
    Require(Detail::Recover(ambiguous_saved,saved_factors), "ambiguous fixture has a canonical saved projection");
    bool any_pair_pass = false, any_pair_fail = false;
    const double slope = double(observed.maximum) - double(observed.minimum);
    for (size_t i = 0; i < factors.count; ++i) {
        for (size_t j = 0; j < saved_factors.count; ++j) {
            const bool pass = Detail::EqualTerms(slope * factors.values[i].a, -double(observed.minimum),
                                                 saved_factors.values[j].a, 0.0) &&
                Detail::EqualTerms(slope * factors.values[i].b, 0.0, saved_factors.values[j].b, 0.0);
            any_pair_pass |= pass;
            any_pair_fail |= !pass;
        }
    }
    Require(any_pair_pass && any_pair_fail, "negative control distinguishes all-pair validation from cherry-picking");
    Require(!Equivalent(ambiguous,observed,ambiguous_saved,saved_range),
            "all exact reconstructions must agree; do not cherry-pick one compatible b");

    Require(CameraRegister(0x00fd05f507fd74f6ULL) == 12, "00fd uses actual s12 camera pointer");
    Require(CameraRegister(0x1a52d4c406d6d7f7ULL) == 12, "1a52 uses actual s12 camera pointer");
    Require(CameraRegister(0xf7216d37fc69d3efULL) == 12, "f721 uses actual s12 camera pointer");
    Require(CameraRegister(0x3ea0d2b80b30e375ULL) == 10, "3ea0 uses actual s10 camera pointer");
    for (uint64_t hash : {0x717e8c021328b920ULL, 0x1780d42d87d4bc0cULL, 0x581995fe06924027ULL,
                          0x6e0f21fc579dfb30ULL, 0x06b929c5899a8b92ULL})
        Require(CameraRegister(hash) == 12, "additional audited guest ISA uses unchanged s12:s13 camera pointer");
    Require(CameraRegister(0) == Missing, "unknown shader cannot use the static layout proof");

    Slots slots {};
    std::array<uint32_t,48> flat {};
    for (size_t i = 0; i < slots.size(); ++i) {
        // A deliberately noncontiguous semantic map defeats any assumed flat offset.
        slots[i] = uint32_t(2*i + 3);
        flat[slots[i]] = std::bit_cast<uint32_t>(source_projection[i]);
    }
    Matrix snapshot {};
    Require(ReadSnapshot(slots, flat, false, snapshot) && SameBits(snapshot, source_projection), "semantic 16-slot map reads exact bound matrix");
    Require(!ReadSnapshot(slots, flat, true, snapshot), "GPU-filled snapshot rejects");
    for (size_t i = 0; i < slots.size(); ++i) {
        auto bad = slots;
        bad[i] = Missing;
        Require(!ReadSnapshot(bad, flat, false, snapshot), "every matrix coefficient must be resolved");
        bad[i] = uint32_t(flat.size());
        Require(!ReadSnapshot(bad, flat, false, snapshot), "out-of-bounds snapshot slot rejects");
        Require(PatchTouches(slots, slots[i]), "bindless patch first word overlap rejects");
        Require(PatchTouches(slots, slots[i]-1), "bindless patch second word overlap rejects");
    }
    auto duplicated = slots;
    duplicated[1] = duplicated[0];
    Require(!ReadSnapshot(duplicated, flat, false, snapshot), "two coefficients cannot alias the same flat slot");
    Require(!PatchTouches(slots, 40), "disjoint appended bindless patch preserves snapshot");
    Require(ReadSnapshot(slots, flat, false, snapshot), "restore exact snapshot");
    auto late_cpu_write = source_projection;
    late_cpu_write[10] = std::nextafter(late_cpu_write[10], 1.0f);
    Require(!SameBits(snapshot, late_cpu_write), "even one-ULP live write after snapshot must not masquerade as bound data");

    const Witness::Stamp depth {1,2,0x1000,0x100,4,3};
    const Witness::Stamp motion {3,4,0x3000,0x100,8,5};
    Attempt allowed {true,true,false,false,true,true,true,false,true,true};
    Require(Eligible(allowed), "only exact ordered native depth write may try equivalence");
    const std::array<bool Attempt::*,7> must_be_true {&Attempt::native, &Attempt::candidate_valid,
        &Attempt::witness_matches, &Attempt::exact_depth, &Attempt::writes_depth,
        &Attempt::trusted_vertex_raster, &Attempt::all_viewports_match};
    for (auto member : must_be_true) {
        auto attempt = allowed;
        attempt.*member = false;
        Require(!Eligible(attempt), "legacy, alias/wrong role, readonly, unsupported stage/raster or mixed viewport rejects");
    }
    for (auto member : {&Attempt::candidate_rejected, &Attempt::witness_pending, &Attempt::clear}) {
        auto attempt = allowed;
        attempt.*member = true;
        Require(!Eligible(attempt), "rejected, pending or any clear cannot rebase camera");
    }
    auto changed = depth;
    ++changed.epoch;
    allowed.witness_matches = Witness::Matches(depth,changed) && Witness::Matches(motion,motion);
    Require(!Eligible(allowed), "external write epoch invalidation remains independent of matrix equivalence");
    changed = motion;
    ++changed.version;
    allowed.witness_matches = Witness::Matches(depth,depth) && Witness::Matches(motion,changed);
    Require(!Eligible(allowed), "motion version invalidation cannot be repaired by matching depth projection");
    std::printf("Gt7NativeDepthProjectionTests: %d checks passed\n", checks);
    return 0;
}
