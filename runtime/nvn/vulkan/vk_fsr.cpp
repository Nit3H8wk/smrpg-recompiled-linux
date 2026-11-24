// (SMRPG) AMD FSR as temporal anti-aliasing ("native": at the resolution it is drawn at) with the AMD FSR SDK 2.x. Its
// signed DLLs go next to the executable: amd_fidelityfx_loader_vk.dll, which loads amd_fidelityfx_upscaler_vk.dll and
// picks the version that runs on the GPU (FSR 4, neural network, on RDNA 4; FSR 3.1 on the rest). The headers (MIT) are
// in third_party/amd-fsr.
//
// Inputs (exec.c identifies them on every frame): the scene's HDR color before post-processing, its depth and the
// motion vectors drawn by URP's MotionVectorRenderPass, which the game doesn't ask for and the runtime forces
// (tools/game_overrides.c, GetRenderPassInputs). The scene is drawn with the subpixel jitter from vk_fsr_jitter,
// different on every frame (exec.c set_viewport). The result is written back into the scene color: what follows (URP's
// post-processing, the UI) doesn't change.
//
// Everything that records commands runs on the recorder thread (record.c rec_CmdCallback), at its place in the order;
// the game's images are always in GENERAL, which the SDK's backend takes as its unordered access state.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else  // (Linux): AMD's FSR libraries are Windows DLLs; load them like shared objects where they exist
#include <dlfcn.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef MAX_PATH
#define MAX_PATH 4096
#endif
#endif

#include <vulkan/vulkan.h>

#include "../../../third_party/amd-fsr/api/include/ffx_api.h"
#include "../../../third_party/amd-fsr/upscalers/include/ffx_upscale.h"
#include "../../../third_party/amd-fsr/api/include/vk/ffx_api_vk.h"
#include "vk_fsr.h"
#include "../../ui/tr.h"

static void flog(const char *fmt, ...) {  // (a single write per line, like rt_log)
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n';
    fwrite(b, 1, (size_t)n, stderr);
}

static struct {
    bool tried, ok;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxConfigure configure;
    PfnFfxQuery query;
    PfnFfxDispatch dispatch;
    VkPhysicalDevice phys;
    VkDevice dev;
    PFN_vkGetDeviceProcAddr gdpa;
    char status[160], status_ui[200];  // (English, for the log; in the interface's language, for the menu)
    FsrApi api;  // (vk_fsr_api: the same for vk_fg.cpp)
} A;

static struct {
    ffxContext ctx;
    uint32_t w, h;
    bool inverted;
    int version;  // (the one requested at creation)
    VkImage out;
    VkDeviceMemory out_mem;
    bool out_ready;  // (in GENERAL)
    bool failed;     // (couldn't be created at this size: not retried every frame)
    uint64_t frames;
} C;

extern "C" void vk_queue_lock(void);  // (record.c)
extern "C" void vk_queue_unlock(void);
static VkQueue g_queue;
extern "C" void vk_fsr_set_queue(VkQueue q) { g_queue = q; }

static bool exe_dir(char *path, size_t n) {
#ifdef _WIN32
    DWORD k = GetModuleFileNameA(NULL, path, (DWORD)n);
    if (!k || k >= n) return false;
    while (k && path[k - 1] != '\\' && path[k - 1] != '/') k--;
    path[k] = 0;
    return k != 0;
#else
    char full[MAX_PATH];
    ssize_t k = readlink("/proc/self/exe", full, sizeof full - 1);
    if (k <= 0) return false;
    full[k] = 0;
    char *s = strrchr(full, '/');
    if (!s || (size_t)(s + 1 - full) >= n) return false;
    memcpy(path, full, (size_t)(s + 1 - full));  // up to and including '/'
    return true;
#endif
}

static bool file_exists(const char *p) {
#ifdef _WIN32
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(p, &st) == 0;
#endif
}

static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_status(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    vsnprintf(A.status, sizeof A.status, fmt, ap);
    vsnprintf(A.status_ui, sizeof A.status_ui, tr(fmt), ap2);
    va_end(ap2);
    va_end(ap);
}

extern "C" bool vk_fsr_available(void) {
    static int have = -1;
    if (have < 0) {
        char dir[MAX_PATH], p[MAX_PATH + 64];
        have = 0;
        if (exe_dir(dir, sizeof dir)) {
            snprintf(p, sizeof p, "%samd_fidelityfx_loader_vk.dll", dir);
            bool a = file_exists(p);
            snprintf(p, sizeof p, "%samd_fidelityfx_upscaler_vk.dll", dir);
            bool b = file_exists(p);
            have = a && b;
        }
        if (!have) set_status("not available (the AMD FSR DLLs are missing next to the program)");
    }
    return have;
}

static void msg(uint32_t type, const wchar_t *m) {
    char b[512];
    size_t i = 0;
    for (; m && m[i] && i < sizeof b - 1; i++) b[i] = m[i] < 128 ? (char)m[i] : '?';
    b[i] = 0;
    flog("[fsr] %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", b);
}

extern "C" bool vk_fsr_init(VkPhysicalDevice phys, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa) {
    if (A.tried) return A.ok;
    A.tried = true;
    A.phys = phys, A.dev = dev, A.gdpa = gdpa;
    if (!vk_fsr_available()) return false;
    char dir[MAX_PATH], p[MAX_PATH + 64];
    if (!exe_dir(dir, sizeof dir)) return false;
    snprintf(p, sizeof p, "%samd_fidelityfx_loader_vk.dll", dir);
#ifdef _WIN32
    HMODULE m = LoadLibraryA(p);
#else
    void *m = dlopen(p, RTLD_NOW);
#endif
    if (!m) {
#ifdef _WIN32
        set_status("not available (amd_fidelityfx_loader_vk.dll could not be loaded: %lu)", GetLastError());
#else
        set_status("not available (amd_fidelityfx_loader_vk.dll could not be loaded: %s)", strerror(errno));
#endif
        flog("[fsr] %s", A.status);
        return false;
    }
#ifdef _WIN32
    A.create = (PfnFfxCreateContext)(void *)GetProcAddress(m, "ffxCreateContext");
    A.destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(m, "ffxDestroyContext");
    A.configure = (PfnFfxConfigure)(void *)GetProcAddress(m, "ffxConfigure");
    A.query = (PfnFfxQuery)(void *)GetProcAddress(m, "ffxQuery");
    A.dispatch = (PfnFfxDispatch)(void *)GetProcAddress(m, "ffxDispatch");
#else
    A.create = (PfnFfxCreateContext)dlsym(m, "ffxCreateContext");
    A.destroy = (PfnFfxDestroyContext)dlsym(m, "ffxDestroyContext");
    A.configure = (PfnFfxConfigure)dlsym(m, "ffxConfigure");
    A.query = (PfnFfxQuery)dlsym(m, "ffxQuery");
    A.dispatch = (PfnFfxDispatch)dlsym(m, "ffxDispatch");
#endif
    A.ok = A.create && A.destroy && A.configure && A.query && A.dispatch;
    if (!A.ok) {
        set_status("not available (AMD's FSR loader lacks the expected API)");
        flog("[fsr] %s", A.status);
        return false;
    }
    A.api = {(void *)A.create, (void *)A.destroy, (void *)A.configure, (void *)A.query, (void *)A.dispatch, phys, dev, gdpa};
    // the versions the SDK offers on this GPU (to the log)
    VkDeviceContext dc = {dev, phys, gdpa};
    uint64_t count = 16, ids[16] = {};
    const char *names[16] = {};
    ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = &dc;
    q.outputCount = &count;
    q.versionIds = ids;
    q.versionNames = names;
    char list[400] = "";
    if (A.query(nullptr, &q.header) == FFX_API_RETURN_OK) {
        if (count > 16) count = 16;
        for (uint64_t i = 0; i < count; i++) {
            if (i) strncat(list, ", ", sizeof list - strlen(list) - 1);
            strncat(list, names[i] ? names[i] : "?", sizeof list - strlen(list) - 1);
        }
    }
    flog("[fsr] AMD FSR loaded; upscaler versions on this GPU: %s", count ? list : "(none)");
    set_status("ready (%s)", count && names[0] ? names[0] : "unknown version");
    return true;
}

// Halton (bases 2 and 3) from 1, like FSR's sequence; phases: 8 x (output / input)^2
static float halton(uint64_t i, uint32_t base) {
    float f = 1, r = 0;
    while (i > 0) {
        f /= (float)base;
        r += f * (float)(i % base);
        i /= base;
    }
    return r;
}
extern "C" void vk_fsr_jitter(uint64_t index, uint32_t render_w, uint32_t display_w, float *jx, float *jy) {
    float ratio = render_w ? (float)display_w / (float)render_w : 1;
    uint64_t phases = (uint64_t)ceilf(8.0f * ratio * ratio);
    if (phases < 1) phases = 1;
    uint64_t i = index % phases + 1;
    *jx = halton(i, 2) - 0.5f;
    *jy = halton(i, 3) - 0.5f;
}

// (SMRPG) its video memory (the context, according to AMD, and its output image), for the menu's meter
// (runtime/ui/perf_meter.c); measured when it is created
static uint64_t g_fsr_vram, g_fsr_out_bytes;
extern "C" uint64_t vk_fsr_vram(void) { return g_fsr_vram; }

static void destroy_ctx(void) {
    g_fsr_vram = 0;
    if (C.ctx) A.destroy(&C.ctx, nullptr), C.ctx = nullptr;
    if (C.out) vkDestroyImage(A.dev, C.out, nullptr), C.out = VK_NULL_HANDLE;
    if (C.out_mem) vkFreeMemory(A.dev, C.out_mem, nullptr), C.out_mem = VK_NULL_HANDLE;
    C.out_ready = false;
}

extern "C" void vk_fsr_release(void) {
    if (!A.ok) return;
    destroy_ctx();
    C.w = C.h = 0;
    C.failed = false;
}

static bool make_output(uint32_t w, uint32_t h) {
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(A.dev, &ci, nullptr, &C.out) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(A.dev, C.out, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(A.phys, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(A.dev, &ai, nullptr, &C.out_mem) != VK_SUCCESS) return false;
    g_fsr_out_bytes = mr.size;
    return vkBindImageMemory(A.dev, C.out, C.out_mem, 0) == VK_SUCCESS;
}

extern "C" const FsrApi *vk_fsr_api(void) { return A.ok ? &A.api : nullptr; }

// The SDK version for that context type whose name starts with want (upscaler: 4 -> "4.1.2", 3 -> "3.1.6", 2 ->
// "2.3.4"; frame generation: 4 -> "4.0.2", 3 -> "3.1.6"); false: the SDK's own
extern "C" bool vk_fsr_pick_version(uint64_t create_desc_type, int want, void *override_out) {
    if (!want || !A.ok) return false;
    ffxOverrideVersion *o = (ffxOverrideVersion *)override_out;
    VkDeviceContext dc = {A.dev, A.phys, A.gdpa};
    uint64_t count = 16, ids[16] = {};
    const char *names[16] = {};
    ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = create_desc_type;
    q.device = &dc;
    q.outputCount = &count;
    q.versionIds = ids;
    q.versionNames = names;
    if (A.query(nullptr, &q.header) != FFX_API_RETURN_OK) return false;
    for (uint64_t i = 0; i < count && i < 16; i++)
        if (names[i] && names[i][0] == '0' + want && names[i][1] == '.') {
            *o = {};
            o->header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
            o->versionId = ids[i];
            return true;
        }
    flog("[fsr] version %d is not among the ones the SDK offers: using its own choice", want);
    return false;
}
static bool pick_version(int want, ffxOverrideVersion *o) {
    return vk_fsr_pick_version(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, want, o);
}

static bool ensure_ctx(uint32_t w, uint32_t h, bool inverted, int version) {
    if (C.ctx && C.w == w && C.h == h && C.inverted == inverted && C.version == version) return true;
    if (C.failed && C.w == w && C.h == h && C.inverted == inverted && C.version == version) return false;
    if (C.ctx || C.out) {
        // (its resources may be in earlier submissions, all on the game queue; a size change is rare)
        if (g_queue) {
            vk_queue_lock();
            vkQueueWaitIdle(g_queue);
            vk_queue_unlock();
        } else {
            vkDeviceWaitIdle(A.dev);
        }
        destroy_ctx();
    }
    C.w = w, C.h = h, C.inverted = inverted, C.version = version, C.failed = true;
    ffxCreateBackendVKDesc be = {};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.deviceContext.vkDevice = A.dev;
    be.deviceContext.vkPhysicalDevice = A.phys;
    be.deviceContext.vkDeviceProcAddr = A.gdpa;
    ffxCreateContextDescUpscaleVersion ver = {};  // (mandatory since SDK 2.1: the API it was compiled with)
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.header.pNext = &be.header;
    ver.version = FFX_UPSCALER_VERSION;
    ffxCreateContextDescUpscale d = {};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    d.header.pNext = &ver.header;
    ffxOverrideVersion ov = {};
    if (pick_version(version, &ov)) ov.header.pNext = d.header.pNext, d.header.pNext = &ov.header;
    d.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION |
              (inverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0);
    d.maxRenderSize = {w, h};
    d.maxUpscaleSize = {w, h};
    d.fpMessage = msg;
    ffxReturnCode_t r = A.create(&C.ctx, &d.header, nullptr);
    if (r != FFX_API_RETURN_OK) {
        C.ctx = nullptr;
        set_status("error creating the context at %ux%u (%u)", w, h, (unsigned)r);
        flog("[fsr] %s", A.status);
        return false;
    }
    if (!make_output(w, h)) {
        destroy_ctx();
        set_status("no memory for its %ux%u image", w, h);
        flog("[fsr] %s", A.status);
        return false;
    }
    ffxQueryGetProviderVersion v = {};
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const char *name = A.query(&C.ctx, &v.header) == FFX_API_RETURN_OK && v.versionName ? v.versionName : "?";
    set_status("AMD FSR %s at %ux%u", name, w, h);
    {
        FfxApiEffectMemoryUsage mu = {};
        ffxQueryDescUpscaleGetGPUMemoryUsage q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
        q.gpuMemoryUsageUpscaler = &mu;
        uint64_t ctx = A.query(&C.ctx, &q.header) == FFX_API_RETURN_OK ? mu.totalUsageInBytes : 0;
        g_fsr_vram = ctx + g_fsr_out_bytes;
        flog("[fsr] video memory: %.0f MB (context %.0f MB, output image %.0f MB)", g_fsr_vram / 1048576.0,
             ctx / 1048576.0, g_fsr_out_bytes / 1048576.0);
    }
    flog("[fsr] in use: %s (depth %s)", A.status, inverted ? "reversed" : "normal");
    C.failed = false;
    return true;
}

static FfxApiResource res(const FsrImage *im, uint32_t extra_usage) {
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = im->format;
    ci.extent = {im->width, im->height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = im->usage;
    return ffxApiGetResourceVK((void *)im->image, ffxApiGetImageResourceDescriptionVK(im->image, ci, extra_usage),
                               FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
}

static void barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
}

extern "C" void vk_fsr_record(VkCommandBuffer cb, const void *data) {
    const FsrDispatch *f = (const FsrDispatch *)data;
    if (!A.ok || !f->color.image || !f->depth.image || !f->mv.image) return;
    uint32_t w = f->color.width, h = f->color.height;  // (the images: the drawn part and the output, inside them)
    uint32_t rw = f->render_w ? f->render_w : w, rh = f->render_h ? f->render_h : h;
    uint32_t ow = f->out_w ? f->out_w : rw, oh = f->out_h ? f->out_h : rh;
    if (rw > w || rh > h || ow > w || oh > h) return;
    bool fresh = !C.ctx || C.w != w || C.h != h || C.inverted != f->depth_inverted || C.version != f->version;
    if (!ensure_ctx(w, h, f->depth_inverted, f->version)) return;
    barrier(cb);
    if (!C.out_ready) {  // (the output image, from UNDEFINED to GENERAL once)
        VkImageMemoryBarrier ib = {};
        ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        ib.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = C.out;
        ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &ib);
        C.out_ready = true;
    }
    FsrImage out = {C.out, VK_FORMAT_R16G16B16A16_SFLOAT, w, h,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cb;
    d.color = res(&f->color, 0);
    d.depth = res(&f->depth, 0);
    d.motionVectors = res(&f->mv, 0);
    d.output = res(&out, FFX_API_RESOURCE_USAGE_UAV);
    d.jitterOffset = {f->jitter_x, f->jitter_y};
    d.motionVectorScale = {f->mv_scale_x, f->mv_scale_y};
    d.renderSize = {rw, rh};
    d.upscaleSize = {ow, oh};
    d.enableSharpening = f->sharpness > 0;
    d.sharpness = f->sharpness > 1 ? 1.0f : f->sharpness;
    d.frameTimeDelta = f->frame_ms > 0 ? f->frame_ms : 16.67f;
    d.preExposure = 1.0f;
    d.reset = f->reset || fresh;
    // the frame's camera (exec.c gets it from URP's view-projection); without it, typical Unity values. FSR uses them
    // for its depth thresholds.
    d.cameraNear = f->cam_near > 0 ? f->cam_near : 0.3f;
    d.cameraFar = f->cam_far > d.cameraNear ? f->cam_far : 1000.0f;
    d.cameraFovAngleVertical = f->cam_fov > 0.01f && f->cam_fov < 3.0f ? f->cam_fov : 0.6f;
    d.viewSpaceToMetersFactor = 1.0f;
    ffxReturnCode_t r = A.dispatch(&C.ctx, &d.header);
    if (r != FFX_API_RETURN_OK) {
        static int nlog;
        if (nlog++ < 5) flog("[fsr] ffxDispatch failed (%u)", (unsigned)r);
        return;
    }
    barrier(cb);
    VkImageBlit bl = {};  // the result, back into the scene color
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[1] = {(int32_t)ow, (int32_t)oh, 1};
    bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.dstOffsets[1] = {(int32_t)ow, (int32_t)oh, 1};
    vkCmdBlitImage(cb, C.out, VK_IMAGE_LAYOUT_GENERAL, f->color.image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    barrier(cb);
    static uint32_t lrw, low;
    if (C.frames++ == 0 || rw != lrw || ow != low) flog("[fsr] %ux%u -> %ux%u", rw, rh, ow, oh);
    lrw = rw, low = ow;
}

extern "C" const char *vk_fsr_status(void) {
    if (!A.status[0]) vk_fsr_available();
    return A.status[0] ? A.status_ui : tr("not loaded yet");
}
