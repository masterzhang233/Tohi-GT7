#ifndef KYTY_DLSS_PSSR_H
#define KYTY_DLSS_PSSR_H
#include "kyty_dlss_backend.h"

/* Optional native-resource extension. The v1 GT7 ABI is unchanged. One session
 * exclusively owns NGX initialization; any number of feature histories share
 * its device/runtime. Features must be released after GPU completion before the
 * session. Calls are serialized internally; host lifetime/queue-order rules and
 * the fail-closed record contract from kyty_dlss_backend.h also apply here. */
#ifdef __cplusplus
extern "C" {
#endif
#define KYTY_DLSS_PSSR_ABI_VERSION 1u
#define KYTY_DLSS_PSSR_REVERSE_DEPTH 1u
#define KYTY_DLSS_PSSR_AUTO_EXPOSURE 2u
typedef struct KytyDlssPssrSession KytyDlssPssrSession;
typedef struct KytyDlssPssrFeature KytyDlssPssrFeature;
typedef struct KytyDlssPssrCreation {
    uint32_t struct_size;
    uint32_t output_width, output_height;
    uint32_t flags;
    uint32_t mode; /* NGX quality modes 0..5, queried through query_optimal */
    uint32_t min_width, min_height, max_width, max_height;
} KytyDlssPssrCreation;
typedef struct KytyDlssPssrImage {
    VkImage image;
    VkImageView view;
    VkImageSubresourceRange range; /* exactly one mip/layer; COLOR or DEPTH */
    VkFormat format;
    uint32_t width, height; /* extent of selected mip, not base image */
    /* Host owns lifetime, valid image/view/format/range/usage, sample count 1,
     * and barriers. Inputs GENERAL or shader-read-only, output GENERAL. */
} KytyDlssPssrImage;
typedef struct KytyDlssPssrEvaluateInfo {
    uint32_t struct_size;
    VkCommandBuffer command_buffer;
    KytyDlssPssrImage color, depth, motion, output;
    const KytyDlssPssrImage* exposure; /* optional 1x1 R32/R16 float */
    uint32_t x, y, width, height; /* input subrect in selected view */
    float jitter_x, jitter_y; /* input-pixel sample offset */
    float motion_scale_x, motion_scale_y; /* encoded MV -> render pixels */
    float pre_exposure, exposure_scale, frame_time_ms;
    uint32_t reset_history;
} KytyDlssPssrEvaluateInfo;
typedef struct KytyDlssPssrApi {
    uint32_t struct_size, abi_version;
    /* Uses paths/device/features/extensions from CreateInfo. Dimensions/quality
     * are ignored; feature dimensions belong to each Creation below. */
    KytyDlssStatus (*create_session)(const KytyDlssCreateInfo*, KytyDlssPssrSession**, KytyDlssError*);
    KytyDlssStatus (*query_optimal)(KytyDlssPssrSession*, uint32_t, uint32_t, uint32_t, KytyDlssOptimalSettings*, KytyDlssError*);
    /* CPU-only allocation; never records commands. */
    KytyDlssStatus (*create_feature)(KytyDlssPssrSession*, const KytyDlssPssrCreation*, KytyDlssPssrFeature**, KytyDlssError*);
    KytyDlssStatus (*record_evaluate)(KytyDlssPssrFeature*, const KytyDlssPssrEvaluateInfo*, KytyDlssRecordResult*, KytyDlssError*);
    KytyDlssStatus (*destroy_feature)(KytyDlssPssrFeature*, uint32_t host_gpu_work_completed, KytyDlssError*);
    KytyDlssStatus (*destroy_session)(KytyDlssPssrSession*, uint32_t host_gpu_work_completed, KytyDlssError*);
} KytyDlssPssrApi;
typedef KytyDlssStatus (*PFN_KytyDlssGetPssrApi)(uint32_t, KytyDlssPssrApi*);
#if defined(KYTY_DLSS_BACKEND_BUILD)
__declspec(dllexport)
#endif
KytyDlssStatus KytyDlssGetPssrApi(uint32_t, KytyDlssPssrApi*);
#ifdef __cplusplus
}
#endif
#endif
