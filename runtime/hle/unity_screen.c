// (SMRPG) True widescreen in Unity: the game's screen (its backbuffer, its render targets and the aspect ratio of its
// camera and UI) takes the aspect ratio of the player's display, instead of drawing 16:9 with black bars.
//
// Unity decides its resolution in a single function (main 0x70006b7204: 1280x720, or 1920x1080 in TV mode); lasgame.dll
// replaces it (tools/game_overrides.c) with one that gives 1920x1080 and calls in here. With "Widescreen" on and a
// display wider than 16:9 (or taller, like 16:10), the width (or height) grows to its aspect ratio, in multiples of 4
// (with 8, 3440x1440 ended up 2584 wide with two rows of black bars). The renderer scales from that size (g_guest_w /
// g_guest_h: runtime/nvn/output_scale.c, vulkan/textures.c).
// At startup and on the fly: if the window's aspect ratio changes (another display mode, another resolution, the window
// resized by hand), Unity requests its resolution again (rt_unity_screen_follow) and the scene moves to the new one,
// without stretching.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../hle.h"
#include "../settings.h"
#include "../nvn/output_scale.h"
#include "../platform.h"

double renderer_display_aspect(void);  // runtime/nvn/vulkan/device.c

// The game's screen for the one Unity chose (gw x gh) and a display of aspect ratio a: wider (or taller) to match it
static void screen_for(int gw, int gh, double a, int *w, int *h) {
    *w = gw, *h = gh;
    double g = (double)gw / (double)gh;
    if (g_settings.aspect != ASPECT_UNLOCKED || !(a > 0)) return;
    if (a > g * 1.01) {  // wider: same height, more width
        int nw = (int)lround(gh * a / 4.0) * 4;
        if (nw > gw && nw <= 4 * gw) *w = nw;
    } else if (a < g * 0.99) {  // taller (16:10, 4:3): same width, more height
        int nh = (int)lround(gw / a / 4.0) * 4;
        if (nh > gh && nh <= 2 * gh) *h = nh;
    }
}

static int calls, base_w = 1920, base_h = 1080;  // (how many times Unity asked for it; what it chose last)

void rt_unity_screen_size(int32_t *w, int32_t *h) {
    if (!w || !h || *w <= 0 || *h <= 0) return;
    int gw = *w, gh = *h, nw, nh;
    double a = renderer_display_aspect();
    screen_for(gw, gh, a, &nw, &nh);
    *w = nw, *h = nh;
    base_w = gw, base_h = gh;
    g_guest_w = *w;
    g_guest_h = *h;
    calls++;
    {   // (at startup and on every change: not often)
        extern uint64_t renderer_present_count(void);
        LOG("[unity] game screen %dx%d (Unity picked %dx%d; display aspect %.3f, widescreen %s; call %d, frame %llu)\n", *w, *h, gw, gh, a, g_settings.aspect == ASPECT_UNLOCKED ? "on" : "no", calls,
            (unsigned long long)renderer_present_count());
    }
}

// The game's screen as it should be now (with the window's current aspect ratio and the current setting); false if that
// is the one it has.
bool rt_unity_screen_wanted(int *w, int *h) {
    screen_for(base_w, base_h, renderer_display_aspect(), w, h);
    return calls > 0 && (*w != g_guest_w || *h != g_guest_h);
}

// The game's screen follows the window on the fly. When the window changes aspect ratio and settles for a moment, Unity
// is notified of a performance mode change (AM message 31, as when the Switch is taken out of its dock): its handler
// (main 0x700062ed24) requests the resolution again (main 0x70006b7204, replaced: rt_unity_screen_size, which widens
// it) and applies it with its SetResolution (0x700062f1d0). The original clamps it to 1920x1080, the Switch's maximum
// (the scene stayed 1920 wide and came out stretched by 33%, tested on 11-16): lasgame.dll replaces it with one without
// that clamp (tools/game_overrides.c). Unity's window textures follow on their own (t->screen:
// runtime/nvn/vulkan/textures.c). Main thread (device.c ui_run), on every iteration.
static bool follow_failed;
void rt_unity_screen_follow(void) {
    static int pend_w, pend_h, asked_w, asked_h, tries;
    static uint64_t pend_since, asked_at;
    int w, h;
    if (!rt_unity_screen_wanted(&w, &h)) {
        pend_w = pend_h = 0;
        follow_failed = false;
        return;
    }
    uint64_t now = plat_now_ns() / 1000000;
    if (w != pend_w || h != pend_h) {  // (another size: wait for the window to settle)
        pend_w = w, pend_h = h, pend_since = now;
        return;
    }
    extern bool am_message_pending(uint32_t msg);
    if (now - pend_since < 300 || am_message_pending(31)) return;  // (still changing; or not picked up by the game: paused)
    if (asked_w == w && asked_h == h) {
        if (now - asked_at < 2000) return;  // (Unity applies it on its next frame)
        if (tries >= 3) {
            if (!follow_failed) LOG("[unity] the game's screen did not follow the window (%dx%d): it stays "
                                    "letterboxed\n", w, h);
            follow_failed = true;
            return;
        }
    } else tries = 0;
    tries++;
    asked_w = w, asked_h = h, asked_at = now;
    LOG("[unity] the window's aspect changed: asking Unity for a %dx%d screen (now %dx%d)\n", w, h, g_guest_w, g_guest_h);
    extern void am_push_message(uint32_t msg);
    am_push_message(31);
}

// Did Unity fail to follow it? (the menu says so: the picture keeps its black bars until the game is reopened)
bool rt_unity_screen_follow_failed(void) { return follow_failed; }

// The aspect ratio of the game's screen (tools/game_overrides.c: the screen captures used as menu backgrounds)
double rt_unity_screen_aspect(void) { return g_guest_h > 0 ? (double)g_guest_w / (double)g_guest_h : 16.0 / 9.0; }

// Should URP draw its motion vectors? (tools/game_overrides.c, GetRenderPassInputs). AMD FSR needs them (Anti-aliasing:
// AMD FSR, vulkan/vk_fsr.cpp); LARECOMP_FORCE_MV=1 forces them for testing.
int rt_force_motion_vectors(void) {
    static int env = -1;
    if (env < 0) env = getenv("LARECOMP_FORCE_MV") ? atoi(getenv("LARECOMP_FORCE_MV")) : 0;
    extern bool vk_fsr_available(void);
    return env || (g_settings.antialias == AA_FSR && vk_fsr_available());
}

// The UI in widescreen. Unity's CanvasScaler ("scale with screen size" mode) computes each canvas's scale from the
// screen size and its reference resolution; the game is designed for 16:9, and with the "width" setting (the usual one)
// on 21:9 the scale comes from the width: the UI grows and is cut off at the top and bottom (the title screen).
// tools/game_overrides.c replaces CanvasScaler.HandleScaleWithScreenSize and, before computing, passes the size through
// here: if it is that of the widened game screen, it is swapped for the 16:9 that fits inside (what you would see on
// the console). That way each element keeps the size it has at 16:9 and the canvas takes up the whole screen: whatever
// is anchored to the edges goes to the edges. At 16:9 nothing changes.
void rt_unity_ui_scale_screen(float *w, float *h) {
    if (!w || !h || *w <= 0 || *h <= 0) return;
    if (fabsf(*w - (float)g_guest_w) > 1.f || fabsf(*h - (float)g_guest_h) > 1.f) return;  // (another target: the same)
    float vw = *w, vh = *h;
    if (vw * 9.f > vh * 16.f + 8.f) vw = vh * 16.f / 9.f;
    else if (vh * 16.f > vw * 9.f + 8.f) vh = vw * 9.f / 16.f;
    else return;
    static int logged;
    if (!logged++) LOG("[unity] interface: scale computed for %.0fx%.0f (16:9 inside %.0fx%.0f)\n", vw, vh, *w, *h);
    *w = vw;
    *h = vh;
}
