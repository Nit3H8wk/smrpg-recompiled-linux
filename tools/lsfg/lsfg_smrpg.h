// (SMRPG) Lossless Scaling Frame Generation for the port: the C API of the separate library (lsfg.dll on Windows,
// liblsfg.so on Android; tools/lsfg/build.sh) that carries lsfg-vk (GameNative's Android branch, MIT), DXVK's DXBC to
// SPIR-V translator (zlib), pe-parse (MIT) and volk (MIT). The port loads it on the fly (runtime/nvn/vulkan/vk_lsfg.c)
// and only talks to it through here. LSFG's shaders come from each player's Lossless.dll (their copy of Lossless
// Scaling): they never ship with the port.
//
// Single-device mode: LSFG works on the port's device and queue (initializeExternal), with the port's images in GENERAL
// layout, and submits its work to that queue: lsfg_smrpg_present has to be called from the thread that submits to the
// queue, in order (after the submit that wrote the input, before the ones that read the outputs). init, create, destroy
// and finalize also submit to that queue (a fallback image, an empty submit with a fence to wait on): with the queue's
// lock if other threads use it.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LSFG_SMRPG_ABI 2  // (lsfg_smrpg_abi: changes if this API changes; 2: cache_path)

typedef struct {
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    uint32_t queue_family;
    VkQueue queue;
    const char *dll_path;  // the player's Lossless.dll
    int performance;       // 1: LSFG 3.1 "performance" (cheaper), 0: the normal one
    float flow_scale;      // Lossless Scaling's "flow scale", 0.25 .. 1
    int generations;       // generated frames per real one (the multiplier - 1)
    int hdr;               // HDR images
    int orig_mipmaps;      // 1: Lossless.dll's "mipmaps" shader; 0: the one lsfg-vk-android ships (default)
    const char *cache_path;  // its pipeline cache on disk (NULL: no cache); created and saved on its own
} LsfgSmrpgInit;

typedef int (*PFN_lsfg_smrpg_abi)(void);
// 0 OK; otherwise, err (up to errlen bytes) says why
typedef int (*PFN_lsfg_smrpg_init)(const LsfgSmrpgInit *in, char *err, int errlen);
// the context (>= 0) for two inputs (in[0], in[1]: the new real image goes into the even / odd-indexed one on each
// frame, taking turns) and n outputs; < 0 if it fails
typedef int (*PFN_lsfg_smrpg_create)(const VkImage in[2], const VkImage *out, int n, uint32_t w, uint32_t h, VkFormat fmt,
                                     char *err, int errlen);
// generates this frame's n images (submits its work to the queue); 0 OK. Anything else (-2: its 2 s wait timed out) is
// a lost context: its frame count did not advance (the parity of the inputs no longer matches) and it may have left an
// unsubmitted fence that the next call would wait on; it has to be deleted and another one created
typedef int (*PFN_lsfg_smrpg_present)(int ctx, char *err, int errlen);
typedef void (*PFN_lsfg_smrpg_destroy)(int ctx);
typedef void (*PFN_lsfg_smrpg_finalize)(void);

#ifdef __cplusplus
}
#endif
