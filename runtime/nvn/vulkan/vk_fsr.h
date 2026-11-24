// (SMRPG) AMD FSR as temporal anti-aliasing: vulkan/vk_fsr.cpp (the SDK) and exec.c (where and with what it is called).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    VkImage image;
    VkFormat format;
    uint32_t width, height;
    VkImageUsageFlags usage;
} FsrImage;

// What a recorder thread packet records (record.c rec_CmdCallback): copied as is into the packet.
typedef struct {
    FsrImage color;  // the scene's HDR color (the result is written back into it)
    FsrImage depth;  // its depth
    FsrImage mv;     // URP's motion vectors (RG16F, in UV)
    float jitter_x, jitter_y;      // this frame's subpixel jitter, in image pixels
    float mv_scale_x, mv_scale_y;  // from URP's vectors to pixels toward the previous frame
    float frame_ms;
    float sharpness;  // 0: without FSR's own sharpening; 0..1
    bool reset;       // the history is invalid (camera cut, size change)
    bool depth_inverted;
    uint32_t render_w, render_h;  // the drawn part (top left of the images)
    uint32_t out_w, out_h;        // the size FSR upscales it to (the same: native anti-aliasing)
    int version;  // the requested version: 0 whichever the SDK picks (the best for the GPU), 4, 3 (3.1) or 2 (2.3)
    float cam_near, cam_far, cam_fov;  // the frame's camera (exec.c); 0: typical Unity values
} FsrDispatch;

// (vk_fg.cpp) the SDK vk_fsr_init loaded: its functions (the Pfn* from ffx_api.h) and the device; NULL if not loaded
typedef struct {
    void *create, *destroy, *configure, *query, *dispatch;
    VkPhysicalDevice phys;
    VkDevice dev;
    PFN_vkGetDeviceProcAddr gdpa;
} FsrApi;
const FsrApi *vk_fsr_api(void);
// The SDK version for that context type whose name starts with "want." (4 -> "4.0.2"): an ffxOverrideVersion in
// override_out; false: whichever the SDK picks
bool vk_fsr_pick_version(uint64_t create_desc_type, int want, void *override_out);

bool vk_fsr_available(void);  // the SDK's DLLs next to the executable
// (translating thread, once, with the device created) loads the DLLs; false if they are missing or unusable
bool vk_fsr_init(VkPhysicalDevice phys, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa);
// subpixel jitter number `index` of FSR's sequence (Halton 2, 3), in pixels (-0.5..0.5)
void vk_fsr_jitter(uint64_t index, uint32_t render_w, uint32_t display_w, float *jx, float *jy);
void vk_fsr_record(VkCommandBuffer cb, const void *dispatch);  // (recorder thread) an FsrDispatch
void vk_fsr_release(void);  // (with the GPU idle) the context and its image
// (device.c) the game queue: a new context waits for it alone (not vkDeviceWaitIdle: with frame generation, AMD's
// present queue is used by its own threads)
void vk_fsr_set_queue(VkQueue q);
const char *vk_fsr_status(void);  // for the menu: version in use or why not

#ifdef __cplusplus
}
#endif
