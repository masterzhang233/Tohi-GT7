#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <utility>

namespace Libs::Graphics::Gt7DlssFrameGenCamera {

// GT7's accepted camera contract is row-vector, right-handed, -Z forward,
// reversed zero-to-one device depth. Matrices here never include TAA jitter.
using Matrix = std::array<double, 16>;
struct Camera {
    std::array<float, 3> position {}, right {}, up {}, forward {};
    float near_plane = 0, far_plane = 0, fov_y = 0, aspect = 0;
    bool infinite = false;
};
inline Matrix Identity() { return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; }
inline Matrix Multiply(const Matrix& a, const Matrix& b) {
    Matrix result {};
    for (size_t row = 0; row < 4; ++row)
        for (size_t col = 0; col < 4; ++col)
            for (size_t k = 0; k < 4; ++k) result[row*4+col] += a[row*4+k]*b[k*4+col];
    return result;
}
inline bool Inverse(const Matrix& input, Matrix& output) {
    auto a = input;
    output = Identity();
    for (size_t col = 0; col < 4; ++col) {
        size_t pivot = col;
        for (size_t row = col + 1; row < 4; ++row)
            if (std::abs(a[row*4+col]) > std::abs(a[pivot*4+col])) pivot = row;
        const double divisor = a[pivot*4+col];
        if (!std::isfinite(divisor) || std::abs(divisor) < 1e-12) return false;
        for (size_t k = 0; k < 4; ++k) {
            std::swap(a[col*4+k], a[pivot*4+k]);
            std::swap(output[col*4+k], output[pivot*4+k]);
        }
        for (size_t k = 0; k < 4; ++k) { a[col*4+k] /= divisor; output[col*4+k] /= divisor; }
        for (size_t row = 0; row < 4; ++row) {
            if (row == col) continue;
            const double factor = a[row*4+col];
            for (size_t k = 0; k < 4; ++k) {
                a[row*4+k] -= factor*a[col*4+k];
                output[row*4+k] -= factor*output[col*4+k];
            }
        }
    }
    for (double x : output) if (!std::isfinite(x)) return false;
    return true;
}
inline bool Build(const Camera& c, Matrix& view, Matrix& projection) {
    if (!std::isfinite(c.near_plane) || c.near_plane <= 0 ||
        !std::isfinite(c.aspect) || c.aspect <= 0 ||
        !std::isfinite(c.fov_y) || c.fov_y <= 0 || c.fov_y >= 3.14159265f ||
        (!c.infinite && (!std::isfinite(c.far_plane) || c.far_plane <= c.near_plane))) return false;
    const std::array<std::array<float,3>,3> basis {c.right, c.up, c.forward};
    for (float x : c.position) if (!std::isfinite(x)) return false;
    for (size_t i = 0; i < 3; ++i) {
        for (size_t j = 0; j < 3; ++j) {
            double dot = 0;
            for (size_t k = 0; k < 3; ++k) {
                if (!std::isfinite(basis[i][k])) return false;
                dot += double(basis[i][k])*basis[j][k];
            }
            if (std::abs(dot - (i == j ? 1.0 : 0.0)) > 0.001) return false;
        }
    }
    const double handedness =
        double(c.right[0])*(double(c.up[1])*c.forward[2]-double(c.up[2])*c.forward[1]) -
        double(c.right[1])*(double(c.up[0])*c.forward[2]-double(c.up[2])*c.forward[0]) +
        double(c.right[2])*(double(c.up[0])*c.forward[1]-double(c.up[1])*c.forward[0]);
    if (std::abs(handedness + 1.0) > 0.001) return false;
    view = Identity();
    for (size_t i = 0; i < 3; ++i) {
        view[i*4] = c.right[i]; view[i*4+1] = c.up[i]; view[i*4+2] = -c.forward[i];
        view[12] -= double(c.position[i])*c.right[i];
        view[13] -= double(c.position[i])*c.up[i];
        view[14] += double(c.position[i])*c.forward[i];
    }
    projection = {};
    projection[5] = 1.0/std::tan(double(c.fov_y)*0.5);
    projection[0] = projection[5]/c.aspect;
    projection[11] = -1.0;
    projection[10] = c.infinite ? 0 : double(c.near_plane)/(double(c.far_plane)-c.near_plane);
    projection[14] = c.infinite ? c.near_plane : double(c.near_plane)*c.far_plane/(double(c.far_plane)-c.near_plane);
    return true;
}

} // namespace Libs::Graphics::Gt7DlssFrameGenCamera
