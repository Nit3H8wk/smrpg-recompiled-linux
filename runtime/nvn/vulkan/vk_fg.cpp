// (SMRPG) AMD FSR frame generation (SDK 2.4, amd_fidelityfx_framegeneration_vk.dll next to the executable): FSR Frame
// Generation 4.0.2 (neural network) on the Radeon RX 9000, 3.1.6 on the rest. For every frame the game presents, AMD's
// swapchain first presents one generated between it and the previous one, on its own queue and with its own pacing
// threads: on screen, twice the frames per second.
//
// Adapted from our own earlier integration of AMD's SDK 2 (the one contributed to NX2navi, vk_fg.cpp). How it fits in
// here:
// - device.c creates two more queues of the graphics family (vk_fg_queue_count, vk_fg_queues) and, while generation is
//   wanted (the setting, with AMD FSR as anti-aliasing), makes the swapchain with vk_fg_create_swapchain and acquires
//   with vk_fg_acquire. The game queue belongs to the recorder thread (record.c): it presents with vk_fg_present, and
//   AMD submits its work (the generation) to it inside that same call; submit_game does so under the queue lock just in
//   case.
// - The order FG 4 requires (configure, prepare, generate) is the recorder thread's: exec.c, after FSR, queues
//   vk_fg_prepare_record (configures the frame and records the preparation with FSR's depth and motion vectors and the
//   camera); before the window is submitted, vk_fg_frame_end_record configures the frames that didn't reach FSR
//   (screens without the motion pass) without generation; the present generates.
// - The VkSwapchainKHR object is AMD's: another size recreates it in place; it is only destroyed after its contexts.
//   Before the generation context goes away or the swapchain is recreated, generation is turned off on it (AMD's
//   order). An in-place recreation keeps its counts of acquired and presented images; if they don't match (one acquired
//   but not presented), on another format or on a failed recreation, everything is remade.
// - The game's UI is inside the image and is interpolated with it (in SMRPG it hardly moves); the runtime menu pauses
//   generation.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else  // (Linux): frame generation loads AMD's DLLs through vk_fsr.cpp; here we only check they are next to the program
#include <sys/stat.h>
#include <unistd.h>
#ifndef MAX_PATH
#define MAX_PATH 4096
#endif
#endif

#include <atomic>
#include <chrono>

#include <vulkan/vulkan.h>

#include "../../../third_party/amd-fsr/api/include/ffx_api.h"
#include "../../../third_party/amd-fsr/api/include/vk/ffx_api_vk.h"
#include "../../../third_party/amd-fsr/framegeneration/include/ffx_framegeneration.h"
#include "../../../third_party/amd-fsr/framegeneration/include/vk/ffx_api_framegeneration_vk.h"
#include "vk_fg.h"
#include "../../ui/tr.h"

extern "C" void vk_queue_lock(void);    // (record.c) the game queue
extern "C" void vk_queue_unlock(void);
extern "C" void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);  // (device.c)

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

namespace {

struct Shot {  // (tests, LARECOMP_FG_PSHOT) a presented image, copied to a CPU-visible buffer
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void *ptr = nullptr;
    uint32_t w = 0, h = 0;
    bool bgr = false;
};

struct {
    VkQueue game = VK_NULL_HANDLE, present = VK_NULL_HANDLE, acquire = VK_NULL_HANDLE;
    uint32_t family = 0;
    ffxContext sc = nullptr;  // the swapchain context
    ffxContext fg = nullptr;  // the generation context
    ffxQueryDescSwapchainReplacementFunctionsVK fn = {};
    // AMD's swapchain object (cur), the one device.c released (pending: the next creation recreates it in place or
    // vk_fg_release destroys it), the format and color space it was made with, its size
    VkSwapchainKHR cur = VK_NULL_HANDLE, pending = VK_NULL_HANDLE;
    VkFormat sc_fmt = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR sc_cs = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D sc_ext = {0, 0};
    // the generation context: which display, format, maximum scene size, version and depth it was made for
    VkExtent2D fg_ext = {0, 0}, fg_max = {0, 0};
    VkFormat fg_fmt = VK_FORMAT_UNDEFINED;
    int fg_version = -1;
    bool fg_inverted = false, fg4 = false;
    std::atomic<int> rebuild{0};  // (recorder → device.c) the generation context is unusable: recreate the swapchain
    // images acquired (translating thread) and presented (recorder) since the object was made: equal when none is
    // waiting for its present (compared with the recorder idle)
    std::atomic<uint64_t> acquired{0}, presented{0};
    // (recorder) the number of the last configured frame and at which present; presents made
    uint64_t frame = 0, configured_at = ~0ull, presents = 0;
    std::atomic<int> on{0};
    int failed = 0;
    FgCamera cam = {};  // the last good camera (a frame without its own uses this one)
    uint64_t generated = 0, prepared = 0, paused = 0, no_inputs = 0, no_camera = 0, hudless = 0;
    std::chrono::steady_clock::time_point last_prepare;
    char status[200] = "";
} F;

const FsrApi *api(void) { return vk_fsr_api(); }
// (SMRPG) frame generation's video memory (its context and its swapchain, according to AMD; measured when they are
// created): for the menu's meter, runtime/ui/perf_meter.c
static uint64_t g_fg_vram_ctx, g_fg_vram_sc;
extern "C" uint64_t vk_fg_vram(void) { return g_fg_vram_ctx + g_fg_vram_sc; }

ffxReturnCode_t api_create(ffxContext *c, ffxCreateContextDescHeader *d) {
    return ((PfnFfxCreateContext)api()->create)(c, d, nullptr);
}
void api_destroy(ffxContext *c) {
    if (c && *c) ((PfnFfxDestroyContext)api()->destroy)(c, nullptr), *c = nullptr;
}
ffxReturnCode_t api_configure(ffxContext *c, ffxConfigureDescHeader *d) { return ((PfnFfxConfigure)api()->configure)(c, d); }
ffxReturnCode_t api_query(ffxContext *c, ffxQueryDescHeader *d) { return ((PfnFfxQuery)api()->query)(c, d); }
ffxReturnCode_t api_dispatch(ffxContext *c, ffxDispatchDescHeader *d) { return ((PfnFfxDispatch)api()->dispatch)(c, d); }

const char *provider(ffxContext *c) {
    ffxQueryGetProviderVersion v = {};
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    return c && *c && api_query(c, &v.header) == FFX_API_RETURN_OK && v.versionName ? v.versionName : "?";
}

// The game queue belongs to the recorder thread; AMD submits to it from the present, which runs on that thread. The
// lock is in case some version did so from another (record.c takes it too, for its own submissions).
VkResult submit_game(uint32_t n, const VkSubmitInfo *si, VkFence fence) {
    vk_queue_lock();
    VkResult r = vkQueueSubmit(F.game, n, si, fence);
    vk_queue_unlock();
    return r;
}

VkImageLayout layout_of(uint32_t state) {
    if (state & FFX_API_RESOURCE_STATE_PRESENT) return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    if (state & FFX_API_RESOURCE_STATE_RENDER_TARGET) return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (state == FFX_API_RESOURCE_STATE_COPY_SRC) return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (state == FFX_API_RESOURCE_STATE_COPY_DEST) return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (state & (FFX_API_RESOURCE_STATE_UNORDERED_ACCESS | FFX_API_RESOURCE_STATE_COMMON)) return VK_IMAGE_LAYOUT_GENERAL;
    if (state & FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ) return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return VK_IMAGE_LAYOUT_GENERAL;
}

void layout_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// (tests) copies an AMD image (in the state it says) to a CPU-visible buffer, leaving it as it was
bool shot_copy(VkCommandBuffer cb, const FfxApiResource &r, Shot &s) {
    VkImage img = (VkImage)r.resource;
    uint32_t w = r.description.width, h = r.description.height;
    uint32_t f = r.description.format;
    if (!img || !w || !h) return false;
    if (f != FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM && f != FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM &&
        f != FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB && f != FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB &&
        f != FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM)
        return false;
    VkDevice dev = api()->dev;
    if (!s.buf) {
        VkBufferCreateInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = (VkDeviceSize)w * h * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(dev, &bi, nullptr, &s.buf) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev, s.buf, &mr);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(api()->phys, &mp);
        uint32_t type = UINT32_MAX, want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
            if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) type = i;
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = type;
        if (type == UINT32_MAX || vkAllocateMemory(dev, &ai, nullptr, &s.mem) != VK_SUCCESS ||
            vkBindBufferMemory(dev, s.buf, s.mem, 0) != VK_SUCCESS || vkMapMemory(dev, s.mem, 0, VK_WHOLE_SIZE, 0, &s.ptr) != VK_SUCCESS)
            return false;
        s.w = w, s.h = h;
    }
    if (s.w != w || s.h != h) return false;
    s.bgr = f == FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM || f == FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB;
    VkImageLayout l = layout_of(r.state);
    layout_barrier(cb, img, l, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rg = {};
    rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buf, 1, &rg);
    layout_barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, l);
    return true;
}

void shot_save(const Shot &s, const char *path) {
    if (!s.ptr) return;
    size_t n = (size_t)s.w * s.h;
    uint8_t *px = (uint8_t *)malloc(n * 4);
    if (!px) return;
    memcpy(px, s.ptr, n * 4);
    for (size_t i = 0; i < n; i++) {
        if (s.bgr) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
        px[i * 4 + 3] = 255;
    }
    vk_save_png(path, px, s.w, s.h);
    free(px);
}

// AMD's callback to generate (in the present, recorder thread): records the generation into the list it provides
ffxReturnCode_t generate(ffxDispatchDescFrameGeneration *d, void *user) {
    ffxContext *ctx = (ffxContext *)user;
    if (!ctx || !*ctx) {  // no context (any more): nothing generated, the swapchain doesn't present a stale image
        d->numGeneratedFrames = 0;
        return FFX_API_RETURN_ERROR;
    }
    F.generated++;
    ffxReturnCode_t r = api_dispatch(ctx, &d->header);
    if (r != FFX_API_RETURN_OK) {
        d->numGeneratedFrames = 0;
        return r;
    }
    return r;
}

// (tests, LARECOMP_FG_PSHOT=N) AMD's composition done here, like its own without a callback (the image, real or
// generated, copied to the real swapchain's), and from frame N on, six consecutive images exactly as presented
// (build/fg_pres_<i>_<real|generado>.png). Runs on AMD's present thread, with its queue.
struct {
    long at = -1;
    int n = 0, logged = 0;
    bool gen[6] = {};
    uint64_t saved_at = 0;
    bool saved = false;
    Shot img[6];
} P;

ffxReturnCode_t present_cb(ffxCallbackDescFrameGenerationPresent *p, void *) {
    VkCommandBuffer cb = (VkCommandBuffer)p->commandList;
    const FfxApiResource &src = p->currentBackBuffer, &dst = p->outputSwapChainBuffer;
    VkImage si = (VkImage)src.resource, di = (VkImage)dst.resource;
    if (P.logged < 2) {
        P.logged++;
        flog("[fg] composition: image %ux%u (format %u, state %u) -> output %ux%u (format %u, state %u), %s",
             src.description.width, src.description.height, src.description.format, src.state, dst.description.width,
             dst.description.height, dst.description.format, dst.state, p->isGeneratedFrame ? "generated" : "real");
    }
    if (!cb || !si || !di) return FFX_API_RETURN_ERROR;
    if (P.n < 6 && (long)F.presents + 1 >= P.at && shot_copy(cb, src, P.img[P.n])) {
        P.gen[P.n] = p->isGeneratedFrame;
        if (++P.n == 6) P.saved_at = F.presents + 8;
    }
    VkImageLayout sl = layout_of(src.state), dl = layout_of(dst.state);
    layout_barrier(cb, si, sl, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    layout_barrier(cb, di, dl, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy rg = {};
    rg.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.extent = {src.description.width < dst.description.width ? src.description.width : dst.description.width,
                 src.description.height < dst.description.height ? src.description.height : dst.description.height, 1};
    vkCmdCopyImage(cb, si, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, di, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    layout_barrier(cb, si, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sl);
    layout_barrier(cb, di, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dl);
    return FFX_API_RETURN_OK;
}

// Generation off on AMD's swapchain swc (no callbacks or images): its presenter waits for the presents in flight and
// stops; the next configuration with generation starts it again.
void fg_disable(VkSwapchainKHR swc) {
    F.on.store(0, std::memory_order_relaxed);
    if (!F.fg || !swc) return;
    ffxConfigureDescFrameGeneration cfg = {};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = (void *)swc;
    cfg.frameGenerationEnabled = false;
    cfg.frameID = ++F.frame;
    api_configure(&F.fg, &cfg.header);
}

void destroy_fg(void) {
    if (!F.fg) return;
    fg_disable(F.cur);
    api_destroy(&F.fg);
    g_fg_vram_ctx = 0;
    F.fg_ext = F.fg_max = {0, 0};
    F.fg_version = -1;
}

// Everything torn down in AMD's order: generation off, its context, the swapchain's and AMD's swapchain object (the one
// device.c released or the current one)
void teardown(void) {
    VkSwapchainKHR obj = F.pending ? F.pending : F.cur;
    if (api()) {
        fg_disable(obj);
        api_destroy(&F.fg);
        api_destroy(&F.sc);
        if (obj && F.fn.pOutDestroySwapchainFFXAPI) F.fn.pOutDestroySwapchainFFXAPI(api()->dev, obj, nullptr, nullptr);
    }
    F.fg = F.sc = nullptr;
    g_fg_vram_ctx = g_fg_vram_sc = 0;
    F.pending = F.cur = VK_NULL_HANDLE;
    F.fg_ext = F.fg_max = {0, 0};
    F.fg_version = -1;
    F.acquired = 0, F.presented = 0;
    F.rebuild = 0;
}

void fail(const char *what, unsigned r) {
    flog("[fg] %s failed (%u): no frame generation until the game restarts", what, r);
    snprintf(F.status, sizeof F.status, tr("error (%s: %u); off until the game restarts"), tr(what), r);
    F.failed = 1;
    // turned off on AMD's swapchain right away; presents keep going through it (vk_fg_active) until device.c swaps it
    // for a normal one (vk_fg_usable is false after a failure)
    fg_disable(F.cur);
}

bool exe_file(const char *name) {
    char p[MAX_PATH + 64];
#ifdef _WIN32
    DWORD k = GetModuleFileNameA(NULL, p, MAX_PATH);
    if (!k || k >= MAX_PATH) return false;
    while (k && p[k - 1] != '\\' && p[k - 1] != '/') k--;
#else
    ssize_t k = readlink("/proc/self/exe", p, MAX_PATH);
    if (k <= 0 || k >= MAX_PATH) return false;
    while (k > 0 && p[k - 1] != '/') k--;
#endif
    snprintf(p + k, sizeof p - k, "%s", name);
#ifdef _WIN32
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(p, &st) == 0;
#endif
}

FfxApiResource res(const FsrImage *im) {
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = im->format;
    ci.extent = {im->width, im->height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = im->usage;
    return ffxApiGetResourceVK((void *)im->image, ffxApiGetImageResourceDescriptionVK(im->image, ci, 0),
                               FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
}

void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

bool hdr_format(VkFormat f) {
    return f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f == VK_FORMAT_A2R10G10B10_UNORM_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT;
}

void amd_message(uint32_t type, const wchar_t *m) {  // (the SDK's messages, to the log; the first 200)
    static int n;
    if (n++ >= 200) return;
    char b[600];
    size_t i = 0;
    for (; m && m[i] && i < sizeof b - 1; i++) b[i] = m[i] < 128 ? (char)m[i] : '?';
    b[i] = 0;
    flog("[fg] AMD %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", b);
}

// The generation context for the swapchain's display and scenes of up to max_render
bool create_fg(VkExtent2D max_render, int version, bool inverted) {
    static bool listed;
    if (!listed) {  // the versions the SDK offers on this GPU (to the log, once)
        listed = true;
        VkDeviceContext dc = {api()->dev, api()->phys, api()->gdpa};
        uint64_t count = 16, ids[16] = {};
        const char *names[16] = {};
        ffxQueryDescGetVersions q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        q.device = &dc;
        q.outputCount = &count;
        q.versionIds = ids;
        q.versionNames = names;
        char list[300] = "";
        if (api_query(nullptr, &q.header) == FFX_API_RETURN_OK)
            for (uint64_t i = 0; i < count && i < 16; i++) {
                if (i) strncat(list, ", ", sizeof list - strlen(list) - 1);
                strncat(list, names[i] ? names[i] : "?", sizeof list - strlen(list) - 1);
            }
        flog("[fg] frame generation versions on this GPU: %s", list[0] ? list : "(none)");
    }
    ffxCreateBackendVKDesc be = {};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.deviceContext.vkDevice = api()->dev;
    be.deviceContext.vkPhysicalDevice = api()->phys;
    be.deviceContext.vkDeviceProcAddr = api()->gdpa;
    ffxCreateContextDescFrameGenerationVersion ver = {};  // (mandatory since SDK 2.1)
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    ver.header.pNext = &be.header;
    ver.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateContextDescFrameGeneration d = {};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    d.header.pNext = &ver.header;
    ffxOverrideVersion ov = {};
    if (vk_fsr_pick_version(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, version, &ov))
        ov.header.pNext = d.header.pNext, d.header.pNext = &ov.header;
    d.flags = (inverted ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0) |
              (hdr_format(F.sc_fmt) ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0);
    static int dbg = -1;  // (tests) LARECOMP_FG_DEBUG=2: AMD's input checker, with its warnings to the log
    if (dbg < 0) dbg = getenv("LARECOMP_FG_DEBUG") ? atoi(getenv("LARECOMP_FG_DEBUG")) : 0;
    if (dbg >= 2) d.flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
    d.displaySize = {F.sc_ext.width, F.sc_ext.height};
    d.maxRenderSize = {max_render.width, max_render.height};
    d.backBufferFormat = ffxApiGetSurfaceFormatVK(F.sc_fmt);
    ffxReturnCode_t r = api_create(&F.fg, &d.header);
    if (r != FFX_API_RETURN_OK) {
        F.fg = nullptr;
        fail("the generation context", r);
        return false;
    }
    F.fg_ext = F.sc_ext, F.fg_fmt = F.sc_fmt, F.fg_max = max_render, F.fg_version = version, F.fg_inverted = inverted;
    {   // (SMRPG) its video memory, according to AMD, for the menu's meter (runtime/ui/perf_meter.c)
        FfxApiEffectMemoryUsage mu = {};
        ffxQueryDescFrameGenerationGetGPUMemoryUsage q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE;
        q.gpuMemoryUsageFrameGeneration = &mu;
        g_fg_vram_ctx = api_query(&F.fg, &q.header) == FFX_API_RETURN_OK ? mu.totalUsageInBytes : 0;
    }
    if (dbg >= 2) {
        ffxConfigureDescGlobalDebug gd = {};
        gd.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
        gd.effectId = FFX_API_EFFECT_ID_FRAMEGENERATION;
        gd.fpMessage = amd_message;
        gd.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE;
        ffxReturnCode_t gr = api_configure(&F.fg, &gd.header);
        flog("[fg] AMD input checker: %s (%u)", gr == FFX_API_RETURN_OK ? "on" : "could not be turned on", (unsigned)gr);
    }
    const char *pv = provider(&F.fg);
    F.fg4 = pv[0] == '4';
    snprintf(F.status, sizeof F.status, tr("active (FSR Frame Generation %s)"), pv);
    flog("[fg] generation context %s for %ux%u%s, scene up to %ux%u, depth %s", pv, F.sc_ext.width,
         F.sc_ext.height, hdr_format(F.sc_fmt) ? " (HDR)" : "", max_render.width, max_render.height,
         inverted ? "reversed" : "normal");
    return true;
}

}  // namespace

extern "C" bool vk_fg_dll(void) {
    static int have = -1;
    if (have < 0) {
        have = vk_fsr_available() && exe_file("amd_fidelityfx_framegeneration_vk.dll");
        if (!have)
            snprintf(F.status, sizeof F.status, "%s",
                     tr("not available (amd_fidelityfx_framegeneration_vk.dll is missing next to the program)"));
    }
    return have;
}

// With the DLL, always all three queues: the setting can be turned on while playing and an idle queue costs nothing
extern "C" uint32_t vk_fg_queue_count(void) { return vk_fg_dll() ? 3 : 1; }

extern "C" void vk_fg_queues(VkQueue game, uint32_t family, VkQueue present, VkQueue acquire) {
    F.game = game, F.family = family, F.present = present, F.acquire = acquire;
    if (vk_fg_dll() && !present)
        snprintf(F.status, sizeof F.status, "%s", tr("not available (the GPU has no second graphics queue)"));
}

extern "C" bool vk_fg_usable(void) { return !F.failed && F.present && F.acquire && vk_fg_dll() && api(); }

// AMD's swapchain is in place: the window's VkSwapchainKHR is its object, and every swapchain call has to go through
// its functions (also after a failure, until device.c swaps it: Vulkan's must never see it)
extern "C" bool vk_fg_active(void) { return F.sc != nullptr; }

extern "C" bool vk_fg_generating(void) { return vk_fg_active() && !F.failed && F.on.load(std::memory_order_relaxed); }

extern "C" bool vk_fg_rebuild_wanted(void) { return vk_fg_active() && F.rebuild.load(std::memory_order_relaxed) != 0; }

extern "C" VkResult vk_fg_create_swapchain(const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc) {
    if (F.sc) {  // the same AMD swapchain, made again for the new size or mode
        VkSwapchainKHR obj = F.pending ? F.pending : F.cur;
        F.pending = VK_NULL_HANDLE, F.cur = obj;
        bool same = ci->imageFormat == F.sc_fmt && ci->imageColorSpace == F.sc_cs;
        if (obj && same && F.acquired == F.presented && !F.failed) {
            fg_disable(obj);  // (the next frame turns it back on)
            // the generation context is for the previous display, or unusable (rebuild): out, with the GPU idle
            // (device.c recreate_swapchain waited); the next frame with FSR makes another
            if (F.fg && (F.rebuild || F.fg_ext.width != ci->imageExtent.width || F.fg_ext.height != ci->imageExtent.height))
                destroy_fg();
            F.rebuild = 0;
            VkSwapchainKHR out = obj;
            VkResult r = F.fn.pOutCreateSwapchainFFXAPI(api()->dev, ci, nullptr, &out, (void *)obj);
            if (r == VK_SUCCESS) {
                F.cur = out, *sc = out;
                F.sc_ext = ci->imageExtent;
                flog("[fg] AMD swapchain resized to %ux%u", ci->imageExtent.width, ci->imageExtent.height);
                return VK_SUCCESS;
            }
            flog("[fg] AMD's swapchain could not be resized to %ux%u (%d): making a new one", ci->imageExtent.width,
                 ci->imageExtent.height, (int)r);
        } else if (obj && F.acquired != F.presented) {
            // its counts would stay one image off forever (every image presented one frame late)
            flog("[fg] an image acquired from AMD's swapchain was never presented: making a new one");
        } else if (obj && !same) {
            flog("[fg] another format or color space: making a new AMD swapchain");
        }
        teardown();  // and a new one below, like the first time
    }
    if (!vk_fg_usable()) return VK_ERROR_FEATURE_NOT_PRESENT;
    ffxCreateContextDescFrameGenerationSwapChainVK d = {};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    d.physicalDevice = api()->phys;
    d.device = api()->dev;
    VkSwapchainKHR none = VK_NULL_HANDLE;
    d.swapchain = &none;
    d.createInfo = *ci;
    d.createInfo.oldSwapchain = VK_NULL_HANDLE;
    d.gameQueue = {F.game, F.family, submit_game};
    d.asyncComputeQueue = {VK_NULL_HANDLE, F.family, nullptr};
    d.presentQueue = {F.present, F.family, nullptr};
    d.imageAcquireQueue = {F.acquire, F.family, nullptr};
    ffxCreateContextDescFrameGenerationSwapChainVersionVK ver = {};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
    ver.version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION;
    d.header.pNext = &ver.header;
    ffxReturnCode_t r = api_create(&F.sc, &d.header);
    if (r != FFX_API_RETURN_OK) {
        F.sc = nullptr;
        fail("AMD's swapchain", r);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    {
        FfxApiEffectMemoryUsage mu = {};
        ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageVK q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_VK;
        q.gpuMemoryUsageFrameGenerationSwapchain = &mu;
        g_fg_vram_sc = api_query(&F.sc, &q.header) == FFX_API_RETURN_OK ? mu.totalUsageInBytes : 0;
    }
    F.fn = {};
    F.fn.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
    if (api_query(&F.sc, &F.fn.header) != FFX_API_RETURN_OK || !F.fn.pOutQueuePresentKHR || !F.fn.pOutAcquireNextImageKHR ||
        !F.fn.pOutGetSwapchainImagesKHR || !F.fn.pOutCreateSwapchainFFXAPI) {
        // the swapchain the context made goes away with it (not handed to anyone yet); AMD's object outlives its
        // context and goes through its own function, if the query got as far as giving it
        api_destroy(&F.sc);
        if (none && F.fn.pOutDestroySwapchainFFXAPI) F.fn.pOutDestroySwapchainFFXAPI(api()->dev, none, nullptr, nullptr);
        F.cur = VK_NULL_HANDLE;
        fail("AMD's swapchain functions", 0);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *sc = none;
    F.cur = none, F.pending = VK_NULL_HANDLE, F.acquired = 0, F.presented = 0;
    F.sc_fmt = ci->imageFormat, F.sc_cs = ci->imageColorSpace, F.sc_ext = ci->imageExtent;
    F.rebuild = 0;
    if (!F.fg) snprintf(F.status, sizeof F.status, "%s", tr("ready (starts with the first FSR frame)"));
    const char *e = getenv("LARECOMP_FG_PSHOT");
    P.at = e ? atol(e) : -1;
    flog("[fg] AMD frame generation swapchain (%s), %ux%u, %u images", provider(&F.sc),
         ci->imageExtent.width, ci->imageExtent.height, ci->minImageCount);
    return VK_SUCCESS;
}

extern "C" void vk_fg_destroy_swapchain(VkSwapchainKHR sc) {
    if (F.sc && sc) F.pending = sc;  // (remade in place in vk_fg_create_swapchain, or destroyed in vk_fg_release)
}

// AMD's doesn't correct the count when given the array (it returned 8 for 3 images: views of null images, and the
// driver crashed): first how many there are, then those
extern "C" VkResult vk_fg_get_images(VkSwapchainKHR sc, uint32_t *n, VkImage *img) {
    uint32_t have = 0, cap = *n;
    VkResult r = F.fn.pOutGetSwapchainImagesKHR(api()->dev, sc, &have, nullptr);
    if (r != VK_SUCCESS) return *n = 0, r;
    if (have > cap) have = cap;
    for (uint32_t i = 0; i < cap; i++) img[i] = VK_NULL_HANDLE;
    r = F.fn.pOutGetSwapchainImagesKHR(api()->dev, sc, &have, img);
    uint32_t k = 0;
    while (k < have && img[k]) k++;
    *n = k;
    return r == VK_INCOMPLETE ? VK_SUCCESS : r;
}

// Both are counted whatever they return (vk_fg_create_swapchain compares the counts): AMD's swapchain counts an acquire
// even if its own submission fails, and counting one too many only costs remaking the object.
extern "C" VkResult vk_fg_acquire(VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, uint32_t *idx) {
    F.acquired++;
    return F.fn.pOutAcquireNextImageKHR(api()->dev, sc, timeout, sem, VK_NULL_HANDLE, idx);
}

extern "C" VkResult vk_fg_present(VkQueue queue, const VkPresentInfoKHR *pi) {
    F.presented++;
    VkResult r = F.fn.pOutQueuePresentKHR(queue, pi);
    F.presents++;
    if (P.n == 6 && !P.saved && F.presents >= P.saved_at) {  // (tests) what was presented, already copied
        P.saved = true;
        for (int i = 0; i < 6; i++) {
            char path[64];
            snprintf(path, sizeof path, "build/fg_pres_%d_%s.png", i, P.gen[i] ? "generated" : "real");
            shot_save(P.img[i], path);
        }
    }
    if (F.presents == 120 || F.presents % 3600 == 0) {
        uint64_t shown = F.fn.pOutGetLastPresentCountFFXAPI && F.cur ? F.fn.pOutGetLastPresentCountFFXAPI(F.cur) : 0;
        flog("[fg] %llu game frames, %llu generated (%llu with the runtime's menu apart), %llu presented in total; "
             "not generated: %llu paused (menu), %llu without FSR, %llu without camera", (unsigned long long)F.presents,
             (unsigned long long)F.generated, (unsigned long long)F.hudless, (unsigned long long)shown,
             (unsigned long long)F.paused, (unsigned long long)F.no_inputs, (unsigned long long)F.no_camera);
    }
    return r;
}

static void configure(bool on, const FgPrepare *p) {
    F.on.store(on ? 1 : 0, std::memory_order_relaxed);
    F.frame++;
    ffxConfigureDescFrameGeneration cfg = {};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = (void *)F.cur;
    cfg.frameGenerationCallback = generate;
    cfg.frameGenerationCallbackUserContext = &F.fg;
    if (P.at > 0) cfg.presentCallback = present_cb;  // (tests: the composition, here)
    cfg.frameGenerationEnabled = on;
    cfg.allowAsyncWorkloads = false;
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("LARECOMP_FG_DEBUG") ? atoi(getenv("LARECOMP_FG_DEBUG")) : 0;
    if (dbg == 1) cfg.flags = FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS;
    // only inside the game image: with black bars around it, generating over the whole window warped its edges
    if (p && p->pic[2] > 0 && p->pic[3] > 0 && (uint32_t)(p->pic[0] + p->pic[2]) <= F.sc_ext.width &&
        (uint32_t)(p->pic[1] + p->pic[3]) <= F.sc_ext.height)
        cfg.generationRect = {p->pic[0], p->pic[1], p->pic[2], p->pic[3]};
    else
        cfg.generationRect = {0, 0, (int32_t)F.sc_ext.width, (int32_t)F.sc_ext.height};
    // the window without the runtime menu: AMD doesn't interpolate what the menu draws on top (the FPS counter)
    if (on && p && p->hudless.image && p->hudless.width == F.sc_ext.width && p->hudless.height == F.sc_ext.height &&
        p->hudless.format == F.sc_fmt) {
        cfg.HUDLessColor = res(&p->hudless);
        F.hudless++;
    }
    cfg.frameID = F.frame;
    F.configured_at = F.presents;
    ffxReturnCode_t r = api_configure(&F.fg, &cfg.header);
    if (r != FFX_API_RETURN_OK) fail("the generation settings", r);
}

extern "C" void vk_fg_prepare_record(VkCommandBuffer cb, const void *data) {
    const FgPrepare *p = (const FgPrepare *)data;
    if (!vk_fg_active() || F.failed || !F.cur) return;
    bool have = p->depth.image && p->mv.image && p->render_w && p->render_h && p->render_w <= p->depth.width &&
                p->render_h <= p->depth.height;
    // the context was made for another version, another depth or smaller scenes: device.c recreates the swapchain on
    // the next frame (with the GPU idle) and the context with it; until then, no generation
    if (F.fg && (F.fg_version != p->version || F.fg_inverted != p->depth_inverted || p->depth.width > F.fg_max.width ||
                 p->depth.height > F.fg_max.height)) {
        F.rebuild = 1;
        have = false;
    }
    if (!F.fg && !F.rebuild && have) {
        VkExtent2D mr = {F.sc_ext.width > p->depth.width ? F.sc_ext.width : p->depth.width,
                         F.sc_ext.height > p->depth.height ? F.sc_ext.height : p->depth.height};
        if (!create_fg(mr, p->version, p->depth_inverted)) return;
    }
    if (!F.fg) {  // (no context yet: no generation this frame)
        F.no_inputs++;
        return;
    }
    // the camera: FG 4 needs it (without it, the last good one; before the first one, no generation); 3.1 makes use of
    // it
    FgCamera cam = p->cam;
    if (cam.ok) F.cam = cam;
    else if (F.cam.ok) cam = F.cam, F.no_camera++;
    else if (F.fg4) have = false, F.no_camera++;
    if (!cam.ok) {  // (3.1 with no camera yet: unit axes, never zero vectors)
        cam = FgCamera{};
        cam.right[0] = cam.up[1] = cam.fwd[2] = 1;
    }
    bool on = have && !p->paused;
    if (!have) F.no_inputs++;
    else if (!on) F.paused++;
    configure(on, p);
    if (F.failed || !on) return;
    auto now = std::chrono::steady_clock::now();
    double dt = F.prepared ? std::chrono::duration<double, std::milli>(now - F.last_prepare).count() : 16.67;
    F.last_prepare = now;
    F.prepared++;
    ffxDispatchDescFrameGenerationPrepareV2 d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    d.frameID = F.frame;
    d.commandList = cb;
    d.renderSize = {p->render_w, p->render_h};
    d.jitterOffset = {p->jitter_x, p->jitter_y};
    d.motionVectorScale = {p->mv_scale_x, p->mv_scale_y};
    {   // (tests) LARECOMP_FG_MV="x,y": multipliers for the motion vector scale, for generation only
        static float mx = 0, my = 0;
        static int read;
        if (!read++) {
            mx = my = 1;
            if (getenv("LARECOMP_FG_MV")) sscanf(getenv("LARECOMP_FG_MV"), "%f,%f", &mx, &my);
        }
        d.motionVectorScale.x *= mx, d.motionVectorScale.y *= my;
    }
    d.frameTimeDelta = dt > 0 && dt < 100 ? (float)dt : 16.67f;
    d.reset = p->reset;
    d.cameraNear = cam.ok && cam.znear > 0 ? cam.znear : 0.3f;
    d.cameraFar = cam.ok && cam.zfar > d.cameraNear ? cam.zfar : 1000.0f;
    d.cameraFovAngleVertical = cam.ok && cam.fov_y > 0.01f && cam.fov_y < 3.0f ? cam.fov_y : 0.6f;
    d.viewSpaceToMetersFactor = 1.0f;  // (Unity: meters)
    d.depth = res(&p->depth);
    d.motionVectors = res(&p->mv);
    for (int i = 0; i < 3; i++)
        d.cameraPosition[i] = cam.pos[i], d.cameraUp[i] = cam.up[i], d.cameraRight[i] = cam.right[i], d.cameraForward[i] = cam.fwd[i];
    full_barrier(cb);
    ffxReturnCode_t r = api_dispatch(&F.fg, &d.header);
    full_barrier(cb);
    if (r != FFX_API_RETURN_OK) fail("the generation setup", r);
}

// Before the window is submitted: a frame that didn't reach FSR (without the motion pass) is configured without
// generation, so the swapchain doesn't generate from another frame's data
extern "C" void vk_fg_frame_end_record(VkCommandBuffer cb, const void *unused) {
    (void)cb, (void)unused;
    if (!vk_fg_active() || F.failed || !F.cur || !F.fg) return;
    if (F.configured_at == F.presents) return;  // (configured at its FSR pass)
    F.no_inputs++;
    configure(false, nullptr);
}

extern "C" void vk_fg_wait_presents(void) {
    if (!F.sc) return;
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK w = {};
    w.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
    api_dispatch(&F.sc, &w.header);
}

extern "C" void vk_fg_release(void) {
    if (F.sc || F.fg) teardown();
    F.on.store(0, std::memory_order_relaxed);
    if (!api()) return;
    for (Shot &s : P.img) {
        if (s.mem) vkFreeMemory(api()->dev, s.mem, nullptr);
        if (s.buf) vkDestroyBuffer(api()->dev, s.buf, nullptr);
        s = Shot();
    }
    F.on.store(0, std::memory_order_relaxed);
}

extern "C" const char *vk_fg_status(void) {
    if (!F.status[0]) vk_fg_dll();
    return F.status[0] ? F.status : tr("not used yet");
}
