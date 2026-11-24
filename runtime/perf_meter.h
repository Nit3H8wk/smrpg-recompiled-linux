// (SMRPG) The menu's meter (Video > Performance now, and the impact of each setting): what the game uses of each
// component, measured while it runs (runtime/perf_meter.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { PM_CPU, PM_GPU, PM_RAM, PM_SSD, PM_VRAM, PM_N };
enum { PM_OK = 0, PM_MID = 1, PM_HIGH = 2 };  // the traffic light: green, yellow, red

typedef struct {
    bool ok;                 // there are at least two samples (rates need an interval)
    double fps;              // game frames per second
    double cpu_pct;          // the process, as a % of the whole processor (all its logical threads)
    int ncpu;
    bool gpu_known;          // (unknown without a GPU timer)
    double gpu_pct, gpu_ms;  // GPU busy: % of the time (Windows: with AMD's generation) and ms per game frame
    bool gpu_pdh;            // (the % comes from the Windows counters; otherwise, from the renderer's timer)
    double ram_gb, ram_total_gb;      // the process's physical memory (working set) and the machine's
    double io_mbs;                    // MB/s read + written by the process (disk: textures, saves, caches)
    double vram_gb, vram_budget_gb;   // the video memory used by the process and what the driver allows it
    uint64_t vram_targets, vram_hd, vram_tex, vram_fsr, vram_fg;  // bytes: render targets, HD textures, the other
                                                                  // textures, AMD FSR, frame generation
    double px;               // pixels drawn per frame, as a multiple of 1920x1080's
    int lv[PM_N];            // each component's traffic light
} PerfNow;

// The current measurement (a new one every half second; until then, the previous one). Render thread (where the menu is
// drawn).
const PerfNow *perf_meter_now(void);
// The level (PM_OK, PM_MID, PM_HIGH) of a value: green below a, yellow below b, red from b up
static inline int pm_level(double v, double a, double b) { return v < a ? PM_OK : v < b ? PM_MID : PM_HIGH; }

#ifdef __cplusplus
}
#endif
