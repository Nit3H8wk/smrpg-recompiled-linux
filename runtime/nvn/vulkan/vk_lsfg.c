// (SMRPG) Lossless Scaling frame generation (LSFG 3.1) inside the port, with the separate library from tools/lsfg
// (lsfg.dll / liblsfg.so: lsfg-vk-android in single-device mode). Menu > Video > "Lossless Scaling frame generation":
// x2, x3 or x4. What it costs (the menu's impact panel and the log): GPU timestamps around its work and the video
// memory it takes, according to the driver.
//
// Each real frame (vk_lsfg_frame_end, from vk_frame_end, on the translating thread): the window image is copied to one
// of LSFG's two inputs (in turns) with the frame's own commands, which are submitted as usual. Then, on the recorder
// thread and in order (rec_QueuePresentCall: it owns the queue), LSFG generates the N intermediate images between the
// previous real frame and this one (it submits its work to the queue, behind the frame), each one is copied to a
// swapchain image with a command buffer of its own and presented (like lsfg-vk-android in its layer, src/context.cpp,
// Android path); the real one is presented in the next ls_run, before that one's generated frames (pend, below: why).
// The swapchain runs in FIFO with N + 1 more images (create_swapchain, vk_lsfg_extra_images): each image is shown for
// one refresh, and the game runs at the display's refresh rate / (N + 1).
// Without the library, without the player's Lossless.dll, without what its shaders need, with HDR or with AMD's
// generation: not used (vk_lsfg_status says so in the menu). With the menu open, only the real frame (and the first one
// afterwards, without intermediates: the previous one would be from before the menu).
// Lossless.dll: LARECOMP_LSFG_DLL; on Android, the one in the data folder; on Windows, the one next to the executable
// or Steam's (the registry's SteamPath key and the libraries in its libraryfolders.vdf). It never ships with the port.
// Tests: LARECOMP_SET lsfg / lsfg_flow / lsfg_perf; LARECOMP_LSFG_DUMP=<from>[,<count>[,<step>]] (in real frames): the
// previous input, the generated frames and the real one, as PNG (lsfg_<n>_0anterior, _1generada.., _<N+1>real);
// LARECOMP_LSFG_ORIG_MIPMAPS=1; LARECOMP_LSFG_FAIL_AT=<real frame>: a failed generation for testing (the context is
// rebuilt).
#include "vk.h"
#include "record.h"
#include "../../settings.h"
#include "../../ui/tr.h"
#include "../../../tools/lsfg/lsfg_smrpg.h"
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define LS_MAX_GEN 3  // (x4)
#define LS_SLOTS 8

extern bool menu_is_open(void);
extern bool vk_fg_active(void);
extern void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
extern void rec_drain(void);
extern void vk_queue_lock(void);  // (record.c) the queue, with its lock
extern void vk_queue_unlock(void);

static struct {
    int lib_state;  // -1 not tried, 0 missing, 1 loaded
    SDL_SharedObject *lib;
    PFN_lsfg_smrpg_init init;
    PFN_lsfg_smrpg_create create;
    PFN_lsfg_smrpg_present present;
    PFN_lsfg_smrpg_destroy destroy;
    PFN_lsfg_smrpg_finalize finalize;
    int dll_state;  // -1 not looked for, 0 missing, 1 found (dll)
    char dll[1024];
    int up_mult, up_flow, up_perf;  // what the library is initialized with (up_mult 0: it isn't)
    int bad_mult, bad_flow, bad_perf;  // what it failed with (not retried with the same)
    char status[600], status_ui[600];
    int ctx;          // the LSFG context (-1: none)
    uint32_t w, h;    // its size
    int n;            // images generated per real frame
    VkImage img[2 + LS_MAX_GEN];  // 0, 1: the inputs; 2..: the outputs
    VmaAllocation mem[2 + LS_MAX_GEN];
    // the recorder thread's
    VkCommandPool pool;
    VkCommandBuffer cb[LS_SLOTS][LS_MAX_GEN];
    VkFence fence[LS_SLOTS][LS_MAX_GEN];
    VkSemaphore acq[LS_SLOTS][LS_MAX_GEN], gen[LS_SLOTS][LS_MAX_GEN];
    // (recorder) what it costs on the GPU: one timestamp before its work (when what came before, the frame, finishes)
    // and another after, per slot; they are read LS_SLOTS frames later, already finished
    VkQueryPool qp;
    VkCommandBuffer tcb[LS_SLOTS][2];
    bool ts_used[LS_SLOTS];
    double gpu_ms;    // average, per real frame (0: not measured)
    uint64_t vram_lib, vram_ctx;  // its own video memory (not through VMA) at initialization and with the context
    bool vram_measured;           // (with VK_EXT_memory_budget)
    uint64_t frames;  // (translating thread) real frames given to LSFG
    bool skip_next;   // (translating thread) the next one, without intermediates
    uint64_t st_real, st_gen, st_skip, st_fail;  // (recorder)
    // (recorder) its generation or the submission of a generated frame failed: its count (frameIdx, which only advances
    // if all goes well) or one of its fences no longer match ours. The translating thread rebuilds the context
    // (vk_lsfg_frame_end).
    atomic_bool broken;
    int rebuilds;      // (translating thread) contexts rebuilt because of that, in a row
    uint64_t last_ns;  // (translating thread) when the last real frame reached it
    uint64_t pw_last;  // (recorder) the time inside vkQueuePresentKHR up to the last stats line
    double init_ms;    // (translating thread) how long the library took to initialize, for the log
    // (translating thread, battle_slow) not generating in this battle; and the per-second count of real frames
    bool bt_slow;
    uint64_t bt_t0, bt_last;
    int bt_n, bt_low;
} L = {.lib_state = -1, .dll_state = -1, .ctx = -1};

// (recorder) the last frame's real image, not presented yet. On Android the Vulkan loader connects the window as EGL,
// and for EGL its queueBuffer waits for the fence of the image queued just before (BufferQueueProducer, "Throttling EGL
// Production"): presented behind its generated frame, the real one left the recorder stuck inside vkQueuePresentKHR
// until the GPU finished the frame and the generation (14.9 ms per real frame on the Thor at 720p with x2; without
// LSFG, 2.7): meanwhile nobody submitted the next frame, and the GPU stayed at 75% with the game at 52 real fps. Now it
// is presented in the next ls_run (the earlier generated frame is already done: no waiting; it is shown behind it,
// almost always on the same refresh as before: on average, about 3 ms later), or before a normal present (present_run:
// the menu open, LSFG off) or before the swapchain is recreated or dropped (device.c, with the recorder stopped). Its
// semaphore is its image's (device.c done_sem[img]): it isn't used again until that image is presented.
static struct { bool on; VkSemaphore wait; VkSwapchainKHR sc; uint32_t img; } pend;
// quiet: before the swapchain is recreated or dropped (without asking for another if it answers OUT_OF_DATE or
// SURFACE_LOST: its semaphore is consumed all the same, as vkQueuePresentKHR says)
static void pend_present(bool quiet) {
    if (!pend.on) return;
    pend.on = false;
    if (!quiet) { rec_present_direct(pend.wait, pend.sc, pend.img); return; }
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &pend.wait,
                           .swapchainCount = 1, .pSwapchains = &pend.sc, .pImageIndices = &pend.img};
    vk_queue_lock();
    vkQueuePresentKHR(g_vk.queue, &pi);
    vk_queue_unlock();
}
void vk_lsfg_flush_pending(void) { pend_present(false); }  // (record.c present_run, on the recorder)
void vk_lsfg_drop_pending(void) { pend_present(true); }    // (device.c recreate_swapchain / surface_drop, after rec_drain)

static void status(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    char s[sizeof L.status];
    vsnprintf(s, sizeof s, fmt, ap);  // (English: the log)
    va_end(ap);
    if (strcmp(s, L.status)) {
        snprintf(L.status, sizeof L.status, "%s", s);
        vsnprintf(L.status_ui, sizeof L.status_ui, tr(fmt), ap2);
        LOG("[lsfg] %s\n", s);
    }
    va_end(ap2);
}
const char *vk_lsfg_status(void) { return L.status[0] ? L.status_ui : tr("not used yet"); }

static bool lib_load(void) {
    if (L.lib_state >= 0) return L.lib_state > 0;
    L.lib_state = 0;
#ifdef _WIN32
    char path[1024];
    const char *base = SDL_GetBasePath();
    snprintf(path, sizeof path, "%slsfg.dll", base ? base : "");
#else
    const char *path = "liblsfg.so";
#endif
    L.lib = SDL_LoadObject(path);
    if (!L.lib) { status("the library %s is missing (%s)", path, SDL_GetError()); return false; }
    PFN_lsfg_smrpg_abi abi = (PFN_lsfg_smrpg_abi)SDL_LoadFunction(L.lib, "lsfg_smrpg_abi");
    L.init = (PFN_lsfg_smrpg_init)SDL_LoadFunction(L.lib, "lsfg_smrpg_init");
    L.create = (PFN_lsfg_smrpg_create)SDL_LoadFunction(L.lib, "lsfg_smrpg_create");
    L.present = (PFN_lsfg_smrpg_present)SDL_LoadFunction(L.lib, "lsfg_smrpg_present");
    L.destroy = (PFN_lsfg_smrpg_destroy)SDL_LoadFunction(L.lib, "lsfg_smrpg_destroy");
    L.finalize = (PFN_lsfg_smrpg_finalize)SDL_LoadFunction(L.lib, "lsfg_smrpg_finalize");
    if (!abi || !L.init || !L.create || !L.present || !L.destroy || !L.finalize || abi() != LSFG_SMRPG_ABI) {
        status("the library %s is not from this version of the port", path);
        SDL_UnloadObject(L.lib);
        L.lib = NULL;
        return false;
    }
    L.lib_state = 1;
    return true;
}

static bool file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}
#ifdef _WIN32
// the Steam libraries: SteamPath from the registry and the "path" entries in its steamapps/libraryfolders.vdf
static bool steam_find(char *out, size_t n) {
    char sp[512];
    DWORD sz = sizeof sp;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, NULL, sp, &sz) != ERROR_SUCCESS)
        return false;
    snprintf(out, n, "%s/steamapps/common/Lossless Scaling/Lossless.dll", sp);
    if (file_exists(out)) return true;
    char vdf[600];
    snprintf(vdf, sizeof vdf, "%s/steamapps/libraryfolders.vdf", sp);
    FILE *f = fopen(vdf, "rb");
    if (!f) return false;
    char line[1200];
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) {
        char *k = strstr(line, "\"path\"");
        if (!k) continue;
        char *a = strchr(k + 6, '"'), *b = a ? strchr(a + 1, '"') : NULL;
        if (!a || !b) continue;
        char lib[600];
        size_t m = 0;
        for (char *c = a + 1; c < b && m + 1 < sizeof lib; c++) {
            if (c[0] == '\\' && c[1] == '\\') c++;  // (the VDF escapes backslashes)
            lib[m++] = *c;
        }
        lib[m] = 0;
        snprintf(out, n, "%s/steamapps/common/Lossless Scaling/Lossless.dll", lib);
        found = file_exists(out);
    }
    fclose(f);
    return found;
}
#endif
static bool dll_find(void) {
    if (L.dll_state >= 0) return L.dll_state > 0;
    L.dll_state = 0;
    const char *e = getenv("LARECOMP_LSFG_DLL");
    if (e && *e) snprintf(L.dll, sizeof L.dll, "%s", e);
#ifdef __ANDROID__
    else {
        const char *root = getenv("LARECOMP_DATA_ROOT");
        snprintf(L.dll, sizeof L.dll, "%s/Lossless.dll", root ? root : ".");
    }
#elif defined(_WIN32)
    else {
        const char *base = SDL_GetBasePath();
        snprintf(L.dll, sizeof L.dll, "%sLossless.dll", base ? base : "");
        if (!file_exists(L.dll) && !steam_find(L.dll, sizeof L.dll)) L.dll[0] = 0;
    }
#else
    else snprintf(L.dll, sizeof L.dll, "Lossless.dll");
#endif
    if (!L.dll[0] || !file_exists(L.dll)) {
#ifdef __ANDROID__
        status("Lossless.dll is missing: copy it from your Lossless Scaling (Steam) to the game's data folder");
#else
        status("Lossless.dll was not found: install Lossless Scaling from Steam (or put it next to the game)");
#endif
        return false;
    }
    L.dll_state = 1;
    return true;
}

// Is it used? (create_swapchain: FIFO and N more images; vk_frame_begin recreates the swapchain if it changes)
bool vk_lsfg_wanted(void) {
    int m = g_settings.lsfg;
    if (m < 2 || m > 4) { status("off"); return false; }  // (the status, for the menu: why it isn't running)
    if (g_settings.hdr) { status("does not work with HDR (turn HDR off to use it)"); return false; }
    if (vk_fg_active()) { status("does not work together with AMD frame generation"); return false; }
    if (!g_vk.lsfg_features) {
        status("the GPU lacks what its shaders need (extended image formats, Vulkan memory model)");
        return false;
    }
    if (!lib_load() || !dll_find()) return false;
    int flow = g_settings.lsfg_flow, perf = g_settings.lsfg_perf;
    return !(m == L.bad_mult && flow == L.bad_flow && perf == L.bad_perf);
}
// (one per generated frame, which are acquired after the real one, another because the real one waits for the next
// frame - pend - and another to spare: with one fewer, on the Thor, the translating thread waited 3-7 ms per frame for
// an image to be available)
uint32_t vk_lsfg_extra_images(void) { return vk_lsfg_wanted() ? (uint32_t)g_settings.lsfg + 1 : 0; }

static void barrier(VkCommandBuffer cb, bool rec, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss,
                    VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                               .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    if (rec) rec_CmdPipelineBarrier2(cb, &di);  // (in the frame's commands: the recorder records them)
    else vkCmdPipelineBarrier2(cb, &di);        // (in one of the recorder's own)
}

// releases everything the context owns (with the queue idle: rec_drain + vkDeviceWaitIdle before). The library submits
// to the queue when it is initialized, when a context is created, when it is deleted and when it shuts down
// (drainQueueBounded: an empty submission with a fence): always under the queue lock, which other threads use too.
static void ctx_free(void) {
    if (L.ctx >= 0) {
        vk_queue_lock();
        L.destroy(L.ctx);
        vk_queue_unlock();
    }
    L.ctx = -1;
    for (int i = 0; i < 2 + LS_MAX_GEN; i++)
        if (L.img[i]) vmaDestroyImage(g_vk.vma, L.img[i], L.mem[i]), L.img[i] = VK_NULL_HANDLE;
    for (int s = 0; s < LS_SLOTS; s++)
        for (int i = 0; i < LS_MAX_GEN; i++) {
            if (L.fence[s][i]) vkDestroyFence(g_vk.dev, L.fence[s][i], NULL), L.fence[s][i] = VK_NULL_HANDLE;
            if (L.acq[s][i]) vkDestroySemaphore(g_vk.dev, L.acq[s][i], NULL), L.acq[s][i] = VK_NULL_HANDLE;
            if (L.gen[s][i]) vkDestroySemaphore(g_vk.dev, L.gen[s][i], NULL), L.gen[s][i] = VK_NULL_HANDLE;
            L.cb[s][i] = VK_NULL_HANDLE;
        }
    if (L.qp) vkDestroyQueryPool(g_vk.dev, L.qp, NULL), L.qp = VK_NULL_HANDLE;
    memset(L.tcb, 0, sizeof L.tcb), memset(L.ts_used, 0, sizeof L.ts_used), L.gpu_ms = 0;
    if (L.pool) vkDestroyCommandPool(g_vk.dev, L.pool, NULL), L.pool = VK_NULL_HANDLE;
    L.w = L.h = 0, L.n = 0;
}
static void quiesce(void) {
    rec_drain();
    vk_queue_lock();
    vkQueueWaitIdle(g_vk.queue);
    vk_queue_unlock();
}
// shuts the library down and releases everything it owns (other settings; off, or with HDR or AMD's: its memory is
// freed)
static void lib_stop(void) {
    if (!L.up_mult) return;
    quiesce();
    ctx_free();
    vk_queue_lock();
    L.finalize();
    vk_queue_unlock();
    L.up_mult = 0;
}
// what the process uses now in the GPU's local heaps, according to the driver (0 without VK_EXT_memory_budget): the
// difference between before and after starting up is LSFG's, including its internals (which don't go through VMA)
static uint64_t vram_now(void) {
    if (!g_vk.budget_ext) return 0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &bp};
    vkGetPhysicalDeviceMemoryProperties2(g_vk.phys, &mp);
    uint64_t u = 0;
    for (uint32_t i = 0; i < mp.memoryProperties.memoryHeapCount; i++)
        if (mp.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) u += bp.heapUsage[i];
    return u;
}

// The library initialized with the current settings and a context the size of the window (on the translating thread)
static bool setup(VkCommandBuffer cb) {
    int m = g_settings.lsfg, flow = g_settings.lsfg_flow, perf = g_settings.lsfg_perf ? 1 : 0;
    uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
    if (L.up_mult && (L.up_mult != m || L.up_flow != flow || L.up_perf != perf)) lib_stop();  // (other settings: from scratch)
    if (!L.up_mult) {
        static int orig_mip = -1;
        if (orig_mip < 0) orig_mip = getenv("LARECOMP_LSFG_ORIG_MIPMAPS") && atoi(getenv("LARECOMP_LSFG_ORIG_MIPMAPS")) != 0;
        LsfgSmrpgInit in = {.gipa = vkGetInstanceProcAddr, .instance = g_vk.inst, .phys = g_vk.phys, .device = g_vk.dev,
                            .queue_family = g_vk.qfam, .queue = g_vk.queue, .dll_path = L.dll, .performance = perf,
                            .flow_scale = (float)flow / 100.0f, .generations = m - 1, .hdr = 0, .orig_mipmaps = orig_mip,
                            .cache_path = "cache/lsfg_pipelines.bin"};  // (next to the pipeline cache: device.c)
        char err[400] = {0};
        quiesce();
        uint64_t v0 = vram_now(), t0 = SDL_GetTicksNS();
        vk_queue_lock();
        int ir = L.init(&in, err, sizeof err);
        if (ir != 0) L.finalize();
        vk_queue_unlock();
        L.init_ms = (SDL_GetTicksNS() - t0) / 1e6;  // (translating its shaders: its pipelines are created with the context)
        if (ir != 0) {
            status("could not start: %s", err);
            L.bad_mult = m, L.bad_flow = flow, L.bad_perf = perf;
            return false;
        }
        uint64_t v1 = vram_now();
        L.vram_lib = v1 > v0 ? v1 - v0 : 0;
        L.up_mult = m, L.up_flow = flow, L.up_perf = perf;
    }
    if (L.ctx >= 0 && L.w == w && L.h == h && L.n == m - 1) return true;
    quiesce();  // (creating the context may submit work to the queue: recorder stopped)
    ctx_free();
    int n = m - 1;
    VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;  // (no HDR, like lsfg-vk: its shaders work in floating point)
    for (int i = 0; i < 2 + n; i++) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(g_vk.vma, &ci, &ai, &L.img[i], &L.mem[i], NULL) != VK_SUCCESS) {
            L.img[i] = VK_NULL_HANDLE;
            status("no memory for its images (%ux%u)", w, h);
            ctx_free();
            L.bad_mult = m, L.bad_flow = flow, L.bad_perf = perf;
            return false;
        }
    }
    char err[400] = {0};
    uint64_t v0 = vram_now(), t0 = SDL_GetTicksNS();
    vk_queue_lock();
    int ctx = L.create(L.img, L.img + 2, n, w, h, fmt, err, sizeof err);
    vk_queue_unlock();
    double ctx_ms = (SDL_GetTicksNS() - t0) / 1e6;
    uint64_t v1 = vram_now();
    L.vram_ctx = v1 > v0 ? v1 - v0 : 0, L.vram_measured = v0 > 0;
    if (ctx < 0) {
        status("its context could not be created: %s", err);
        ctx_free();
        L.bad_mult = m, L.bad_flow = flow, L.bad_perf = perf;
        return false;
    }
    // (its images to GENERAL in the frame's commands: only now, with the context made. If something fails earlier, they
    // are destroyed right away, and this frame, which is still submitted the usual way, must not reference them)
    for (int i = 0; i < 2 + n; i++)
        barrier(cb, true, L.img[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    L.ctx = ctx, L.w = w, L.h = h, L.n = n;
    // (the new context starts its count at 0 and reads the new image from input frameIdx % 2: ours must do the same, or
    // the intermediates would go backwards with x3 / x4)
    L.frames = 0;
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &L.pool));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (int s = 0; s < LS_SLOTS; s++)
        for (int i = 0; i < n; i++) {
            VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = L.pool,
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
            VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, &L.cb[s][i]));
            VK_CHECK(vkCreateFence(g_vk.dev, &fi, NULL, &L.fence[s][i]));
            VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &L.acq[s][i]));
            VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &L.gen[s][i]));
        }
    if (g_vk.ts_bits && g_vk.props.limits.timestampPeriod > 0) {  // (the timestamps: recorded once per slot)
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                    .queryCount = 2 * LS_SLOTS};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &L.qp) != VK_SUCCESS) L.qp = VK_NULL_HANDLE;
        for (int s = 0; L.qp && s < LS_SLOTS; s++)
            for (int j = 0; j < 2; j++) {
                VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = L.pool,
                                                  .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
                VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, &L.tcb[s][j]));
                VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                               .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT};
                vkBeginCommandBuffer(L.tcb[s][j], &bi);
                if (!j) vkCmdResetQueryPool(L.tcb[s][j], L.qp, 2 * s, 2);
                vkCmdWriteTimestamp2(L.tcb[s][j], VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, L.qp, 2 * s + j);
                vkEndCommandBuffer(L.tcb[s][j]);
            }
    }
    L.skip_next = true;  // (the first one: no previous real frame)
    status("active: x%d, flow %d%%, %s, %ux%u, with %s", m, flow, perf ? "performance mode" : "quality", w, h, L.dll);
    extern float vk_display_hz(void);
    LOG("[lsfg] time: %.0f ms to start (translating its shaders), %.0f ms its context (its pipelines, with their "
        "cache); display at %.0f Hz\n",
        L.init_ms, ctx_ms, vk_display_hz());
    L.init_ms = 0;  // (already reported: with another window, only the context)
    if (L.vram_measured)
        LOG("[lsfg] video memory: %.1f MB its own (%.1f at startup, %.1f the context) and %.1f for its images\n",
            (L.vram_lib + L.vram_ctx) / 1048576.0, L.vram_lib / 1048576.0, L.vram_ctx / 1048576.0,
            (2.0 + n) * w * h * 8 / 1048576.0);
    return true;
}

// (LARECOMP_LSFG_DUMP) LSFG images (RGBA16F) to PNG, on the recorder, waiting for the GPU
static float half_to_float(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff, f;
    if (e == 0) {
        if (!m) f = s;
        else {
            e = 127 - 15 + 1;
            while (!(m & 0x400)) m <<= 1, e--;
            f = s | (e << 23) | ((m & 0x3ff) << 13);
        }
    } else if (e == 31) f = s | 0x7f800000 | (m << 13);
    else f = s | ((e + 127 - 15) << 23) | (m << 13);
    float r;
    memcpy(&r, &f, 4);
    return r;
}
static void dump(uint64_t frame, int k) {
    // the previous one, the generated ones (in order: the first is the closest to the previous one) and the real one
    int m = L.n + 2;
    VkImage imgs[2 + LS_MAX_GEN];
    char names[2 + LS_MAX_GEN][16];
    imgs[0] = L.img[k ^ 1], snprintf(names[0], sizeof names[0], "0previous");
    for (int i = 0; i < L.n; i++) imgs[1 + i] = L.img[2 + i], snprintf(names[1 + i], sizeof names[1 + i], "%dgenerated", 1 + i);
    imgs[m - 1] = L.img[k], snprintf(names[m - 1], sizeof names[m - 1], "%dreal", m - 1);
    VkDeviceSize sz = (VkDeviceSize)L.w * L.h * 8;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz * (VkDeviceSize)m, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VkBuffer buf;
    VmaAllocation mem;
    VmaAllocationInfo info;
    if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &mem, &info) != VK_SUCCESS) return;
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = L.pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &cbi);
    for (int i = 0; i < m; i++) {
        barrier(cb, false, imgs[i], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy c = {.bufferOffset = sz * i, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .imageExtent = {L.w, L.h, 1}};
        vkCmdCopyImageToBuffer(cb, imgs[i], VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c);
    }
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f;
    VK_CHECK(vkCreateFence(g_vk.dev, &fi, NULL, &f));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    vk_queue_lock();
    vkQueueSubmit(g_vk.queue, 1, &si, f);
    vk_queue_unlock();
    vkWaitForFences(g_vk.dev, 1, &f, VK_TRUE, 5000000000ull);
    vmaInvalidateAllocation(g_vk.vma, mem, 0, VK_WHOLE_SIZE);
    uint8_t *px = malloc((size_t)L.w * L.h * 4);
    for (int i = 0; px && i < m; i++) {
        const uint16_t *src = (const uint16_t *)((const uint8_t *)info.pMappedData + sz * i);
        for (size_t p = 0; p < (size_t)L.w * L.h; p++)
            for (int c = 0; c < 4; c++) {
                float v = c == 3 ? 1.0f : half_to_float(src[p * 4 + c]);
                px[p * 4 + c] = (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f);
            }
        char path[128];
        snprintf(path, sizeof path, "lsfg_%llu_%s.png", (unsigned long long)frame, names[i]);
        vk_save_png(path, px, L.w, L.h);
    }
    free(px);
    vkDestroyFence(g_vk.dev, f, NULL);
    vkFreeCommandBuffers(g_vk.dev, L.pool, 1, &cb);
    vmaDestroyBuffer(g_vk.vma, buf, mem);
}

// ---- on the recorder thread: generate, present the generated frames and the real one
typedef struct { VkSemaphore done; uint32_t img; int k; bool skip; uint64_t frame; } LsRun;
static void ls_run(const void *p) {
    const LsRun *a = p;
    VkSwapchainKHR sc = g_vk.swapchain;
    int slot = (int)(a->frame % LS_SLOTS);
    char err[400] = {0};
    bool gen = false;
    // (the previous real frame, before this one's generated frames and before submitting LSFG's work: the fence the
    // driver gives a present - the one the next present waits for - follows queue order, and behind LSFG it would make
    // the generated frame wait until that finished: measured on the Thor, 6-10 ms per real frame in vkQueuePresentKHR)
    pend_present(false);
    if (L.ctx >= 0 && !atomic_load(&L.broken)) {  // (broken: neither generate nor wait for its fences until it is rebuilt)
        if (L.qp && L.ts_used[slot]) {  // (this slot's measurement from LS_SLOTS frames ago)
            uint64_t t[4];  // (per query: its value and whether it is available)
            if (vkGetQueryPoolResults(g_vk.dev, L.qp, 2 * slot, 2, sizeof t, t, 2 * sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS && t[1] && t[3]) {
                uint64_t t0 = t[0], t1 = t[2];
                if (g_vk.ts_bits < 64) { uint64_t m = (1ull << g_vk.ts_bits) - 1; t0 &= m, t1 &= m; }
                double ms = t1 >= t0 ? (double)(t1 - t0) * g_vk.props.limits.timestampPeriod / 1e6 : -1;
                if (ms >= 0 && ms < 1000) L.gpu_ms = L.gpu_ms > 0 ? L.gpu_ms * 0.9 + ms * 0.1 : ms;
            }
        }
        VkSubmitInfo ts[2];
        for (int j = 0; j < 2; j++)
            ts[j] = (VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &L.tcb[slot][j]};
        // (test of the failure path: LARECOMP_LSFG_FAIL_AT=<real frame>, without calling its present, like an exception
        // halfway through: its count doesn't advance)
        static long fail_at = -2;
        if (fail_at == -2) fail_at = getenv("LARECOMP_LSFG_FAIL_AT") ? atol(getenv("LARECOMP_LSFG_FAIL_AT")) : -1;
        bool fake_fail = fail_at >= 0 && (long)a->frame == fail_at;
        if (fake_fail) fail_at = -1;  // (once: the new context counts from 0 again)
        vk_queue_lock();
        bool timed = L.qp && vkQueueSubmit(g_vk.queue, 1, &ts[0], VK_NULL_HANDLE) == VK_SUCCESS;
        int r = fake_fail ? (snprintf(err, sizeof err, "test failure"), -1) : L.present(L.ctx, err, sizeof err);
        L.ts_used[slot] = timed && vkQueueSubmit(g_vk.queue, 1, &ts[1], VK_NULL_HANDLE) == VK_SUCCESS;
        vk_queue_unlock();
        if (r == 0) gen = true;
        else {
            // (its count didn't advance, and if it failed between creating a fence and submitting it, it waits for it
            // next time: another context)
            if (L.st_fail++ < 5) LOG("[lsfg] generation failed (%d): %s\n", r, err);
            atomic_store(&L.broken, true);
        }
    }
    static long dump_at = -2, dump_n = 1, dump_step = 1;  // (LARECOMP_LSFG_DUMP=<from>[,<count>[,<step>]])
    if (dump_at == -2) {
        const char *e = getenv("LARECOMP_LSFG_DUMP");
        dump_at = -1;
        if (e) sscanf(e, "%ld,%ld,%ld", &dump_at, &dump_n, &dump_step);
        if (dump_step < 1) dump_step = 1;
    }
    long fr = (long)a->frame;
    if (gen && dump_at >= 0 && fr >= dump_at && fr < dump_at + dump_n * dump_step && (fr - dump_at) % dump_step == 0)
        dump(a->frame, a->k);
    bool shown = false, cut = false;  // (some generated frame was presented; the swapchain failed: the real one, right
                                      // away, so that it reports it)
    for (int i = 0; gen && !a->skip && i < L.n; i++) {
        VkFence f = L.fence[slot][i];
        if (vkWaitForFences(g_vk.dev, 1, &f, VK_TRUE, 1000000000ull) != VK_SUCCESS) { L.st_skip++; continue; }
        uint32_t gi;
        VkResult r = vkAcquireNextImageKHR(g_vk.dev, sc, 100000000ull, L.acq[slot][i], VK_NULL_HANDLE, &gi);
        if (r == VK_TIMEOUT || r == VK_NOT_READY) { L.st_skip++; continue; }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { L.st_skip++; cut = true; break; }  // (OUT_OF_DATE: the real one
                                                            // will report it)
        vkResetFences(g_vk.dev, 1, &f);
        VkCommandBuffer cb = L.cb[slot][i];
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb, &bi);
        VkImage out = L.img[2 + i], dst = g_vk.sc_images[gi];
        barrier(cb, false, out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        barrier(cb, false, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int)L.w, (int)L.h, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int)L.w, (int)L.h, 1}}};
        vkCmdBlitImage(cb, out, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
        barrier(cb, false, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
        barrier(cb, false, out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_BLIT_BIT, 0,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        vkEndCommandBuffer(cb);
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &L.acq[slot][i],
                           .pWaitDstStageMask = &ws, .commandBufferCount = 1, .pCommandBuffers = &cb, .signalSemaphoreCount = 1,
                           .pSignalSemaphores = &L.gen[slot][i]};
        vk_queue_lock();
        VkResult sr = vkQueueSubmit(g_vk.queue, 1, &si, f);
        vk_queue_unlock();
        if (sr != VK_SUCCESS) {
            // (its fence was left reset with no submission and image gi acquired but not presented: another context and
            // another swapchain)
            LOG("[lsfg] submitting a generated frame: %d\n", sr);
            atomic_store(&L.broken, true);
            rec_request_recreate();
            cut = true;
            break;
        }
        rec_present_direct(L.gen[slot][i], sc, gi);
        L.st_gen++;
        shown = true;
    }
    if (shown && !cut) pend.on = true, pend.wait = a->done, pend.sc = sc, pend.img = a->img;
    else rec_present_direct(a->done, sc, a->img);  // (no generated frames ahead: it waits for nothing)
    extern uint64_t g_vk_frame_waits[4];  // (device.c: [2], the time inside vkQueuePresentKHR, for all of them)
    uint64_t pw = __atomic_load_n(&g_vk_frame_waits[2], __ATOMIC_RELAXED);
    if (++L.st_real == 1) L.pw_last = pw;
    if (L.st_real % 600 == 0) {
        LOG("[lsfg] %llu real frames, %llu generated presented, %llu skipped, %llu failed; GPU %.2f ms per real "
            "frame; presenting %.2f ms per real frame\n",
            (unsigned long long)L.st_real, (unsigned long long)L.st_gen, (unsigned long long)L.st_skip, (unsigned long long)L.st_fail,
            L.gpu_ms, (double)(pw - L.pw_last) / 600 / 1e6);
        L.pw_last = pw;
    }
}

// (menu: the impact panel) what it costs now: GPU ms per real frame (0: not measured; measured with the menu closed and
// kept for as long as the context lasts) and the bytes of video memory (0: no context): its images and, if *measured,
// its internals too. Only if the context is the one for the current settings and window: with the menu open it isn't
// rebuilt, and after changing x2/x3/x4, the flow or the mode, the measurement would be the previous one's (the menu
// estimates).
static bool ctx_current(void) {
    return L.ctx >= 0 && L.up_mult == g_settings.lsfg && L.up_flow == g_settings.lsfg_flow &&
           L.up_perf == (g_settings.lsfg_perf ? 1 : 0) && L.w == g_vk.sc_extent.width && L.h == g_vk.sc_extent.height;
}
double vk_lsfg_gpu_ms(void) { return ctx_current() ? L.gpu_ms : 0; }
uint64_t vk_lsfg_vram(int *measured) {
    bool cur = ctx_current();
    *measured = cur && L.vram_measured;
    if (!cur) return 0;
    return (uint64_t)(2 + L.n) * L.w * L.h * 8 + (L.vram_measured ? L.vram_lib + L.vram_ctx : 0);
}

// (hle/battle_rate.c) In battle the game needs 60 real fps: its logic takes 60 steps of 1/60 s per second, spread in
// real time across the frames (vsync ticks at the "FPS limit"), and its animations and sound are time-based: short of
// 60 real fps, steps bunch up and timed hits drift out of sync. With generation it may fall short: because of the
// display (in FIFO each image is one refresh: with x3 / x4 at 120 Hz, or x2 at 60 Hz, only 40 / 30 real frames fit,
// half as fast with x4) or because of the GPU (on the Thor at 1080p with x2, 39). So, in battle, the display is checked
// on every frame and real frames are counted per second: two seconds in a row below 56, and until the battle ends, real
// frames only (as with the menu open). Called with generation running (otherwise: battle_reset).
static void battle_reset(void) { L.bt_t0 = L.bt_last = 0, L.bt_n = L.bt_low = 0; }
static bool battle_slow(void) {
    extern bool rt_battle_vsync_60(void);
    extern float vk_display_hz(void);
    if (!rt_battle_vsync_60()) {
        if (L.bt_slow) LOG("[lsfg] battle over: generating again\n");
        L.bt_slow = false;
        battle_reset();
        return false;
    }
    if (L.bt_slow) return true;
    float hz = vk_display_hz();
    if (hz / (float)g_settings.lsfg < 59.0f) {
        LOG("[lsfg] battle: not generating while it lasts (with x%d at %.0f Hz only %.0f real frames fit; it needs "
            "60)\n", g_settings.lsfg,
            hz, hz / g_settings.lsfg);
        return L.bt_slow = true;
    }
    uint64_t now = SDL_GetTicksNS();
    if (!L.bt_t0 || now - L.bt_last > 100000000ull) L.bt_t0 = now, L.bt_n = 0;  // (start again from here after a gap)
    L.bt_last = now;
    if (now - L.bt_t0 >= 1000000000ull) {
        double fps = L.bt_n * 1e9 / (double)(now - L.bt_t0);
        L.bt_low = fps < 56 ? L.bt_low + 1 : 0;
        L.bt_t0 = now, L.bt_n = 0;
        if (L.bt_low >= 2) {
            LOG("[lsfg] battle: not generating while it lasts (with generation it ran at %.0f real frames; it needs "
                "60)\n", fps);
            return L.bt_slow = true;
        }
    }
    L.bt_n++;
    return false;
}

// (device.c vk_frame_end) the real frame's window, img, already drawn (its rendering closed, in
// COLOR_ATTACHMENT_OPTIMAL): if generation is running, copies it to LSFG's input, leaves it in PRESENT_SRC, submits the
// frame (acquired -> done) and queues the generation and the presents: true. false: the usual path.
bool vk_lsfg_frame_end(VkCommandBuffer cb, uint32_t img, VkSemaphore acquired, VkSemaphore done) {
    bool want = vk_lsfg_wanted();
    if (!want && L.up_mult) lib_stop();  // (off, or with HDR or AMD's: its memory is freed right away)
    // (any frame that doesn't reach it: the next one, without intermediates, since the previous one it has is no longer
    // the one just before; also with the menu open, and in a battle that wouldn't reach 60 real fps with generation)
    bool on = want && g_vk.sc_lsfg && !g_vk.hdr_active && !menu_is_open();
    if (!on) battle_reset();
    if (!on || battle_slow()) { L.skip_next = true; return false; }
    if (atomic_load(&L.broken)) {  // (it failed on the recorder: another context; on the fourth in a row, off with these settings)
        quiesce();
        ctx_free();
        atomic_store(&L.broken, false);
        LOG("[lsfg] context rebuilt after a failure (%d in a row)\n", L.rebuilds + 1);
        if (++L.rebuilds > 3) {
            status("failed several times in a row (see lasrecomp.log): off until its settings change");
            L.bad_mult = g_settings.lsfg, L.bad_flow = g_settings.lsfg_flow, L.bad_perf = g_settings.lsfg_perf ? 1 : 0;
            L.rebuilds = 0;
            lib_stop();
            L.skip_next = true;
            return false;
        }
    }
    if (!setup(cb)) { L.skip_next = true; return false; }
    if (L.frames == 600) L.rebuilds = 0;  // (600 good real frames in a row: earlier failures no longer count)
    // (a while without real frames (minimized window, lost surface, long load): the same)
    uint64_t now = SDL_GetTicksNS();
    if (now - L.last_ns > 250000000ull) L.skip_next = true;
    L.last_ns = now;
    int k = (int)(L.frames & 1);
    VkImage sc_img = g_vk.sc_images[img];
    barrier(cb, true, sc_img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(cb, true, L.img[k], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int)L.w, (int)L.h, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int)L.w, (int)L.h, 1}}};
    rec_CmdBlitImage(cb, sc_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, L.img[k], VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    barrier(cb, true, sc_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
    barrier(cb, true, L.img[k], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    extern void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra);
    vk_submit_extra(acquired, done, VK_NULL_HANDLE);
    LsRun a = {.done = done, .img = img, .k = k, .skip = L.skip_next, .frame = L.frames};
    rec_QueuePresentCall(ls_run, &a, sizeof a);
    L.frames++;
    L.skip_next = false;
    return true;
}
