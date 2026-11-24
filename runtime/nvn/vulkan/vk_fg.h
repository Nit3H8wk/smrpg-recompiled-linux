// (SMRPG) AMD FSR frame generation (vulkan/vk_fg.cpp): AMD's swapchain in place of device.c's and, on every frame with
// FSR, its preparation with the depth, the motion vectors and the camera (exec.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "vk_fsr.h"

#ifdef __cplusplus
extern "C" {
#endif

// The frame's camera in Unity world space (exec.c gets it from URP's view-projection)
typedef struct {
    float pos[3], up[3], right[3], fwd[3];  // position and axes (unit length)
    float fov_y;                            // vertical field of view, in radians
    float znear, zfar;                      // planes (very large zfar: infinite)
    int ok;
} FgCamera;

// What the recorder thread's packet records on every frame with FSR (exec.c fsr_dispatch, record.c rec_CmdCallback)
typedef struct {
    FsrImage depth, mv;           // the scene's depth and motion vectors (FSR's)
    uint32_t render_w, render_h;  // the drawn part (top left of the images)
    float jitter_x, jitter_y;     // the frame's subpixel jitter, in pixels
    float mv_scale_x, mv_scale_y; // from URP's vectors to pixels toward the previous frame
    bool reset;                   // the history is invalid (cut, size change)
    bool depth_inverted;
    bool paused;                  // the runtime menu is open: no frames are generated
    int version;                  // the requested one: 0 the SDK's (the best for the GPU), 4 or 3
    int pic[4];                   // where the game image is in the window (x, y, width, height)
    FgCamera cam;
    FsrImage hudless;             // the window without the runtime menu (device.c vk_fg_hudless); image 0: without it
} FgPrepare;

// (device.c) the window without the runtime menu (FPS counter, timer), which this frame copies before the menu draws
// over it: with it AMD doesn't interpolate what the menu adds
bool vk_fg_hudless_wanted(void);
FsrImage vk_fg_hudless(void);

bool vk_fg_dll(void);              // amd_fidelityfx_framegeneration_vk.dll (and the FSR ones) next to the executable
uint32_t vk_fg_queue_count(void);  // graphics family queues worth requesting: 3 with the DLL, 1 without it
// (device.c, with the device created) the game queue and the two extra ones for AMD's present and acquire
void vk_fg_queues(VkQueue game, uint32_t family, VkQueue present, VkQueue acquire);
bool vk_fg_usable(void);           // the SDK loaded, the queues there and no earlier failure (device.c checks the setting)
bool vk_fg_active(void);           // the window's swapchain is AMD's
bool vk_fg_generating(void);       // ... and the last prepared frame was generating
bool vk_fg_rebuild_wanted(void);   // the generation context is unusable (another version or a larger scene): device.c
                                   // recreates the swapchain (with the GPU idle the context goes away, and the next
                                   // frame makes another)
                                   // device.c's swapchain calls while generation is wanted. The first creation makes
                                   // AMD's swapchain context; later ones (another size) recreate the swapchain in
                                   // place. On failure: VK_ERROR_* and device.c carries on with its own. destroy only
                                   // notes the object (the next creation recreates it, or vk_fg_release destroys it
                                   // after the contexts).
VkResult vk_fg_create_swapchain(const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc);
void vk_fg_destroy_swapchain(VkSwapchainKHR sc);
VkResult vk_fg_get_images(VkSwapchainKHR sc, uint32_t *n, VkImage *img);
VkResult vk_fg_acquire(VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, uint32_t *idx);
VkResult vk_fg_present(VkQueue queue, const VkPresentInfoKHR *pi);  // (recorder thread)
void vk_fg_prepare_record(VkCommandBuffer cb, const void *prepare);    // (recorder thread) an FgPrepare
void vk_fg_frame_end_record(VkCommandBuffer cb, const void *unused);   // (recorder thread) before the window is submitted
void vk_fg_wait_presents(void);    // AMD's in-flight presents, done (before vkDeviceWaitIdle)
void vk_fg_release(void);          // everything torn down (with the GPU and the recorder idle)
const char *vk_fg_status(void);    // for the menu
// (tests) LARECOMP_FG_PSHOT=N: from frame N on, six consecutive images exactly as presented
// (build/fg_pres_<i>_<real|generado>.png); LARECOMP_FG_MV="x,y" multiplies the motion vector scale; LARECOMP_FG_DEBUG=1
// AMD's marks on the generated frames, =2 its input checker (warnings to the log)

#ifdef __cplusplus
}
#endif
