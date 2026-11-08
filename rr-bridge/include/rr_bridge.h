/* The ABI of the NGX bridge: the C seam between the RayReconstruction Mod and the DLL that links the
 * NGX SDK, whose license keeps its headers and libraries out of the MIT-licensed dinput8.dll.
 *
 * The Mod loads the bridge with LoadLibrary and an absolute path in reframework/rr/, finds
 * RR_BRIDGE_ENTRY_NAME with GetProcAddress and links nothing of it. The entry returns the table of
 * operations.
 *
 * Frozen in every version: the entry's name and signature, and abi_version as the first member of the
 * table, so that the Mod can read it before trusting the rest. Any other change bumps
 * RR_BRIDGE_ABI_VERSION.
 *
 * Plain C, without windows.h or d3d12.h: the D3D12 objects go as opaque pointers. */
#ifndef RR_BRIDGE_H
#define RR_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 2: the bridge copies the strings of init, never runs concurrently, and a creation that fails leaves the
 * Mod's list unexecuted.
 * 3: evaluate records RR, and its parameters carry no camera matrices. */
#define RR_BRIDGE_ABI_VERSION 3u

#define RR_BRIDGE_ENTRY_NAME "rr_bridge_get_api"

/* The name the build gives the bridge (target rr_bridge in cmake.toml). */
#define RR_BRIDGE_FILE_NAME "rr_bridge.dll"

/* The installed snippet of RR, which has to be in the bridge's folder. NGX looks for it there first, but
 * may load another RR snippet, like the one of its OTA cache while the NVIDIA App override of RR is on. */
#define RR_BRIDGE_SNIPPET_FILE_NAME "nvngx_dlssd.dll"

/* rr_bridge_api::flags */
#define RR_BRIDGE_FLAG_NGX_SDK 0x1u /* Built with the NGX SDK. Without it, every operation fails with RR_BRIDGE_NO_SDK. */

typedef int32_t rr_bridge_result;

enum rr_bridge_result_code {
    RR_BRIDGE_OK = 0,
    RR_BRIDGE_NO_SDK = 1,             /* built without the NGX SDK */
    RR_BRIDGE_UNSUPPORTED = 2,        /* no RTX GPU, or the driver does not support RR */
    RR_BRIDGE_NO_SNIPPET = 3,         /* nvngx_dlssd.dll is not in reframework/rr/ */
    RR_BRIDGE_OTHER_NGX_CONSUMER = 4, /* NGX or an NGX snippet was already loaded in the process */
    RR_BRIDGE_ERROR = 5,              /* any other failure: see last_error */
};

/* The bridge keeps its own copies of the strings. */
typedef struct rr_bridge_init_params {
    void* device;              /* ID3D12Device* */
    const wchar_t* snippet_dir; /* absolute, reframework/rr/: the first entry of PathListInfo */
    const wchar_t* log_dir;     /* absolute, reframework/rr/ngx/ */
} rr_bridge_init_params;

/* The RR feature, owned by the bridge. */
typedef struct rr_bridge_feature rr_bridge_feature;

typedef struct rr_bridge_feature_params {
    void* command_list; /* ID3D12GraphicsCommandList*: the Mod's own, open, which the Mod executes and fences;
                           if the creation fails, or the Mod rejects the feature, it discards the list unexecuted */
    uint32_t width;     /* the render size, which is also the output size (DLAA) */
    uint32_t height;
} rr_bridge_feature_params;

/* The resources are ID3D12Resource*. The inputs arrive in NON_PIXEL_SHADER_RESOURCE and the output in
 * UNORDERED_ACCESS; after the call, the Mod restores the state of the command list. T arrives without
 * exposure, so the bridge passes InPreExposure and InExposureScale at 1 and no exposure texture.
 *
 * No camera matrices. The DLSS-RR Integration Guide asks for pInWorldToViewMatrix and pInViewToClipMatrix
 * only together with pInSpecularHitDistance, the alternative to specular motion vectors (sections 3.4.8 and
 * 3.4.9, p. 17), and RE2 gives neither. In the SDK 310.9.16 they are float* fields of the DLSSD evaluation
 * (nvsdk_ngx_helpers_dlssd_d3d.h:498-499) that go to NGX as void* (:700-701), and the headers give no
 * convention. Should they ever be needed, Streamline says they are row major and without jitter
 * (sl_consts.h:183-185 in v2.14.16) and hands them to NGX unconverted (sl.dlss_d/dlss_dEntry.cpp:1045-1046). */
typedef struct rr_bridge_eval_params {
    void* command_list; /* ID3D12GraphicsCommandList*: the game's, at the TAA draw */
    void* color;        /* T */
    void* output;       /* O */
    void* depth;
    void* motion_vectors;
    void* normals_roughness; /* the roughness in .w: the feature reads it packed */
    void* diffuse_albedo;
    void* specular_albedo;
    uint32_t width; /* the render size, the feature's */
    uint32_t height;
    float jitter[2];   /* InJitterOffset, in render pixels */
    float mv_scale[2]; /* MVScale */
    uint32_t reset;    /* InReset: the reset of the history */
} rr_bridge_eval_params;

/* Called only from the render thread, never concurrently: the Mod serializes every call. The evaluation
 * goes from the TAA draw, and the creation of the feature on the Mod's own command list, at Present. */
typedef struct rr_bridge_api {
    uint32_t abi_version; /* the first member in every version */
    uint32_t flags;       /* RR_BRIDGE_FLAG_* */

    rr_bridge_result (*init)(const rr_bridge_init_params* params);

    /* GPU, driver and snippet. Only after init. */
    rr_bridge_result (*query_availability)(void);

    rr_bridge_result (*create_feature)(const rr_bridge_feature_params* params, rr_bridge_feature** feature);
    void (*release_feature)(rr_bridge_feature* feature);

    rr_bridge_result (*evaluate)(rr_bridge_feature* feature, const rr_bridge_eval_params* params);

    /* The last error as text; never null. */
    const char* (*last_error)(void);
} rr_bridge_api;

typedef const rr_bridge_api* (*rr_bridge_get_api_fn)(void);

#if defined(RR_BRIDGE_BUILD) && defined(_WIN32)
#define RR_BRIDGE_EXPORT __declspec(dllexport)
#elif defined(RR_BRIDGE_BUILD)
#define RR_BRIDGE_EXPORT __attribute__((visibility("default")))
#else
#define RR_BRIDGE_EXPORT
#endif

/* The entry, RR_BRIDGE_ENTRY_NAME. */
RR_BRIDGE_EXPORT const rr_bridge_api* rr_bridge_get_api(void);

#ifdef __cplusplus
}
#endif

#endif
