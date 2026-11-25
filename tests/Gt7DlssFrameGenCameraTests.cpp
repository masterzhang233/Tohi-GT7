#include "graphics/presentation/window/gt7DlssFrameGenCamera.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace Libs::Graphics::Gt7DlssFrameGenCamera;
using Point = std::array<double, 4>;

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
}
bool Close(double a, double b, double tolerance = 1e-9) {
    return std::isfinite(a) && std::isfinite(b) &&
           std::abs(a - b) <= tolerance * std::max({1.0, std::abs(a), std::abs(b)});
}
Point Transform(const Point& p, const Matrix& matrix) {
    Point out {};
    for (size_t col = 0; col < 4; ++col)
        for (size_t row = 0; row < 4; ++row) out[col] += p[row] * matrix[row * 4 + col];
    return out;
}
std::array<double, 3> Ndc(const Point& p) { return {p[0]/p[3], p[1]/p[3], p[2]/p[3]}; }
Camera Ordinary() {
    Camera c {};
    c.right = {1,0,0}; c.up = {0,1,0}; c.forward = {0,0,-1};
    c.near_plane = 0.01f; c.far_plane = 3000; c.fov_y = 0.907571211f; c.aspect = 1920.0f/1080.0f;
    return c;
}
// Independent pinhole geometry oracle: positive distance is along camera
// forward. This does not construct a matrix or use the helper's inverse.
std::array<double, 3> Project(const Camera& c, const Point& point) {
    double right = 0, up = 0, distance = 0;
    for (size_t i = 0; i < 3; ++i) {
        const double delta = point[i] - c.position[i];
        right += delta*c.right[i]; up += delta*c.up[i]; distance += delta*c.forward[i];
    }
    const double half_height = distance*std::tan(double(c.fov_y)*0.5);
    const double depth = c.infinite ? double(c.near_plane)/distance :
        double(c.near_plane)*(double(c.far_plane)-distance)/
        (distance*(double(c.far_plane)-c.near_plane));
    return {right/(half_height*c.aspect), up/half_height, depth};
}
void CheckCamera(const Camera& c, const Point& world) {
    Matrix view {}, projection {};
    Check(Build(c, view, projection), "camera accepted");
    const auto actual = Ndc(Transform(world, Multiply(view, projection)));
    const auto expected = Project(c, world);
    for (size_t i = 0; i < 3; ++i) Check(Close(actual[i], expected[i], 2e-7), "projection agrees with pinhole geometry");
    const auto eye = Transform({c.position[0],c.position[1],c.position[2],1}, view);
    Check(Close(eye[0],0) && Close(eye[1],0) && Close(eye[2],0), "eye transforms to view origin");
}
void CheckReprojection(const Camera& current, const Camera& previous, const Point& world) {
    Matrix cv {}, cp {}, pv {}, pp {}, inverse {};
    Check(Build(current,cv,cp) && Build(previous,pv,pp), "both temporal cameras accepted");
    const auto current_vp = Multiply(cv,cp);
    Check(Inverse(current_vp,inverse), "current VP invertible");
    const auto clip_to_previous = Multiply(inverse,Multiply(pv,pp));
    const auto clip = Transform(world,current_vp);
    const auto reprojection = Ndc(Transform(clip,clip_to_previous));
    const auto previous_expected = Project(previous,world);
    for (size_t i=0;i<3;++i) Check(Close(reprojection[i],previous_expected[i],2e-6), "static world point reprojects to previous camera");
    Matrix previous_to_clip {};
    Check(Inverse(clip_to_previous,previous_to_clip), "temporal transform invertible");
    const auto round_trip=Ndc(Transform(Transform(clip,clip_to_previous),previous_to_clip));
    const auto original=Ndc(clip);
    for (size_t i=0;i<3;++i) Check(Close(round_trip[i],original[i],2e-6), "clip reprojection round trip");
}
}

int main() {
    const auto ordinary=Ordinary();
    Matrix view {}, projection {};
    Check(Build(ordinary,view,projection), "ordinary RH -Z camera accepted");
    Check(view==Identity(), "ordinary pose keeps identity view");
    Check(projection[11]==-1 && projection[8]==0 && projection[9]==0,
          "projection uses negative Z and contains no jitter");
    for (const double distance : {double(ordinary.near_plane),0.1,1.0,10.0,1000.0,double(ordinary.far_plane)})
        CheckCamera(ordinary,{distance*0.1,distance*0.05,-distance,1});
    Check(Close(Ndc(Transform({0,0,-ordinary.near_plane,1},projection))[2],1), "near plane maps to depth one");
    Check(Close(Ndc(Transform({0,0,-ordinary.far_plane,1},projection))[2],0), "far plane maps to depth zero");

    auto infinite=ordinary; infinite.infinite=true; infinite.far_plane=std::numeric_limits<float>::infinity();
    Check(Build(infinite,view,projection), "infinite depth camera accepted");
    for (const double distance : {0.01,1.0,3000.0,1e8}) CheckCamera(infinite,{1,2,-distance,1});
    Check(projection[10]==0, "infinite depth has no finite far-plane term");

    // Analytic screen-space motion: move the camera right and up by one metre.
    // A static point moves left/up-to-down in the rendered image; its backward
    // vector to the previous frame is positive X and negative Y in pixel space.
    auto moved=ordinary; moved.position={1,1,0};
    CheckCamera(moved,{2,3,-20,1});
    CheckReprojection(moved,ordinary,{2,3,-20,1});
    const auto now=Project(moved,{0,0,-10,1}), before=Project(ordinary,{0,0,-10,1});
    const double backward_x=(before[0]-now[0])*1920*0.5;
    const double backward_y=-(before[1]-now[1])*1080*0.5;
    const double expected_pixels=1080/(20*std::tan(double(ordinary.fov_y)*0.5));
    Check(Close(backward_x,expected_pixels,1e-7) && Close(backward_y,-expected_pixels,1e-7),
          "backward render-pixel motion is X-right/Y-down");
    Check(Close(backward_x/1920,(before[0]-now[0])*0.5) &&
          Close(backward_y/1080,-(before[1]-now[1])*0.5), "positive reciprocal dimensions normalize backward motion");

    auto rotated=moved;
    rotated.right={0,0,-1}; rotated.up={0,1,0}; rotated.forward={-1,0,0};
    rotated.fov_y=1.2f; rotated.near_plane=0.2f; rotated.far_plane=5000;
    CheckCamera(rotated,{-20,2,-2,1});
    CheckReprojection(rotated,ordinary,{-20,2,-2,1});
    CheckReprojection(ordinary,rotated,{-20,2,-2,1});
    CheckReprojection(infinite,ordinary,{1,3,-100,1});

    // Independent game capture witness from the accepted BB camera: these
    // are decoded basis/eye fields, not values generated by Build(). Compare
    // against the captured guest view and unjittered projection directly.
    auto captured=ordinary;
    captured.position={266.38427734375f,2.647624969482422f,70.09295654296875f};
    captured.right={0.9840980768203735f,0.17587128281593323f,0.024905405938625336f};
    captured.up={-0.17563261091709137f,0.9843887686729431f,-0.011483397334814072f};
    captured.forward={0.02653620019555092f,-0.006926587782800198f,-0.9996238946914673f};
    Check(Build(captured,view,projection), "recorded GT7 camera accepted");
    const Matrix captured_view {
        0.9840980768203735,-0.17563261091709137,-0.02653620019555092,0,
        0.17587128281593323,0.9843887686729431,0.006926587782800198,0,
        0.024905405938625336,-0.011483397334814072,0.9996238946914673,0,
        -264.35955810546875,44.984375,-63.016109466552734,1};
    for (size_t i=0;i<16;++i) Check(Close(view[i],captured_view[i],2e-6), "reconstructed view matches recorded guest matrix");
    Check(Close(projection[0],1.1532959938049316,1e-7) && Close(projection[5],2.0503039360046387,1e-7),
          "reconstructed projection matches recorded guest focal length");
    CheckCamera(captured,{260,2,-20,1});
    CheckReprojection(captured,ordinary,{260,2,-20,1});

    const auto Reject=[&](auto change,const char* name) { auto bad=ordinary; change(bad); Check(!Build(bad,view,projection),name); };
    Reject([](auto& c){c.forward={0,0,1};}, "opposite handedness rejected");
    Reject([](auto& c){c.up=c.right;}, "nonorthogonal basis rejected");
    Reject([](auto& c){c.right[0]=2;}, "scaled basis rejected");
    Reject([](auto& c){c.position[2]=std::numeric_limits<float>::quiet_NaN();}, "nonfinite position rejected");
    Reject([](auto& c){c.forward[2]=std::numeric_limits<float>::infinity();}, "nonfinite basis rejected");
    Reject([](auto& c){c.near_plane=0;}, "zero near plane rejected");
    Reject([](auto& c){c.far_plane=c.near_plane;}, "coincident depth planes rejected");
    Reject([](auto& c){c.aspect=0;}, "zero aspect rejected");
    Reject([](auto& c){c.fov_y=3.2f;}, "invalid FOV rejected");
    Matrix singular {}, out {};
    Check(!Inverse(singular,out), "singular inverse rejected");
    std::printf("GT7 DLSS-G camera checks: %s\n",failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
