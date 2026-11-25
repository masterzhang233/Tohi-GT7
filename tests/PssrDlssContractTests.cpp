#include "graphics/guest_gpu/pssr.h"
#include "graphics/host_gpu/renderer/dlss.h"
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
void Need(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
KytyDlssStatus OptimalMock(KytyDlssPssrSession*,uint32_t width,uint32_t height,uint32_t mode,
                           KytyDlssOptimalSettings* settings,KytyDlssError*) {
    if (mode==0) { *settings={sizeof(*settings),width/2,height/2,width/3,height/3,width,height}; return KYTY_DLSS_OK; }
    if (mode==2) { *settings={sizeof(*settings),width*2/3,height*2/3,width/2,height/2,width,height}; return KYTY_DLSS_OK; }
    if (mode==5) { *settings={sizeof(*settings),width,height,width,height,width,height}; return KYTY_DLSS_OK; }
    return KYTY_DLSS_UNSUPPORTED;
}
}
void RunPssrDlssContractTests() {
    using namespace Libs::Graphics;
    Pssr::Request request {};
    request.width=1920; request.height=1080;
    request.depth[0]=request.motion[0]=1;
    request.pre_exposure=1; request.motion_scale[0]=1920; request.motion_scale[1]=1080;
    request.flags=Pssr::kReverseDepth|Pssr::kAutoExposure;
    Need(Dlss::Accepts(request),"native PSSR temporal request rejected");
    auto invalid=request; invalid.flags|=Pssr::kSpatialOnly;
    Need(!Dlss::Accepts(invalid),"spatial-only request entered temporal upscaler");
    invalid=request; invalid.depth[0]=0;
    Need(!Dlss::Accepts(invalid),"missing depth accepted");
    invalid=request; invalid.motion[0]=0;
    Need(!Dlss::Accepts(invalid),"missing motion accepted");
    invalid=request; invalid.pre_exposure=0;
    Need(!Dlss::Accepts(invalid),"zero pre-exposure accepted");
    invalid=request; invalid.jitter[0]=std::numeric_limits<float>::quiet_NaN();
    Need(!Dlss::Accepts(invalid),"NaN jitter accepted");
    invalid=request; invalid.motion_scale[1]=std::numeric_limits<float>::infinity();
    Need(!Dlss::Accepts(invalid),"infinite motion scale accepted");
    KytyDlssPssrApi api {}; api.query_optimal=OptimalMock;
    Dlss::Parameter parameters {&api,reinterpret_cast<KytyDlssPssrSession*>(uintptr_t(1))};
    Dlss::Inputs inputs {}; inputs.output.width=2560; inputs.output.height=1440;
    auto creation=Dlss::Choose(parameters,request,inputs);
    Need(creation && creation->mode==2,"nearest supported quality mode was not selected");
    Need(Dlss::Takes(*creation,request,inputs),"new feature does not accept originating request");
    request.flags|=Pssr::kReset;
    Need(Dlss::Takes(*creation,request,inputs),"history reset incorrectly recreates feature");
    request.flags^=Pssr::kReverseDepth;
    Need(!Dlss::Takes(*creation,request,inputs),"changed depth direction reused wrong feature");
    request.flags=creation->flags;
    request.width=creation->min_width; request.height=creation->min_height;
    Need(Dlss::Takes(*creation,request,inputs),"dynamic minimum rejected");
    --request.width;
    Need(!Dlss::Takes(*creation,request,inputs),"below-range dynamic size accepted");
    request.width=2560; request.height=1440;
    creation=Dlss::Choose(parameters,request,inputs);
    Need(creation && creation->mode==5,"equal resolution did not choose DLAA");
    inputs.output.width=3840;
    Need(!Dlss::Takes(*creation,request,inputs),"output resize reused incompatible feature");
    request.width=1; request.height=1;
    Need(!Dlss::Choose(parameters,request,inputs),"unsupported native size selected a feature");
    std::puts("PSSR DLSS contract: temporal validation, quality choice, dynamic range, reset and resize passed");
}
