// (SMRPG) The menu's meter: what the game uses of CPU, GPU, RAM, disk and video memory, measured while it runs. Every
// half second (only when someone asks for it: the menu open, or LARECOMP_LOG_PERF=1 every 2 s in the log):
//  - CPU: the process's CPU time (all its threads) between two samples, as a % of the whole processor;
//  - GPU: the renderer's GPU timer (timestamps on every submit, vulkan/exec.c pace_gpu_ns): the time the GPU spent on
//    the game, as a % of the time and in ms per frame (without AMD's frame generation, which runs on its own queues);
//  - RAM: the process's physical memory (working set);
//  - disk: the bytes read and written by the process, per second;
//  - video memory: what the process uses according to the driver (VK_EXT_memory_budget) and, by kind, that of its
//    textures (vulkan/textures.c vk_vram_by_kind) and that of AMD FSR and its frame generation (what AMD reports).
// The traffic light of each one (green / yellow / red): CPU < 30% / < 60%, GPU < 60% / < 90%, RAM < 25% / < 50% of the
// machine's, disk < 10 / < 100 MB/s, video memory < 60% / < 85% of what the driver allows.
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "perf_meter.h"
#include "platform.h"

int64_t pace_gpu_ns(void);                                  // nvn/vulkan/exec.c (-1: no GPU timer)
uint64_t renderer_present_count(void);                      // nvn/vulkan/device.c
bool vk_vram_budget(uint64_t *usage, uint64_t *budget);     // nvn/vulkan/device.c
void vk_vram_by_kind(uint64_t out[3]);                      // nvn/vulkan/textures.c
uint64_t vk_fsr_vram(void);                                 // nvn/vulkan/vk_fsr.cpp
uint64_t vk_fg_vram(void);                                  // nvn/vulkan/vk_fg.cpp
void renderer_get_scale(float *sx, float *sy);              // nvn/vulkan/textures.c
extern int g_guest_w, g_guest_h;                            // nvn/output_scale.c

static PerfNow now_;
static struct {
    bool have;
    uint64_t t_ns, cpu_ns, io, frames;
    int64_t gpu_ns;
} prev;

static void sample(void) {
    PlatProcStats ps;
    uint64_t t = plat_now_ns(), frames = renderer_present_count();
    int64_t gpu = pace_gpu_ns();
    bool ok = plat_proc_stats(&ps);
    PerfNow n = now_;
    if (ok && prev.have && t > prev.t_ns) {
        double dt = (double)(t - prev.t_ns) / 1e9;
        n.ok = true;
        n.fps = (double)(frames - prev.frames) / dt;
        n.ncpu = ps.ncpu > 0 ? ps.ncpu : 1;
        n.cpu_pct = (double)(ps.cpu_ns - prev.cpu_ns) / 1e9 / dt / n.ncpu * 100.0;
        n.gpu_known = gpu >= 0 && prev.gpu_ns >= 0;
        if (n.gpu_known) {
            double g = (double)(gpu - prev.gpu_ns) / 1e9;
            n.gpu_pct = g / dt * 100.0;
            n.gpu_ms = frames > prev.frames ? g * 1000.0 / (double)(frames - prev.frames) : 0;
        }
        n.io_mbs = (double)(ps.io_read + ps.io_write - prev.io) / 1e6 / dt;
    }
    if (ok) {
        n.ram_gb = ps.ram_used / 1073741824.0;
        n.ram_total_gb = ps.ram_total / 1073741824.0;
    }
    uint64_t use, bud, kinds[3];
    if (vk_vram_budget(&use, &bud)) n.vram_gb = use / 1073741824.0, n.vram_budget_gb = bud / 1073741824.0;
    vk_vram_by_kind(kinds);
    n.vram_targets = kinds[0], n.vram_hd = kinds[1], n.vram_tex = kinds[2];
    n.vram_fsr = vk_fsr_vram(), n.vram_fg = vk_fg_vram();
    float sx = 1, sy = 1;
    renderer_get_scale(&sx, &sy);
    n.px = (double)g_guest_w * sx * (double)g_guest_h * sy / (1920.0 * 1080.0);
    {   // (the whole GPU according to Windows, AMD's frame generation included; otherwise, the renderer's timer)
        static uint64_t pdh_at;
        static double pdh_pct = -1;
        double v;
        if (!pdh_at || t - pdh_at >= 1000000000ull) {  // (the Windows counters: once per second)
            pdh_at = t;
            if (plat_gpu_usage(&v)) pdh_pct = v;
        }
        n.gpu_pdh = pdh_pct >= 0;
        if (n.gpu_pdh) n.gpu_pct = pdh_pct, n.gpu_known = true;
    }
    n.lv[PM_CPU] = pm_level(n.cpu_pct, 30, 60);
    n.lv[PM_GPU] = n.gpu_known ? pm_level(n.gpu_pct, 60, 90) : PM_OK;
    n.lv[PM_RAM] = n.ram_total_gb > 0 ? pm_level(n.ram_gb / n.ram_total_gb, 0.25, 0.5) : PM_OK;
    n.lv[PM_SSD] = pm_level(n.io_mbs, 10, 100);
    n.lv[PM_VRAM] = n.vram_budget_gb > 0 ? pm_level(n.vram_gb / n.vram_budget_gb, 0.6, 0.85) : PM_OK;
    now_ = n;
    prev.have = ok, prev.t_ns = t, prev.cpu_ns = ps.cpu_ns, prev.io = ps.io_read + ps.io_write, prev.frames = frames;
    prev.gpu_ns = gpu;
}

const PerfNow *perf_meter_now(void) {
    uint64_t t = plat_now_ns();
    if (!prev.have || t - prev.t_ns >= 500000000ull) sample();
    return &now_;
}

// LARECOMP_LOG_PERF=1 (tests: the impact of each setting): one [perf] line every 2 s. From the present
// (vulkan/device.c), on the render thread.
void perf_meter_log_tick(void) {
    static int on = -1;
    static uint64_t last;
    if (on < 0) on = getenv("LARECOMP_LOG_PERF") && atoi(getenv("LARECOMP_LOG_PERF")) != 0;
    if (!on) return;
    uint64_t t = plat_now_ns();
    if (last && t - last < 2000000000ull) return;
    last = t;
    const PerfNow *n = perf_meter_now();
    if (!n->ok) return;
    LOG("[perf] frame %llu %.1f fps cpu %.1f%% gpu %.1f%% %.2f ms/frame ram %.2f GB disk %.1f MB/s vram %.2f/%.2f GB "
        "(targets %.0f MB, HD %.0f MB, textures %.0f MB, fsr %.0f MB, fg %.0f MB) px %.2f\n",
        (unsigned long long)renderer_present_count(), n->fps, n->cpu_pct, n->gpu_pct, n->gpu_ms, n->ram_gb, n->io_mbs,
        n->vram_gb, n->vram_budget_gb, n->vram_targets / 1048576.0, n->vram_hd / 1048576.0, n->vram_tex / 1048576.0,
        n->vram_fsr / 1048576.0, n->vram_fg / 1048576.0, n->px);
}
