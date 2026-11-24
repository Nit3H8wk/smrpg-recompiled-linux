// In-game overlay menu built with Dear ImGui (Metal renderer on macOS, Vulkan elsewhere; input fed from SDL events).
//
// Threads: SDL events arrive on the main thread and are queued; the menu is laid out and drawn on the
// render thread inside backend_present, after the game image, into the same drawable.
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif
#include "../app_name.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <time.h>
#include <atomic>
#include <string>
#include <vector>

#include "imgui.h"
#include "option_rows.h"
#include "tr.h"
#include "imgui_internal.h"  // (NavCursorVisible)
#include "osk.h"
#include "pad_nav.h"
#ifdef __APPLE__
#include "imgui_impl_metal.h"
#else
#include "imgui_impl_vulkan.h"
extern "C" bool vk_imgui_backend_init(void);  // runtime/nvn/vulkan/imgui_glue.cpp
extern "C" const char *vk_fsr_status(void);  // (SMRPG) runtime/nvn/vulkan/vk_fsr.cpp
extern "C" const char *vk_fg_status(void);   // (SMRPG) runtime/nvn/vulkan/vk_fg.cpp
extern "C" bool vk_fg_generating(void);
extern "C" bool vk_lsfg_wanted(void);       // (SMRPG) runtime/nvn/vulkan/vk_lsfg.c
extern "C" const char *vk_lsfg_status(void);
extern "C" double vk_lsfg_gpu_ms(void);
extern "C" uint64_t vk_lsfg_vram(int *measured);
extern "C" bool rt_unity_screen_wanted(int *w, int *h);  // (SMRPG) runtime/hle/unity_screen.c
extern "C" bool rt_unity_screen_follow_failed(void);  // (SMRPG) runtime/hle/unity_screen.c
extern "C" void input_apply_background(void);  // runtime/hle/input.c (controllers in the background)
// runtime/nvn/vulkan/device.c: the command buffer ImGui records into (a secondary one the recorder thread executes in
// the window image's commands, or that buffer itself after the recorder caught up), then handed back
extern "C" void *vk_overlay_begin(void *window_cmd);
extern "C" void vk_overlay_end(void *window_cmd, void *cmd);
#endif

extern "C" {
#include "../hle.h"
#include "../settings.h"
#include "menu.h"
#include "../perf_meter.h"
#include "../platform.h"
void input_reset(void);
void save_editor_draw(float scale);  // save_editor.mm
extern "C" int game_in_play(void);  // hle/warp.c
void save_editor_on_open(void);
void rando_tab_draw(float scale);
bool warp_tab_draw(float scale);  // warp_tab.mm: true when a warp started
extern "C" void cheats_draw(float scale);  // cheats.mm
extern "C" void difficulty_draw(float scale);  // difficulty.mm
void controls_tab_draw(float scale);  // controls_ui.mm
bool controls_ui_took_back(void);
bool rando_tab_took_back(void);  // rando_tab.mm: B went back from the Triforce hunt's count
#include "../input_bind.h"
#include "../paths.h"
}

// ------------------------------------------------------------------ state shared with the main thread
static std::atomic<bool> g_open{false};
static std::atomic<bool> g_text_busy{false};  // a text field / the on-screen keyboard has the keys (render thread: ui_text_busy)
static std::atomic<bool> g_want_fullscreen_apply{false};  // (SMRPG: the display mode and the display resolution)
static std::atomic<int> g_last_full_mode{WIN_BORDERLESS};  // (F11 goes back to the last one that was not windowed)

// (SMRPG) The window's monitor, read on the main thread (SDL requires it) for the menu: its fullscreen modes (one per
// size, with its highest refresh rate), the desktop, the usable area for a window and the window's size
struct DisplayInfo {
    int n;
    int w[64], h[64];
    float hz[64];
    int desk_w, desk_h;
    float desk_hz;
    int usable_w, usable_h;
    int win_w, win_h;
};
static pthread_mutex_t g_disp_lock = PTHREAD_MUTEX_INITIALIZER;
static DisplayInfo g_disp;
extern "C" void menu_request_window_apply(void) { g_want_fullscreen_apply.store(true); }
static void refresh_display_info(SDL_Window *w);
extern "C" void menu_apply_window_mode_now(void *win);

struct QEvent {
    enum { KEY, ANALOG, MOUSE_POS, MOUSE_BUTTON, WHEEL, TEXT } kind;
    ImGuiKey key;
    bool down;
    float value, x, y;
    int button;
    char text[32];
};
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static std::vector<QEvent> q_events;
static float g_pixel_density = 1;

static void push(const QEvent &e) {
    pthread_mutex_lock(&q_lock);
    q_events.push_back(e);
    pthread_mutex_unlock(&q_lock);
}

bool menu_is_open(void) { return g_open.load(); }
static void set_open(bool open);
extern "C" void menu_test_open(void) {
    set_open(true);
    if (getenv("LARECOMP_MENU_TAB")) { extern int g_tab_request_ext; g_tab_request_ext = atoi(getenv("LARECOMP_MENU_TAB")); }
}

void menu_tabs_focus(bool first_tab);
static Settings g_settings_at_open;  // (SMRPG) the settings when the menu opened: "Undo changes"
static void set_open(bool open) {
    if (open == g_open.load()) return;
    g_open.store(open);
    input_reset();  // nothing held while the menu had focus leaks into the game
    bind_capture_cancel();
    if (open) {
        g_settings_at_open = g_settings;
        save_editor_on_open();
        static bool opened;  // the controller's cursor: the first tab (Controls) the first time this session, later the
        menu_tabs_focus(!opened);  // selected one
        opened = true;
    }
    else { settings_save(); osk_cancel(); g_text_busy = false; }
}

static ImGuiKey key_from_scancode(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_LGUI: return ImGuiKey_LeftSuper;
    case SDL_SCANCODE_RGUI: return ImGuiKey_RightSuper;
    case SDL_SCANCODE_A: return ImGuiKey_A;
    case SDL_SCANCODE_C: return ImGuiKey_C;
    case SDL_SCANCODE_V: return ImGuiKey_V;
    case SDL_SCANCODE_X: return ImGuiKey_X;
    case SDL_SCANCODE_Z: return ImGuiKey_Z;
    case SDL_SCANCODE_Q: return ImGuiKey_Q;  // previous tab
    case SDL_SCANCODE_E: return ImGuiKey_E;  // next tab
    default: return ImGuiKey_None;
    }
}


// ---- software keyboard (runtime/hle/swkbd.c: the game's name entry etc.) --------------------------------------
// The game thread waits in menu_keyboard_run (re-presenting the last frame); this dialog takes the text: typed
// directly when the player last used a keyboard, an on-screen keyboard (d-pad + confirm, back deletes, Start / + OK; buttons as in pad_nav.h) when
// they last used a controller.
static std::atomic<int> g_kbd{0};  // 1 open, 2 confirmed, 3 cancelled
static std::atomic<bool> g_last_pad{false};
static char g_kbd_text[96];
static int g_kbd_max = 8;
static bool capturing() { return g_open.load() || g_kbd.load() == 1; }
// The game's name limit counts characters as the console's keyboard does, in UTF-16 units (a character past U+FFFF
// takes two); the text is UTF-8. Cuts s after max_units of them, only between characters (a character left incomplete
// - a cut inside it, or bytes that are no character - goes too, with the rest), and returns how many units are left.
static int kbd_cut(char *s, int max_units) {
    int units = 0;
    for (unsigned char *p = (unsigned char *)s; *p;) {
        int n = *p < 0x80 ? 1 : *p >= 0xc2 && *p <= 0xdf ? 2 : *p >= 0xe0 && *p <= 0xef ? 3 : *p >= 0xf0 && *p <= 0xf4 ? 4 : 0;
        int i = 1;
        while (i < n && (p[i] & 0xc0) == 0x80) i++;
        if (i == n && n > 2 && ((*p == 0xe0 && p[1] < 0xa0) || (*p == 0xed && p[1] > 0x9f) || (*p == 0xf0 && p[1] < 0x90) ||
                                (*p == 0xf4 && p[1] > 0x8f)))
            n = 0;  // (overlong, a surrogate or past U+10FFFF: the game's UTF-8 conversion refuses those too)
        int u = n == 4 ? 2 : 1;
        if (!n || i < n || units + u > max_units) { *p = 0; break; }
        units += u, p += n;
    }
    return units;
}
// The game gets no input while this is true (runtime/hle/input.c): menu or name entry up.
extern "C" bool menu_input_captured(void) { return capturing(); }

bool menu_handle_event(const SDL_Event *e) {
    static bool back_down, start_down;
    if (g_open.load() && bind_capture_event(e)) return true;  // Controls tab waiting for a new binding: it takes the press
    if (e->type == SDL_EVENT_KEY_DOWN) g_last_pad = false;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) g_last_pad = true;
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {  // Enter confirms, Esc cancels
        if (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) { g_kbd = 2; return true; }
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { g_kbd = 3; return true; }
    }
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_START) {
        g_kbd = 2;
        return true;
    }
    // typing (a text field, the on-screen keyboard, the name entry): keys are text, not menu shortcuts
    bool typing = g_kbd.load() == 1 || (g_open.load() && g_text_busy.load());
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F1 ||
                               (e->key.scancode == SDL_SCANCODE_ESCAPE && !g_open.load()))) {
            if (!typing) set_open(!g_open.load());  // (Esc with the menu open goes to ImGui below: it cancels a field)
            return true;
        }
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F11 ||
                               (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))) ||
                               (e->key.scancode == SDL_SCANCODE_F && (e->key.mod & SDL_KMOD_GUI) && (e->key.mod & SDL_KMOD_CTRL)))) {
            // (SMRPG) between windowed and the last mode that was not (borderless, to begin with)
            if (g_settings.window_mode == WIN_WINDOW) g_settings.window_mode = g_last_full_mode.load();
            else g_last_full_mode.store(g_settings.window_mode), g_settings.window_mode = WIN_WINDOW;
            g_settings.fullscreen = g_settings.window_mode != WIN_WINDOW;
            g_want_fullscreen_apply.store(true);
            settings_save();
            return true;
        }
        /* fallthrough */
    case SDL_EVENT_KEY_UP: {
        if (!capturing()) return false;
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        if (down && e->key.scancode == SDL_SCANCODE_ESCAPE) {  // Esc: back out of popups, else close
            QEvent q = {};
            q.kind = QEvent::KEY; q.key = ImGuiKey_Escape; q.down = true; push(q);
            q.down = false; push(q);
            return true;
        }
        ImGuiKey k = key_from_scancode(e->key.scancode);
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::TEXT; strlcpy(q.text, e->text.text, sizeof q.text); push(q); }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) back_down = down;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_START) start_down = down;
        if (down && !typing && ((back_down && start_down) || e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)) {
            set_open(!g_open.load());
            return true;
        }
        if (!capturing()) return false;
        ImGuiKey k = ui_pad_key(e->gbutton.which, e->gbutton.button, down);  // (confirm / back: pad_nav.h)
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (!capturing()) return false;
        float v = e->gaxis.value / 32767.0f;
        QEvent q = {};
        q.kind = QEvent::ANALOG;
        switch (e->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            q.key = ImGuiKey_GamepadLStickLeft; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickRight; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            q.key = ImGuiKey_GamepadLStickUp; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickDown; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: q.key = ImGuiKey_GamepadL2; q.value = v; push(q); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: q.key = ImGuiKey_GamepadR2; q.value = v; push(q); break;
        default: break;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::MOUSE_POS; q.x = e->motion.x * g_pixel_density; q.y = e->motion.y * g_pixel_density; push(q); }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!capturing()) return false;
        {
            QEvent q = {};
            q.kind = QEvent::MOUSE_BUTTON;
            q.button = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            q.down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            push(q);
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::WHEEL; q.x = e->wheel.x; q.y = e->wheel.y; push(q); }
        return true;
    default:
        return false;
    }
}

extern "C" uint32_t g_window_resize_request;  // (test scripts: runtime/hle/input.c "resize")
void menu_apply_window_requests(void *win) {
    SDL_Window *w = (SDL_Window *)win;
    g_pixel_density = SDL_GetWindowPixelDensity(w);
    static bool text_input;
    bool want_text = capturing();
    if (want_text != text_input) {
        if (want_text) SDL_StartTextInput(w);
        else SDL_StopTextInput(w);
        text_input = want_text;
    }
    static bool first = true;
    if (first || g_want_fullscreen_apply.exchange(false)) {
        first = false;
        menu_apply_window_mode_now(w);
    }
    if (uint32_t r = __atomic_exchange_n(&g_window_resize_request, 0u, __ATOMIC_RELAXED))
        SDL_SetWindowSize(w, (int)(r >> 16), (int)(r & 0xffff));
    static uint64_t next_info;  // (the monitor, for the menu: every second; the mode also reads it when applied)
    uint64_t now = SDL_GetTicks();
    if (now >= next_info) {
        next_info = now + 1000;
        refresh_display_info(w);
    }
}

// (SMRPG) The window's monitor (main thread)
static void refresh_display_info(SDL_Window *w) {
    SDL_DisplayID d = SDL_GetDisplayForWindow(w);
    if (!d) d = SDL_GetPrimaryDisplay();
    DisplayInfo di = {};
    int count = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(d, &count);
    for (int i = 0; modes && i < count && di.n < 64; i++) {  // (SDL gives them from largest to smallest)
        const SDL_DisplayMode *m = modes[i];
        float pd = m->pixel_density > 0 ? m->pixel_density : 1;
        int mw = (int)lroundf(m->w * pd), mh = (int)lroundf(m->h * pd);
        int k = 0;
        while (k < di.n && !(di.w[k] == mw && di.h[k] == mh)) k++;
        if (k < di.n) {
            if (m->refresh_rate > di.hz[k]) di.hz[k] = m->refresh_rate;
        } else {
            di.w[di.n] = mw, di.h[di.n] = mh, di.hz[di.n] = m->refresh_rate, di.n++;
        }
    }
    SDL_free(modes);
    if (const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(d)) {
        float pd = dm->pixel_density > 0 ? dm->pixel_density : 1;
        di.desk_w = (int)lroundf(dm->w * pd), di.desk_h = (int)lroundf(dm->h * pd), di.desk_hz = dm->refresh_rate;
    }
    SDL_Rect u;
    if (SDL_GetDisplayUsableBounds(d, &u)) di.usable_w = u.w, di.usable_h = u.h;
    SDL_GetWindowSizeInPixels(w, &di.win_w, &di.win_h);
    static bool logged;
    if (!logged && di.n) {  // (to the log, once: what the monitor offers)
        logged = true;
        char list[1024];
        int k = 0;
        for (int i = 0; i < di.n && k < (int)sizeof list - 32; i++)
            k += snprintf(list + k, sizeof list - k, "%s%dx%d@%.0f", i ? ", " : "", di.w[i], di.h[i], di.hz[i]);
        LOG("[window] monitor %dx%d@%.0f (usable area %dx%d); modes: %s\n", di.desk_w, di.desk_h, di.desk_hz, di.usable_w,
            di.usable_h, list);
    }
    pthread_mutex_lock(&g_disp_lock);
    g_disp = di;
    pthread_mutex_unlock(&g_disp_lock);
}

// (SMRPG) The display mode (main thread; device.c ui_window_init applies it when creating the window, before the game
// asks for its resolution). Windowed: with a frame, at the "Display resolution" size (in pixels; centered). Borderless:
// a frameless window with the monitor's size and position, not fullscreen (instant Alt+Tab). Fullscreen: exclusive, in
// the monitor mode closest to "Display resolution" (or the desktop's). The test window (LARECOMP_WINDOW /
// LARECOMP_HIDDEN) does not change, except with LARECOMP_WINDOW_MODES=1.
extern "C" void menu_apply_window_mode_now(void *win) {
    SDL_Window *w = (SDL_Window *)win;
    if ((getenv("LARECOMP_WINDOW") || getenv("LARECOMP_HIDDEN")) && !getenv("LARECOMP_WINDOW_MODES")) return;
    static int prev = -1;
    SDL_DisplayID d = SDL_GetDisplayForWindow(w);
    if (!d) d = SDL_GetPrimaryDisplay();
    int mode = g_settings.window_mode;
    if (mode == WIN_FULLSCREEN) {
        SDL_DisplayMode m;
        const SDL_DisplayMode *want = NULL;
        if (g_settings.out_w > 0 && g_settings.out_h > 0 &&
            SDL_GetClosestFullscreenDisplayMode(d, g_settings.out_w, g_settings.out_h, 0.0f, false, &m))
            want = &m;
        else
            want = SDL_GetDesktopDisplayMode(d);
        SDL_SetWindowBordered(w, true);
        SDL_SetWindowFullscreenMode(w, want);
        SDL_SetWindowFullscreen(w, true);
    } else {
        if (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) {
            SDL_SetWindowFullscreen(w, false);
            SDL_SyncWindow(w);
        }
        SDL_SetWindowFullscreenMode(w, NULL);  // (the earlier exclusive mode, forgotten)
        SDL_Rect r = {0, 0, 0, 0};
        SDL_GetDisplayBounds(d, &r);
        if (mode == WIN_BORDERLESS) {
            SDL_SetWindowBordered(w, false);
            SDL_SetWindowPosition(w, r.x, r.y);
            SDL_SetWindowSize(w, r.w, r.h);
        } else {
            SDL_SetWindowBordered(w, true);
            int ww = g_settings.out_w, wh = g_settings.out_h;
            if ((ww <= 0 || wh <= 0) && prev != WIN_WINDOW && prev != -1) ww = 1280, wh = 720;  // (leaving the others)
            if (ww > 0 && wh > 0) {
                float dens = SDL_GetWindowPixelDensity(w);
                if (!(dens > 0)) dens = 1;
                int pw = (int)lroundf(ww / dens), ph = (int)lroundf(wh / dens);
                SDL_Rect u;
                if (SDL_GetDisplayUsableBounds(d, &u)) {  // (with the frame inside the usable area)
                    if (pw > u.w) pw = u.w;
                    if (ph > u.h - 40) ph = u.h - 40;
                }
                SDL_SetWindowSize(w, pw, ph);
                SDL_SetWindowPosition(w, (int)SDL_WINDOWPOS_CENTERED_DISPLAY(d), (int)SDL_WINDOWPOS_CENTERED_DISPLAY(d));
            }
        }
    }
    SDL_SyncWindow(w);
    prev = mode;
    int px = 0, py = 0, pw = 0, ph = 0, x = 0, y = 0;
    SDL_GetWindowSizeInPixels(w, &pw, &ph);
    SDL_GetWindowPosition(w, &x, &y);
    SDL_GetWindowSize(w, &px, &py);
    SDL_WindowFlags f = SDL_GetWindowFlags(w);
    const SDL_DisplayMode *fm = SDL_GetWindowFullscreenMode(w);
    LOG("[window] %s: %dx%d pixels (%dx%d at %d,%d), %s%s%s\n",
        mode == WIN_FULLSCREEN ? "full screen" : mode == WIN_BORDERLESS ? "borderless" : "window", pw, ph, px, py, x, y,
        (f & SDL_WINDOW_FULLSCREEN) ? "full screen" : "not full screen", (f & SDL_WINDOW_BORDERLESS) ? ", no frame" : "",
        fm && (f & SDL_WINDOW_FULLSCREEN) ? " (exclusive mode)" : "");
    refresh_display_info(w);
}

// ------------------------------------------------------------------ render thread
static bool g_inited;
static ImGuiContext *g_ctx;  // ours: the start screen (back to it after a reboot) has its own context
static ImFont *g_font, *g_font_big;
static ImFont *g_menu_font, *g_menu_font_big;  // the menu window (Baloo 2); everything else keeps g_font
static int g_tab, g_tab_request = -2;  // (-2: no request; -1 is the Prelaunch tab)
static int g_focus_tab = -99;  // put the nav cursor on this tab next frame (-100: the selected one; -99: none)
static int g_focus_frames;

// The start screen's Settings opening: the controller's cursor starts on the first tab the first time in a session,
// later on the tab that's selected
static bool g_page_fresh_seen;  // (focus_check: the menu opened)
static bool g_page_fresh;  // the page shown hasn't been entered yet: the first Down goes to its first enabled row
static bool g_page_focus_next;  // (that page has no option rows: its first control takes the cursor next frame)
static int g_shown_tab = -1000;  // the page drawn last (its id)
void menu_tabs_focus(bool first_tab) {
    g_focus_tab = first_tab ? -1 : -100, g_focus_frames = 0, g_page_fresh = g_page_fresh_seen = true;
}
int g_tab_request_ext = -1;

static void style_init(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 14; s.FrameRounding = 8; s.GrabRounding = 8; s.TabRounding = 8; s.ChildRounding = 10;
    s.WindowPadding = ImVec2(28, 24); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 0; s.ScrollbarSize = 18;
    ImVec4 *c = s.Colors;
    ImVec4 gold(0.93f, 0.76f, 0.33f, 1), gold_dim(0.62f, 0.49f, 0.20f, 1), green(0.15f, 0.36f, 0.25f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.09f, 0.08f, 0.94f);
    c[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.13f, 0.11f, 0.85f);
    c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.10f, 0.09f, 0.98f);
    c[ImGuiCol_Text] = ImVec4(0.96f, 0.94f, 0.88f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.64f, 0.58f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.19f, 0.16f, 1);
    c[ImGuiCol_FrameBgHovered] = green;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.19f, 0.45f, 0.31f, 1);
    c[ImGuiCol_Button] = ImVec4(0.14f, 0.22f, 0.18f, 1);
    c[ImGuiCol_ButtonHovered] = green;
    c[ImGuiCol_ButtonActive] = ImVec4(0.21f, 0.50f, 0.34f, 1);
    c[ImGuiCol_Header] = green;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.19f, 0.45f, 0.31f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.21f, 0.50f, 0.34f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.12f, 0.18f, 0.15f, 1);
    c[ImGuiCol_TabHovered] = green;
    c[ImGuiCol_TabSelected] = ImVec4(0.21f, 0.44f, 0.31f, 1);
    c[ImGuiCol_TabSelectedOverline] = gold;
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_SliderGrab] = gold_dim;
    c[ImGuiCol_SliderGrabActive] = gold;
    c[ImGuiCol_NavCursor] = gold;
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.36f, 0.30f, 1);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
}


#ifdef __APPLE__
static void init(id<MTLDevice> dev) {
#else
static void init(void) {
#endif
    IMGUI_CHECKVERSION();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.IniFilename = NULL;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    char dejavu[1024];  // none of the system's: the bundled DejaVu Sans (third_party/fonts/DejaVu, in every release)
    const char *bundled = rt_resource("DejaVuSans.ttf", "third_party/fonts/DejaVu/DejaVuSans.ttf", dejavu, sizeof dejavu) ? dejavu : NULL;
    const char *paths[] = {"/System/Library/Fonts/Avenir Next.ttc", "/System/Library/Fonts/Helvetica.ttc",
                           "C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", bundled};
    for (const char *p : paths) {
        if (!p) continue;
        if (FILE *f = fopen(p, "rb")) fclose(f);  // a missing file is an assertion in ImGui, not a NULL return
        else continue;
        if (!g_font) g_font = io.Fonts->AddFontFromFileTTF(p, 34, &cfg);
        if (g_font && !g_font_big) { ImFontConfig b = cfg; b.FontNo = 0; g_font_big = io.Fonts->AddFontFromFileTTF(p, 56, &b); }
    }
    // the menu window itself: Baloo 2 SemiBold, as on the start screen (shipped, OFL), at its Settings window's sizes; the
    // FPS counter, overlays and on-screen keyboard keep the fonts above
    char baloo[1024];
    if (const char *f = getenv("LARECOMP_MENU_FONT")) strlcpy(baloo, f, sizeof baloo);  // (test) another weight / face
    if (getenv("LARECOMP_MENU_FONT") || rt_resource("Baloo2-SemiBold.ttf", "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", baloo, sizeof baloo)) {
        g_menu_font = io.Fonts->AddFontFromFileTTF(baloo, 39, &cfg);
        g_menu_font_big = io.Fonts->AddFontFromFileTTF(baloo, 72, &cfg);
    }
    if (!g_font) g_font = io.Fonts->AddFontDefault();
    if (!g_font_big) g_font_big = g_font;
    if (!g_menu_font || !g_menu_font_big) g_menu_font = g_font, g_menu_font_big = g_font_big;
    style_init();
#ifdef __APPLE__
    ImGui_ImplMetal_Init(dev);
#else
    vk_imgui_backend_init();
#endif
    g_inited = true;
}

static void drain_events(ImGuiIO &io) {
    pthread_mutex_lock(&q_lock);
    std::vector<QEvent> ev;
    ev.swap(q_events);
    pthread_mutex_unlock(&q_lock);
    for (const QEvent &q : ev) {
        switch (q.kind) {
        case QEvent::KEY: io.AddKeyEvent(q.key, q.down); break;
        case QEvent::ANALOG: io.AddKeyAnalogEvent(q.key, q.value > 0.1f, q.value); break;
        case QEvent::MOUSE_POS: io.AddMousePosEvent(q.x, q.y); break;
        case QEvent::MOUSE_BUTTON: io.AddMouseButtonEvent(q.button, q.down); break;
        case QEvent::WHEEL: io.AddMouseWheelEvent(q.x, q.y); break;
        case QEvent::TEXT: io.AddInputCharactersUTF8(q.text); break;
        }
    }
}

extern "C" void renderer_get_scale(float *sx, float *sy);
extern "C" int input_pad_family(void);  // runtime/hle/input.c
extern "C" double output_scale_dynamic_factor(void);

extern "C" bool g_game_las;  // runtime/game_addresses.c (false en SMRPG)
extern "C" int g_guest_w, g_guest_h;  // runtime/nvn/output_scale.c: the game's screen (1920x1080 or wider)
extern "C" bool renderer_hdr_state(bool *offered);  // runtime/nvn/vulkan/device.c: HDR10 active now

// ---------------------------------------------------------------- (SMRPG) the impact of each graphics setting
// Under its name, each graphics setting shows its impact on CPU, GPU, RAM, disk (SSD) and video memory at its current
// value, in traffic-light colors, and the reason in its description. The basis is what the game measures while it runs
// (runtime/perf_meter.c: the video memory of each kind, exact; the GPU time per frame; the pixels drawn) and, for what
// cannot be told apart at run time, what was measured on the test PC on 2026-11-16 (Radeon RX 9070 XT, 3440x1440
// window, 60 FPS, the same scene): AMD FSR 1.4 ms per frame, SMAA 0.4, 16x anisotropic 0.4, sharpness 0.2, frame
// generation 2.1 ms per generated frame (Windows counters: +25% GPU with the game at 120), texture detail nothing
// measurable; the CPU at 60 / 120 / 240 FPS, 3 / 5.5 / 9.5% of the processor, and +60% with frame generation; HD
// textures (43) and dumping, little disk. Those times are scaled by the current pixels (measured at 2.39 times 1080p).
// GPU: its cost against the time of one frame at the FPS limit (green < 15%, yellow < 40%, red from 40%).
// Video memory: against what the driver allows the game (green < 4%, yellow < 12%, red from 12%).
static const PerfNow *g_pm;
static double frame_budget_ms(void) { return 1000.0 / (g_settings.fps_limit > 0 ? g_settings.fps_limit : 60); }
static signed char gpu_lv(double ms) { return (signed char)pm_level(ms / frame_budget_ms(), 0.15, 0.40); }
static signed char vram_lv(double bytes) {
    double b = g_pm->vram_budget_gb * 1073741824.0;
    return (signed char)(b > 0 ? pm_level(bytes / b, 0.04, 0.12) : PM_OK);
}
static const char *num(double v, int dec) {  // with a decimal comma in Spanish (a few buffers: several on one line)
    static char buf[8][32];
    static int bi;
    char *b = buf[bi++ & 7];
    snprintf(b, sizeof buf[0], "%.*f", dec, v);
    if (ui_spanish())
        for (char *c = b; *c; c++) if (*c == '.') *c = ',';
    return b;
}
static const char *mb(double bytes) {
    return bytes >= 1073741824.0 ? num(bytes / 1073741824.0, 2) : num(bytes / 1048576.0, 0);
}
static const char *mem(double bytes) {  // "350 MB" / "1,96 GB"
    static char buf[8][32];
    static int bi;
    char *b = buf[bi++ & 7];
    snprintf(b, sizeof buf[0], "%s %s", mb(bytes), bytes >= 1073741824.0 ? "GB" : "MB");
    return b;
}
struct Impact {
    signed char lv[PM_N] = {0, 0, 0, 0, 0};
    char val[PM_N][24] = {"", "", "", "", ""};  // what it uses of each one, short ("2.5 ms", "501 MB"; empty: nothing)
    char why[900] = "";
    void set(int c, const char *fmt, ...) __attribute__((format(printf, 3, 4))) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(val[c], sizeof val[c], tr(fmt), ap);
        va_end(ap);
    }
    void add(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        size_t k = strlen(why);
        if (k && k + 2 < sizeof why) why[k++] = '\n', why[k] = 0;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(why + k, sizeof why - k, tr(fmt), ap);
        va_end(ap);
    }
};
enum { IM_NONE, IM_OUTRES, IM_FPS, IM_WIDE, IM_RES, IM_AA, IM_FSRMODE, IM_FSRVER, IM_FG, IM_FGVER, IM_SS, IM_SHARP,
       IM_ANISO, IM_SHADERS, IM_TEXDETAIL, IM_TEXHD, IM_TEXDUMP, IM_HDR, IM_LSFG };
static void impact_for(int which, Impact &im) {
    const PerfNow *n = g_pm;
    double P = n->px > 0 ? n->px : 1, k = P / 2.39, B = frame_budget_ms();
    double Po = 1;  // (the output: the window, as a multiple of 1080p)
    {
        DisplayInfo di;
        pthread_mutex_lock(&g_disp_lock);
        di = g_disp;
        pthread_mutex_unlock(&g_disp_lock);
        if (di.win_w > 0 && di.win_h > 0) Po = (double)di.win_w * di.win_h / (1920.0 * 1080.0);
    }
    double ko = Po / 2.39;
    int f = g_settings.fps_limit > 0 ? g_settings.fps_limit : 60;
    switch (which) {
    case IM_NONE:
        im.add("No noticeable effect on any component.");
        break;
    case IM_TEXDETAIL:
        im.add("Measured: no noticeable change on any component.");
        break;
    case IM_OUTRES:
        if (g_settings.res_mode != RES_MATCH) {
            im.add("With a fixed internal resolution only the size of the final image changes: little.");
            break;
        }
        im.add("With the internal resolution set to \"Match display\", this one decides how much is drawn:");
        /* fallthrough */
    case IM_RES:
    case IM_FSRMODE: {
        double ms = P > 1 ? n->gpu_ms * (P - 1) / P : 0;
        double vb = (double)n->vram_targets + (which == IM_FSRMODE ? (double)n->vram_fsr : 0);
        im.lv[PM_GPU] = gpu_lv(ms), im.lv[PM_VRAM] = vram_lv(vb);
        im.set(PM_GPU, "%s ms", num(ms, 1)), im.set(PM_VRAM, "%s", mem(vb));
        im.add("GPU: draws %s times the pixels of 1080p: about %s ms per frame (%s %% of its time at %d FPS).",
               num(P, 2), num(ms, 1), num(ms / B * 100, 0), f);
        im.add("Video memory: %s in render targets at that resolution (measured now)%s.", mem(n->vram_targets),
               which == IM_FSRMODE && n->vram_fsr ? tr(", plus AMD FSR's own") : "");
        im.add("(Measured on the test PC, with an RX 9070 XT: 4K, 7.6 ms and 0.5 GB; 8K, 13.7 ms and 2 GB.)");
        break;
    }
    case IM_FPS: {
        double load = n->gpu_known ? n->gpu_ms * f / 1000.0 : 0;
        im.lv[PM_CPU] = (signed char)(f <= 60 ? PM_OK : f <= 144 ? PM_MID : PM_HIGH);
        im.lv[PM_GPU] = (signed char)pm_level(load, 0.6, 0.9);
        im.set(PM_CPU, "x%s", num(f / 60.0, f % 60 ? 1 : 0)), im.set(PM_GPU, "%s %%", num(load * 100, 0));
        im.add("CPU: the game's work grows with the frame rate (measured: 3%% of the processor at 60, 5.5%% at 120, "
               "9.5%% at 240).");
        im.add("GPU: %s ms per frame x %d = %s%% of the GPU.", num(n->gpu_ms, 1), f, num(load * 100, 0));
        break;
    }
    case IM_WIDE: {
        double a = g_guest_h > 0 ? (double)g_guest_w / g_guest_h : 16.0 / 9, extra = a > 1.79 ? 1 - (16.0 / 9) / a : 0;
        if (g_settings.aspect != ASPECT_UNLOCKED || extra <= 0) {
            im.add("On the current display, nothing (it is 16:9).");
            break;
        }
        double ms = n->gpu_ms * extra, vb = n->vram_targets * extra;
        im.lv[PM_GPU] = gpu_lv(ms), im.lv[PM_VRAM] = vram_lv(vb);
        im.set(PM_GPU, "+%s ms", num(ms, 1)), im.set(PM_VRAM, "+%s", mem(vb));
        im.add("GPU: %s%% more pixels per frame, about %s ms.", num(extra / (1 - extra) * 100, 0), num(ms, 1));
        im.add("Video memory: about %s more in render targets.", mem(vb));
        break;
    }
    case IM_AA:
    case IM_FSRVER: {
        int aa = g_settings.antialias;
        if (aa == AA_OFF) {
            im.add("No anti-aliasing: nothing.");
            break;
        }
        if (aa == AA_FSR && !n->vram_fsr) {
            im.add("AMD FSR is not running right now (%s): nothing.", vk_fsr_status());
            break;
        }
        // (FSR) measured on the test PC (RX 9070 XT) with its pass on its own (LARECOMP_GPUPROF_AT): FSR 4, 1.4 ms at
        // 3440x1440 and 3.1 ms at 5160x2160 (it grows with the pixels: its neural network goes over all of them); at
        // 5160x2160, FSR 3.1 2.25 ms (72%) and FSR 2.3 1.7 ms (54%). The version: the chosen one, or with "Automatic"
        // the one running (AMD FSR's status).
        int ver = g_settings.fsr_version;
        for (const char *s = vk_fsr_status(); !ver && *s; s++)
            if (s[0] >= '2' && s[0] <= '4' && s[1] == '.') ver = s[0] - '0';
        double vf = ver == 3 ? 0.72 : ver == 2 ? 0.54 : 1.0;
        double ms = (aa == AA_SMAA ? 0.4 : 1.4 * vf) * k;
        im.lv[PM_GPU] = gpu_lv(ms);
        im.set(PM_GPU, "+%s ms", num(ms, 1));
        if (aa == AA_FSR) {
            im.lv[PM_VRAM] = vram_lv((double)n->vram_fsr);
            if (n->vram_fsr) im.set(PM_VRAM, "%s", mem(n->vram_fsr));
            im.add("GPU: about %s ms per frame (measured: FSR 4, 1.4 ms at 3440x1440 and 3.1 ms at 5160x2160; at "
                   "5160x2160, FSR 3.1 2.3 ms and FSR 2.3 1.7 ms; estimated for the current pixels).", num(ms, 1));
            if (P > Po * 1.1)  // (the internal resolution above the window's: FSR works at the internal one)
                im.add("FSR works at the internal resolution, here %s times the window's pixels: with the internal "
                       "resolution matching the display it would cost about %s ms. For more FPS: that resolution, "
                       "\"FSR quality\", or frame generation with the FPS limit halved.", num(P / Po, 2), num(1.4 * vf * ko, 1));
            if (n->vram_fsr) im.add("Video memory: %s (its context, according to AMD, and its image).", mem(n->vram_fsr));
            else im.add("Video memory: not active right now (it could not be created at this resolution, or it has "
                        "not been needed yet).");
        } else
            im.add("GPU: about %s ms per frame (measured: 0.4 ms at 3440x1440). Video memory: little.", num(ms, 1));
        break;
    }
    case IM_FG:
    case IM_FGVER: {
        if (!g_settings.framegen) {
            im.add("Off: nothing.");
            break;
        }
        if (!n->vram_fg) {
            im.add("Generation is not running right now (%s): nothing.", vk_fg_status());
            break;
        }
        bool light = which == IM_FGVER && g_settings.fg_version == 3;
        double ms = 2.1 * ko * (light ? 0.5 : 1);
        im.lv[PM_GPU] = gpu_lv(ms), im.lv[PM_CPU] = PM_MID, im.lv[PM_VRAM] = vram_lv((double)n->vram_fg);
        im.set(PM_GPU, "+%s ms", num(ms, 1)), im.set(PM_CPU, "+60 %%"), im.set(PM_RAM, "+80 MB");
        im.set(PM_VRAM, "%s", mem(n->vram_fg));
        im.add("GPU: about %s ms per generated frame (measured with the Windows counters: 2.1 ms with FG 4 at "
               "3440x1440, +25%% GPU with 120 game frames)%s.", num(ms, 1), light ? tr("; FG 3.1, lighter "
               "(estimated: half)") : "");
        im.add("CPU: +60%% (measured: from 5.5 to 9%% of the processor at 120 FPS). RAM: +80 MB.");
        im.add("Video memory: %s (its context and its swapchain, according to AMD).", mem(n->vram_fg));
        break;
    }
    case IM_LSFG: {
        // (SMRPG) Lossless Scaling frame generation (vulkan/vk_lsfg.c). GPU: measured now, with timestamps around its
        // work for each real frame (the motion and the N in-between ones), over the stretch before the menu was opened
        // (with it open it does not generate; on an RX 9070 XT at 1080p: x2, 1.4 ms; in performance mode, 0.77; x3,
        // 2.2; flow at 50%, 1.2). Memory: its N + 2 window-sized RGBA16F images, the swapchain's N extra ones and its
        // internals (measured with VK_EXT_memory_budget; without it, estimated from the PC's figures at 1080p: 43 MB
        // with the flow at 75%, the same with x2, x3 or x4; 20 at 50%; 27 in performance mode).
        if (g_settings.lsfg < 2) {
            im.add("Off: nothing.");
            break;
        }
        if (!vk_lsfg_wanted()) {
            im.add("Generation is not running right now (%s): nothing.", vk_lsfg_status());
            break;
        }
        int nn = g_settings.lsfg - 1, meas = 0;
        double fl = g_settings.lsfg_flow / 75.0, px = 1920.0 * 1080 * Po, vl = (double)vk_lsfg_vram(&meas);
        double vb = (vl > 0 ? vl : (nn + 2) * 8 * px) + (nn + 1) * 4 * px +
                    (meas ? 0 : 43.0 * 1048576 * Po * fl * fl * (g_settings.lsfg_perf ? 0.63 : 1.0));
        double ms = vk_lsfg_gpu_ms();
        im.lv[PM_VRAM] = vram_lv(vb), im.set(PM_VRAM, "%s", mem(vb));
        if (ms > 0) {
            im.lv[PM_GPU] = gpu_lv(ms), im.set(PM_GPU, "+%s ms", num(ms, 2));
            im.add("GPU: %s ms per real frame (measured before opening this menu: the motion between the previous one "
                   "and it, and %s).", num(ms, 2), nn == 1 ? tr("the one in between") : nn == 2 ? tr("the 2 in between") : tr("the 3 in between"));
        } else
            im.add("GPU: measured while it generates (with this menu closed): look here again after playing a little. "
                   "(Measured on an RX 9070 XT at 1080p: x2, 1.4 ms per real frame; in performance mode, 0.8; x3, "
                   "2.2.)");
        im.add("Video memory: about %s (its %d floating-point window images, %d extra swapchain images and its own).",
               mem(vb), nn + 2, nn);
        break;
    }
    case IM_SS: {
        int s = g_settings.supersample > 0 ? g_settings.supersample : 1;
        if (s <= 1) {
            im.add("No supersampling: nothing.");
            break;
        }
        double fr = (double)(s - 1) / s, ms = n->gpu_ms * fr, vb = n->vram_targets * fr;
        im.lv[PM_GPU] = gpu_lv(ms), im.lv[PM_VRAM] = vram_lv(vb);
        im.set(PM_GPU, "+%s ms", num(ms, 1)), im.set(PM_VRAM, "+%s", mem(vb));
        im.add("GPU: x%d pixels, about %s ms per frame (measured: x4 at 3440x1440 goes from 3.9 to 13 ms).", s, num(ms, 1));
        im.add("Video memory: about %s in larger render targets.", mem(vb));
        break;
    }
    case IM_SHARP:
        if (g_settings.sharpness <= 0) {
            im.add("Off: nothing.");
            break;
        }
        im.lv[PM_GPU] = gpu_lv(0.2 * ko);
        im.set(PM_GPU, "+%s ms", num(0.2 * ko, 1));
        im.add("GPU: about %s ms per frame (measured: 0.2 ms at 3440x1440).", num(0.2 * ko, 1));
        break;
    case IM_ANISO: {
        double ms = 0.4 * (g_settings.aniso / 16.0) * k;
        im.lv[PM_GPU] = gpu_lv(ms);
        if (g_settings.aniso) im.set(PM_GPU, "+%s ms", num(ms, 1));
        if (g_settings.aniso) im.add("GPU: about %s ms per frame (measured: 0.4 ms with 16x at 3440x1440).", num(ms, 1));
        else im.add("Game default: nothing.");
        break;
    }
    case IM_SHADERS:
        if (g_settings.shader_wait == 1) im.lv[PM_CPU] = PM_MID, im.set(PM_CPU, "hitches");
        im.add("Each new shader is prepared on the CPU once (then it stays in the disk cache). \"Wait\" stops the "
               "game meanwhile: hitches.");
        break;
    case IM_TEXHD:
        if (!g_settings.tex_pack || !n->vram_hd) {
            im.add(g_settings.tex_pack ? "No HD textures loaded (the \"textures\" folder is empty, or the ones it has "
                                         "have not been on screen yet): nothing."
                                       : "Off: nothing.");
            break;
        }
        im.lv[PM_VRAM] = vram_lv((double)n->vram_hd);
        im.set(PM_VRAM, "%s", mem(n->vram_hd)), im.set(PM_SSD, "on load");
        im.add("Video memory: %s in HD textures (measured now).", mem(n->vram_hd));
        im.add("Disk and CPU: each PNG is read and decompressed when its texture loads (measured with 43: barely "
               "anything).");
        break;
    case IM_TEXDUMP:
        if (!g_settings.tex_dump) {
            im.add("Off: nothing.");
            break;
        }
        im.lv[PM_SSD] = PM_MID;
        im.set(PM_SSD, "4 MB/s");
        im.add("Disk: writes a PNG for each new texture (measured: up to 4 MB/s when opening a menu; the folder grows "
               "as you play). CPU: compressing them, little.");
        break;
    case IM_HDR:
        if (!g_settings.hdr) {
            im.add("Off: nothing.");
            break;
        }
        im.add("One more step at the end (conversion to HDR10) and a 10-bit output image: little (estimated, not "
               "measured).");
        break;
    }
}
// A setting's description with its impact, and the line of dots of its row (the next one)
static const char *with_impact(int which, const char *base) {
    Impact im;
    impact_for(which, im);
    const char *vals[PM_N] = {im.val[0], im.val[1], im.val[2], im.val[3], im.val[4]};
    opt_impact_next(im.lv, vals, im.why);
    return base;
}

// Performance now (at the top of the description pane, always): what the game uses of each component, measured
// (runtime/perf_meter.c), with its traffic light
static void meter_header(void *) {
    const PerfNow *n = g_pm;
    float w = ImGui::GetContentRegionAvail().x, fs = ImGui::GetFontSize(), ls = fs * 0.62f, vs = fs * 0.68f, rr = fs * 0.17f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    dl->AddText(ImGui::GetFont(), ls, o, IM_COL32(150, 154, 140, 255), tr("NOW"));
    float y = o.y + ls * 1.25f, cw = w / 5;
    static const char *const names[PM_N] = {"CPU", "GPU", "RAM", "SSD", "VRAM"};
    char v[PM_N][32];
    if (n->ok) {
        snprintf(v[PM_CPU], sizeof v[0], "%s %%", num(n->cpu_pct, 0));
        snprintf(v[PM_GPU], sizeof v[0], "%s %% %s ms", num(n->gpu_pct, 0), num(n->gpu_ms, 1));
        snprintf(v[PM_RAM], sizeof v[0], "%s GB", num(n->ram_gb, 1));
        snprintf(v[PM_SSD], sizeof v[0], "%s MB/s", num(n->io_mbs, 1));
        snprintf(v[PM_VRAM], sizeof v[0], "%s/%s GB", num(n->vram_gb, 1), num(n->vram_budget_gb, 0));
    } else
        for (int i = 0; i < PM_N; i++) strcpy(v[i], "...");
    for (int i = 0; i < PM_N; i++) {
        float x = o.x + cw * i;
        dl->AddCircleFilled(ImVec2(x + rr, y + ls * 0.55f), rr, n->ok ? opt_level_colour(n->lv[i]) : IM_COL32(110, 110, 100, 255));
        dl->AddText(ImGui::GetFont(), ls, ImVec2(x + rr * 2 + fs * 0.18f, y), IM_COL32(170, 170, 160, 255), names[i]);
        dl->AddText(ImGui::GetFont(), vs, ImVec2(x, y + ls * 1.2f), IM_COL32(240, 236, 224, 255), v[i]);
    }
    float h = ls * 1.25f + ls * 1.2f + vs * 1.3f;
    dl->AddLine(ImVec2(o.x, o.y + h), ImVec2(o.x + w, o.y + h), IM_COL32(236, 194, 84, 90), 1.0f);
    ImGui::Dummy(ImVec2(w, h + fs * 0.3f));
}

// Restart the game: another copy with the same command line and the current settings (the ones that require reopening
// the game, like the language, are applied); this one closes. The new one waits for this one to end (LARECOMP_WAIT_PID,
// runtime/platform/windows.c). Asks for confirmation (B / Esc: no).
static void restart_rows(void) {
    static int ask;
    if (ask && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) ask = 0;
    bool pending = g_settings.language != g_settings_boot.language;
    const char *d = "Closes the game and opens it again with the current settings: whatever only applies when it "
                    "opens (the language) is applied. Anything not saved is lost (the game autosaves when you change "
                    "areas). Asks for confirmation: press again.";
    if (pending || ask) opt_mark_next();
    if (opt_action("Restart the game", ask ? "Sure? Press again" : pending ? "Pending: the language" : "", d)) {
        if (!ask) ask = 1;
        else {
            ask = 0;
            settings_save();
            if (plat_spawn_self()) {
                SDL_Event q = {};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
            }
        }
    }
    if (ask && opt_action("Cancel", "", "Back to the menu without restarting.")) ask = 0;
}

// Save, undo, reset, restart: the footer of the description pane (always, at the bottom right), with a status line.
// Reset and Restart ask for a second press (B / Esc: no).
static void buttons_footer(void *) {
    float w = ImGui::GetContentRegionAvail().x, fs = ImGui::GetFontSize(), gap = fs * 0.25f, bw = (w - 3 * gap) / 4;
    static char saved[48];
    static int ask_reset, ask_restart;
    if ((ask_reset || ask_restart) &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        ask_reset = ask_restart = 0;
    bool changed = memcmp(&g_settings, &g_settings_at_open, sizeof g_settings) != 0;
    bool pending = g_settings.language != g_settings_boot.language;
    if (opt_pane_button("save", "Save", "Saves all the settings to larecomp.ini now (next to the saves). They "
                        "apply right away, and they are also saved when this menu closes.", bw, false, false)) {
        settings_save();
        time_t t = time(NULL);
        struct tm lt;
#ifdef _WIN32
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        snprintf(saved, sizeof saved, tr("Saved at %02d:%02d:%02d"), lt.tm_hour, lt.tm_min, lt.tm_sec);
        ask_reset = ask_restart = 0;
    }
    ImGui::SameLine(0, gap);
    if (opt_pane_button("undo", "Undo", "Goes back to the settings there were when this menu opened, on every "
                        "tab (except the controls).", bw, false, !changed)) {
        g_settings = g_settings_at_open;
        g_want_fullscreen_apply.store(true);
        input_apply_background();
        ask_reset = ask_restart = 0;
    }
    ImGui::SameLine(0, gap);
    if (opt_pane_button("reset", ask_reset ? "Sure?" : "Reset",
                        "Puts this tab's settings back to their defaults (the internal resolution and the upscaling, "
                        "the ones for your GPU). The language does not change. Asks for confirmation: press again.", bw, ask_reset, false)) {
        if (!ask_reset) ask_reset = 1, ask_restart = 0;
        else {
            ask_reset = 0;
            settings_reset_video();
            g_want_fullscreen_apply.store(true);
        }
    }
    ImGui::SameLine(0, gap);
    if (opt_pane_button("restart", ask_restart ? "Sure?" : "Restart",
                        "Closes the game and opens it again with the current settings: whatever only applies when it "
                        "opens (the language) is applied. Anything not saved is lost (the game autosaves when you "
                        "change areas). Asks for confirmation: press again.", bw, ask_restart || pending, false)) {
        if (!ask_restart) ask_restart = 1, ask_reset = 0;
        else {
            ask_restart = 0;
            settings_save();
            if (plat_spawn_self()) {
                SDL_Event q = {};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
            }
        }
    }
    const char *st = ask_reset || ask_restart ? "Press again to confirm (B or Esc: no)."
                   : pending ? "Restart pending: the language."
                   : saved[0] ? saved : "Changes apply right away.";
    float ss = fs * 0.62f;
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ss, ImVec2(o.x, o.y + fs * 0.15f),
                                        ask_reset || ask_restart || pending ? IM_COL32(236, 194, 84, 255)
                                                                            : IM_COL32(150, 154, 140, 255), tr(st));
}

// (SMRPG) SMRPG Recomp's display tab: without the Link's Awakening parts (aspect with its camera hooks, blur, Hero
// Mode) and with named resolutions, supersampling, sharpness and anisotropic filtering.
static void tab_display_smrpg(void) {
    float scale = ImGui::GetFontSize() / 34;
    g_pm = perf_meter_now();
    opt_begin("##video", scale);
    opt_section("Game");
    {   // (the language of the game's text: runtime/services/am.c GetDesiredLanguage; Automatic = Windows's)
        static const char *langs[N_LANGUAGES] = {"Automatic (Windows')", "Japanese", "English", "Spanish", "French",
                                                 "German", "Italian", "Dutch", "Russian", "Korean", "Chinese (Simplified)",
                                                 "Chinese (Traditional)"};
        bool pending = g_settings.language != g_settings_boot.language;
        opt_choice("Game language", &g_settings.language, langs, N_LANGUAGES, LANG_AUTO,
                   pending ? "The language of the game's text (menus, dialogue) and of this menu. The game's own text "
                             "changes the next time it opens: \"Restart the game\", at the end of this tab, does it "
                             "now."
                           : "The language of the game's text (menus, dialogue) and of this menu. The game's own text "
                             "changes the next time it opens.",
                   false);
    }
    opt_section("Display");
    {
        static const char *wm[] = {"Windowed", "Borderless", "Fullscreen"};
        if (opt_choice("Display mode", &g_settings.window_mode, wm, 3, WIN_WINDOW, with_impact(IM_NONE,
                       "Windowed: with a frame, the size of \"Display resolution\" (or whatever you drag its edges "
                       "to). Borderless: fills the whole monitor at its resolution without being exclusive full "
                       "screen: Alt+Tab and Windows notifications are instant. Fullscreen: exclusive, at the "
                       "\"Display resolution\" (the monitor switches mode if it is not its own). F11 or Alt+Enter: "
                       "between the window and the last of the other two."), false)) {
            g_settings.fullscreen = g_settings.window_mode != WIN_WINDOW;
            g_want_fullscreen_apply.store(true);
        }
    }
    {   // the final resolution: the window size or the monitor mode in fullscreen
        DisplayInfo di;
        pthread_mutex_lock(&g_disp_lock);
        di = g_disp;
        pthread_mutex_unlock(&g_disp_lock);
        auto aspect_name = [](int w, int h) -> const char * {
            double a = h > 0 ? (double)w / h : 0;
            if (fabs(a - 16.0 / 9) < 0.02) return "16:9";
            if (fabs(a - 16.0 / 10) < 0.02) return "16:10";
            if (a > 2.3 && a < 2.45) return "21:9";
            if (fabs(a - 32.0 / 9) < 0.06) return "32:9";
            if (fabs(a - 4.0 / 3) < 0.02) return "4:3";
            if (fabs(a - 5.0 / 4) < 0.02) return "5:4";
            return "";
        };
        static char labels[66][56];
        static const char *names[66];
        int vw[66], vh[66], n = 0, sel = 0;
        if (g_settings.window_mode == WIN_BORDERLESS) {
            snprintf(labels[0], sizeof labels[0], tr("%dx%d (the monitor's)"), di.desk_w, di.desk_h);
            names[0] = labels[0];
            int one = 0;
            opt_choice("Display resolution", &one, names, 1, 0, with_impact(IM_OUTRES,
                       "Borderless, the window fills the whole monitor: its resolution is the monitor's. For another "
                       "one, \"Windowed\" or \"Fullscreen\"."), false);
        } else {
            bool full = g_settings.window_mode == WIN_FULLSCREEN;
            if (full) snprintf(labels[0], sizeof labels[0], "Monitor (%dx%d)", di.desk_w, di.desk_h);
            else snprintf(labels[0], sizeof labels[0], tr("Free (%dx%d)"), di.win_w, di.win_h);
            vw[0] = vh[0] = 0, n = 1;
            if (full) {  // the monitor's modes of its own aspect (others, the monitor stretched to the whole panel)
                double da = di.desk_h > 0 ? (double)di.desk_w / di.desk_h : 0;
                for (int i = 0; i < di.n && n < 66; i++) {
                    if (da > 0 && di.h[i] > 0 && fabs((double)di.w[i] / di.h[i] / da - 1) > 0.02) continue;
                    const char *an = aspect_name(di.w[i], di.h[i]);
                    snprintf(labels[n], sizeof labels[n], "%dx%d%s%s%s, %.0f Hz", di.w[i], di.h[i], *an ? " (" : "", an, *an ? ")" : "",
                             di.hz[i]);
                    vw[n] = di.w[i], vh[n] = di.h[i], n++;
                }
            } else {  // window sizes that fit on the monitor (with the frame)
                static const int pre[][2] = {{1280, 720},  {1366, 768},  {1600, 900},  {1920, 1080}, {1920, 1200},
                                             {2560, 1080}, {2560, 1440}, {2560, 1600}, {3440, 1440}, {3840, 1600},
                                             {3840, 2160}, {5120, 1440}, {5120, 2160}};
                for (auto &p : pre) {
                    if (di.usable_w > 0 && (p[0] > di.usable_w || p[1] > di.usable_h - 40)) continue;
                    const char *an = aspect_name(p[0], p[1]);
                    snprintf(labels[n], sizeof labels[n], "%dx%d%s%s%s", p[0], p[1], *an ? " (" : "", an, *an ? ")" : "");
                    vw[n] = p[0], vh[n] = p[1], n++;
                }
            }
            for (int i = 0; i < n; i++) {
                names[i] = labels[i];
                if (vw[i] == g_settings.out_w && vh[i] == g_settings.out_h) sel = i;
            }
            if (opt_choice("Display resolution", &sel, names, n, 0, with_impact(IM_OUTRES,
                           full ? "The monitor's resolution in full screen: its modes with the same aspect ratio as "
                                  "its own (at their highest refresh rate), so that the monitor does not stretch the "
                                  "image. For another aspect ratio (for example 16:9 on a 21:9 monitor), "
                                  "\"Windowed\" at that resolution."
                                : "The size of the window, in pixels (it is centered on the monitor). The 21:9 and "
                                  "32:9 ones give the wide view even on a 16:9 monitor (with \"Widescreen\"). Free: "
                                  "whatever it has when you drag its edges."),
                           false)) {
                g_settings.out_w = vw[sel], g_settings.out_h = vh[sel];
                g_want_fullscreen_apply.store(true);
            }
        }
    }
    opt_toggle("Show FPS", &g_settings.show_fps, false,
               with_impact(IM_NONE, "Shows the frames per second in the top right corner."));
    {
        static const char *fl[] = {"30", "60", "90", "120", "144", "165", "240"};
        static const int flv[] = {30, 60, 90, 120, 144, 165, 240};
        int fi = 1;
        for (int i = 0; i < 7; i++) if (flv[i] == g_settings.fps_limit) fi = i;
        if (opt_choice("FPS limit", &fi, fl, 7, 1, with_impact(IM_FPS,
                       "How many frames per second the game draws: 60 is what the console does. Higher is smoother. "
                       "Battles are drawn at this limit too and their logic stays at 60 steps per second, as on the "
                       "console: presses between steps are held, so that timed hits, sound and animations don't "
                       "drift apart. With frame generation, 120 on screen."), false))
            g_settings.fps_limit = flv[fi];
    }
    {
        static const char *wide[] = {"On (fill the screen)", "Off (16:9 with bars)"};
        char d[700];
        int n = snprintf(d, sizeof d,
                         tr("On an ultrawide (21:9, 32:9) or 16:10 display or window, the game draws more of the scene "
                         "at the sides (or above and below) instead of black bars: its camera opens up, nothing is "
                         "stretched and the interface keeps its proportions. It follows the window: change the "
                         "display mode or resolution (or drag the window) and it adapts in a moment. Off: 16:9 as on "
                         "the console, with bars.\n\nThe game's screen is now %d x %d."), g_guest_w, g_guest_h);
        int ww, wh;
        if (rt_unity_screen_wanted(&ww, &wh) && n > 0 && n < (int)sizeof d)
            snprintf(d + n, sizeof d - n, rt_unity_screen_follow_failed()
                     ? tr(" The game did not change it to %d x %d: it will the next time it opens (until then, with bars).")
                     : tr(" Changing to %d x %d."), ww, wh);
        opt_choice("Widescreen", &g_settings.aspect, wide, 2, ASPECT_UNLOCKED, with_impact(IM_WIDE, d));
    }
    {   // (the game's CRI movies, in their 16:9: runtime/hle/movie_fit.c)
        static const char *mf[] = {"Black bars", "Fill the screen"};
        double ga = g_guest_h > 0 ? (double)g_guest_w / g_guest_h : 16.0 / 9;
        bool other = g_settings.aspect == ASPECT_UNLOCKED && fabs(ga / (16.0 / 9) - 1) > 0.01;
        if (!other) opt_disable_next();
        opt_choice("Widescreen movies", &g_settings.movie_fit, mf, 2, 1, with_impact(IM_NONE,
                   other ? "The game's movies are made in 16:9. Fill the screen: the movie is enlarged without "
                           "stretching until it covers it; on 21:9 a little is cropped at the top and bottom (on "
                           "16:10, at the sides). The animated title background stays in 16:9 (its logo reaches the "
                           "top edge). Black bars: the whole movie in the center, with black at the sides. Applies "
                           "right away."
                         : "Only with widescreen on and a display that is not 16:9: on 16:9 the movies fill it."),
                   false);
    }

    opt_section("Image");
    static const char *res[] = {"Match display", "Dynamic", "720p", "900p", "1080p", "1440p", "1800p",
                                "2160p (4K)", "2880p (5K)", "4320p (8K)", "50 %", "67 %", "75 %", "85 %"};
    static const int res_mode[] = {RES_MATCH, RES_DYNAMIC, RES_H720, RES_H900, 1, RES_H1440, RES_H1800, 2, RES_H2880, 4,
                                   RES_Q50, RES_Q67, RES_Q75, RES_Q85};
    enum { NRES = sizeof res_mode / sizeof res_mode[0] };
    {
        int ri = 0, rdef = 0;
        for (int i = 0; i < NRES; i++) {
            if (res_mode[i] == g_settings.res_mode) ri = i;
            if (res_mode[i] == g_default_res_mode) rdef = i;
        }
        float sx, sy;
        renderer_get_scale(&sx, &sy);
        char d[480];
        int n = snprintf(d, sizeof d, tr("The resolution the game is drawn at. Match display: that of your monitor or "
                                      "window. Dynamic: the highest your GPU keeps at full speed. 720p to 4320p: "
                                      "that height, whatever the window. The percentages are of the display.\n\nNow: "
                                      "%.0f x %.0f."), (double)g_guest_w * sx, (double)g_guest_h * sy);
        if (g_settings.res_mode == RES_DYNAMIC)
            snprintf(d + n, sizeof d - n, tr(" Dynamic: %.0f%% of the display."), output_scale_dynamic_factor() * 100);
        if (opt_choice("Internal resolution", &ri, res, NRES, rdef, with_impact(IM_RES, d), false))
            g_settings.res_mode = res_mode[ri], g_settings.res_auto = false;
    }
    {
        static const char *ups[] = {"Bilinear", "AMD FSR 1"};
        int ui = g_settings.upscaler == UPSCALE_BILINEAR ? 0 : 1;
        if (opt_choice("Upscaling", &ui, ups, 2, g_default_upscaler == UPSCALE_BILINEAR ? 0 : 1, with_impact(IM_NONE,
                       "How the image is enlarged to the window when it is drawn at a lower resolution. AMD FSR 1: "
                       "edge-aware upscaling, sharper. Bilinear: simple and soft.")))
            g_settings.upscaler = ui ? UPSCALE_SHARP : UPSCALE_BILINEAR, g_settings.upscaler_auto = false;
    }
    {
        static const char *aa[] = {"Off", "SMAA", "AMD FSR"};
        char d[700];
        snprintf(d, sizeof d,
                 tr("Smooths the jagged edges of the image. SMAA: finds the edges and blends only along them, so "
                 "textures stay sharp. AMD FSR: temporal anti-aliasing (FSR 4 on Radeon RX 9000, FSR 3.1 on the "
                 "rest) with the game's own motion vectors: it gathers several frames, with stable edges and fine "
                 "detail without the cost of supersampling. They can be combined with supersampling.\n\nAMD FSR: %s."),
                 vk_fsr_status());
        opt_choice("Anti-aliasing", &g_settings.antialias, aa, 3, AA_OFF, with_impact(IM_AA, d));
    }
    if (g_settings.antialias == AA_FSR) {
        static const char *fm[] = {"Native (anti-aliasing)", "Quality (x1.5)", "Balanced (x1.7)", "Performance (x2)",
                                   "Ultra performance (x3)"};
        opt_choice("FSR quality", &g_settings.fsr_mode, fm, 5, 0, with_impact(IM_FSRMODE,
                   "Native: the scene is drawn at the internal resolution and FSR only removes the jaggies. The rest: "
                   "the scene is drawn with fewer pixels (1.5, 1.7, 2 or 3 times less in width and height) and FSR "
                   "enlarges it to the internal resolution, for more frames per second on more modest GPUs. The "
                   "interface is always at full resolution. With the internal resolution set to \"Dynamic\", native "
                   "only."),
                   false);
        static const char *fv[] = {"Automatic", "FSR 4", "FSR 3.1", "FSR 2.3"};
        static const int fvv[] = {0, 4, 3, 2};
        int fi = 0;
        for (int i = 0; i < 4; i++) if (fvv[i] == g_settings.fsr_version) fi = i;
        if (opt_choice("FSR version", &fi, fv, 4, 0, with_impact(IM_FSRVER,
                       "Which version of AMD FSR is used. Automatic: the best your GPU runs (FSR 4, a neural network, "
                       "on Radeon RX 9000; FSR 3.1 on the rest). FSR 3.1 and FSR 2.3: the earlier algorithms, on any "
                       "GPU. The change applies right away."), false))
            g_settings.fsr_version = fvv[fi];
        char fgd[1000];
        snprintf(fgd, sizeof fgd,
                 tr("Generates a frame between every two of the game's with its motion vectors (AMD FSR Frame "
                 "Generation: version 4, a neural network, on Radeon RX 9000; 3.1 on the rest): twice the frames per "
                 "second on screen and smoother motion. It adds some latency and, on things that move very fast, the "
                 "odd artifact. It works best with at least 60 game frames and the FPS limit at half the monitor's "
                 "refresh rate (for example 120 on a 240 Hz one: you see 240). It pauses while this menu is "
                 "open.\n\nFrame generation: %s."),
                 vk_fg_status());
        opt_toggle("Frame generation", &g_settings.framegen, false, with_impact(IM_FG, fgd));
        if (g_settings.framegen) {
            static const char *gv[] = {"Automatic", "FG 4 (AI)", "FG 3.1"};
            static const int gvv[] = {0, 4, 3};
            int gi = 0;
            for (int i = 0; i < 3; i++) if (gvv[i] == g_settings.fg_version) gi = i;
            if (opt_choice("Frame generation version", &gi, gv, 3, 0, with_impact(IM_FGVER,
                           "Which AMD frame generation is used. Automatic: the best your GPU runs (FG 4, a neural "
                           "network, on Radeon RX 9000; FG 3.1 on the rest). FG 3.1: the earlier algorithm, on any "
                           "GPU. The change applies in a moment."), false))
                g_settings.fg_version = gvv[gi];
        }
    }
    {   // (SMRPG) Lossless Scaling frame generation (vulkan/vk_lsfg.c)
        static const char *lm[] = {"Off", "x2", "x3", "x4"};
        int li = g_settings.lsfg >= 2 && g_settings.lsfg <= 4 ? g_settings.lsfg - 1 : 0;
        char d[1100];
        snprintf(d, sizeof d,
                 tr("Generates frames between the game's with Lossless Scaling's frame generation (LSFG 3.1): x2 shows "
                 "twice the frames per second, x3 three times, x4 four times. The game runs at the display's refresh "
                 "rate divided by the multiplier (on a 120 Hz display with x2, 60 real frames; with x4, 30) and some "
                 "latency is added. Battles need 60 real frames: if they don't get there, nothing is generated while "
                 "the battle lasts. It needs your copy of Lossless Scaling from Steam: it is looked for in your "
                 "Steam libraries. It does not work with HDR or with AMD frame generation.\n\nNow: %s."),
                 vk_lsfg_status());
        // (a short label: the long one overlapped the value)
        if (opt_choice("Lossless Scaling frame generation", &li, lm, 4, 0, with_impact(IM_LSFG, d), false))
            g_settings.lsfg = li ? li + 1 : 0;
        if (g_settings.lsfg) {
            static const char *fl[] = {"25 %", "50 %", "75 %", "100 %"};
            int fi = g_settings.lsfg_flow <= 37 ? 0 : g_settings.lsfg_flow <= 62 ? 1 : g_settings.lsfg_flow <= 87 ? 2 : 3;
            if (opt_choice("Flow scale (LSFG)", &fi, fl, 4, 2, with_impact(IM_LSFG,
                           "The resolution at which LSFG computes the motion between frames: lower is cheaper and a "
                           "little less precise. For 1080p, Lossless Scaling's 75%; on large displays 50% is enough."), false))
                g_settings.lsfg_flow = 25 * (fi + 1);
            const bool perf_def = false;  // (SMRPG: on PC, quality mode)
            opt_toggle("Performance mode (LSFG)", &g_settings.lsfg_perf, perf_def, with_impact(IM_LSFG,
                       "The light variant of LSFG: quite a bit less work for the GPU, with slightly lower quality on "
                       "things that move fast."));
        }
    }
    {
        static const char *ss[] = {"Off", "x2", "x4", "x8", "x16"};
        static const int ssv[] = {1, 2, 4, 8, 16};
        int si = 0;
        for (int i = 0; i < 5; i++) if (ssv[i] == g_settings.supersample) si = i;
        if (opt_choice("Supersampling", &si, ss, 5, 0, with_impact(IM_SS,
                       "Supersampling anti-aliasing: the game is drawn with 2, 4, 8 or 16 times more pixels and "
                       "reduced to the window with a box filter. It removes jaggies and the shimmering of fine "
                       "detail. It multiplies the GPU's work: x4 draws at twice the width and height."), false))
            g_settings.supersample = ssv[si];
    }
    opt_int("Sharpness", &g_settings.sharpness, 0, 10, 1, 0, "%d",
            with_impact(IM_SHARP, "Sharpening of the final image (AMD RCAS, the one in FSR): from 1 to 10, the "
                                  "maximum. Useful with supersampling or when upscaling."),
            "Off");
    opt_toggle("True black", &g_settings.true_black, true, with_impact(IM_NONE,
               "The game's color grading leaves black as a very dark greenish gray (1/255 of red and green): you see "
               "it on loading screens, at the sides of widescreen movies and in the deepest shadows, above all on "
               "OLED or with HDR. On, black is black and the rest barely changes (the interface and the movies, not "
               "at all). Off: as on the console."));
    {
        static const char *af[] = {"Game default", "2x", "4x", "8x", "16x"};
        static const int afv[] = {0, 2, 4, 8, 16};
        int ai = 4;
        for (int i = 0; i < 5; i++) if (afv[i] == g_settings.aniso) ai = i;
        if (opt_choice("Anisotropic filtering", &ai, af, 5, 4,
                       with_impact(IM_ANISO, "Sharp textures when seen at an angle (floors, paths). 16x costs next to "
                                             "nothing on a current GPU."), false))
            g_settings.aniso = afv[ai];
    }
    {
        static const char *sw[] = {"In the background", "Wait", "Balanced"};
        int si = g_settings.shader_wait >= 0 && g_settings.shader_wait <= 2 ? g_settings.shader_wait : 2;
        if (opt_choice("Shader compilation", &si, sw, 3, 2, with_impact(IM_SHADERS,
                       "When something new appears, its shader is prepared (the first time; after that it stays "
                       "saved). Balanced: waits at most 10 ms per frame, so almost nothing shows up late and there "
                       "are no long hitches. In the background: no waiting, new things may be missing for an "
                       "instant. Wait: nothing is ever missing, at the cost of a hitch or two when entering a new "
                       "area."), false))
            g_settings.shader_wait = si;
    }
    {
        static const char *td[] = {"Normal", "High", "Maximum"};
        opt_choice("Texture detail", &g_settings.tex_detail, td, 3, 0, with_impact(IM_TEXDETAIL,
                   "Uses a more detailed version of each texture at a distance (a finer mipmap level). Sharper, at "
                   "the cost of some shimmering on fine detail, which supersampling removes. Goes well with "
                   "supersampling."),
                   false);
    }
    opt_toggle("HD textures", &g_settings.tex_pack, true, with_impact(IM_TEXHD,
               "Uses the textures in the \"textures\" folder (next to the saves) instead of the game's: one PNG per "
               "texture, named like its dump (at least the 16-digit hash at the start of the name), with the same "
               "aspect ratio and the same size or larger (for example, enlarged x2 or x4 with an AI). The folder is "
               "read when the game opens; each texture changes the next time the game loads it."));
    opt_toggle("Dump textures", &g_settings.tex_dump, false, with_impact(IM_TEXDUMP,
               "Saves each new texture the game loads in the \"textures-dump\" folder (next to the saves), as a PNG "
               "with its hash in the name: the starting point for HD textures. Slows loading down a little while it "
               "is on."));

    opt_section("HDR");
    {
        bool offered = false, on = renderer_hdr_state(&offered);
        char d[420];
        snprintf(d, sizeof d, tr("HDR10 output for HDR displays. The game's image is SDR: it is converted to HDR with "
                              "the brightness below and, if you like, with the highlights boosted. Now: %s."),
                 on ? tr("HDR10 active") : g_settings.hdr ? tr("your display does not offer HDR (turn on \"Use HDR\" in "
                                                         "Windows Settings > Display)") : offered ? "SDR" : "SDR");
        opt_toggle("HDR", &g_settings.hdr, false, with_impact(IM_HDR, d));
        if (!g_settings.hdr) opt_disable_next();
        opt_int("Paper white", &g_settings.hdr_paper, 80, 400, 10, 200, "%d nits",
                "How bright the game's normal white looks (the interface, the skies). 200 is the usual value.");
        if (!g_settings.hdr) opt_disable_next();
        opt_int("Peak brightness", &g_settings.hdr_peak, 400, 2000, 50, 1000, "%d nits",
                "Your display's peak: highlights are boosted up to it.");
        if (!g_settings.hdr) opt_disable_next();
        opt_int("Highlight boost", &g_settings.hdr_boost, 0, 10, 1, 5, "%d",
                "How much the brightest parts (fire, sky, glints) are boosted toward the peak brightness. 0: the game "
                "as it is, just brighter.", "Off");
    }
    {   // the description pane: the meter at the top, the buttons at the bottom (with their status line)
        float fs = ImGui::GetFontSize();
        opt_pane_header(meter_header, NULL);
        opt_pane_footer(buttons_footer, NULL, fs * 1.75f + fs * 0.62f * 1.2f + fs * 0.45f);
    }
    opt_end();
}

static void tab_display(void) {
    if (!g_game_las) { tab_display_smrpg(); return; }
    float scale = ImGui::GetFontSize() / 34;
    opt_begin("##video", scale);
    opt_section("Display");
    const char *aspects[] = {"Unlocked", "Original 16:9"};
    opt_choice("Aspect ratio", &g_settings.aspect, aspects, 2, ASPECT_UNLOCKED,
               "Unlocked: the view fills the window, so wider (or taller) windows show more of the island - up to "
               "ultrawide; houses and dungeons keep their framing. Original 16:9: the game's own framing, with bars.");
    bool fs = g_settings.fullscreen;
    if (opt_toggle("Fullscreen", &fs, false, "Fills the screen. Also: F11, Alt+Enter or Cmd+Ctrl+F.")) {
        g_settings.fullscreen = fs;
        g_settings.window_mode = fs ? WIN_BORDERLESS : WIN_WINDOW;  // (SMRPG: the display mode)
        g_want_fullscreen_apply.store(true);
    }
    opt_toggle("Show FPS", &g_settings.show_fps, false, "Shows the frame rate in the top right corner.");

    opt_section("Rendering");
    // display order -> RES_* (the ini / enum order predates the menu order)
    // (up to 4x: 7680 x 4320, 8K - more only ran out of GPU memory, and sat next to 75% in the list)
    // (Match display in the middle: left from it 75% / 50% / 25%, right Dynamic then 1x-4x; user 2026-10-11)
    static const char *res[] = {"25%", "50%", "75%", "Match display", "Dynamic", "1x", "2x", "3x", "4x"};
    static const int res_mode[] = {RES_Q25, RES_Q50, RES_Q75, RES_MATCH, RES_DYNAMIC, 1, 2, 3, 4};
    enum { RES_MATCH_AT = 3 };
    enum { NRES = sizeof res_mode / sizeof res_mode[0] };
    {
        int ri = RES_MATCH_AT, rdef = RES_MATCH_AT;  // (rdef: the GPU's default - Dynamic on an integrated GPU)
        for (int i = 0; i < NRES; i++) {
            if (res_mode[i] == g_settings.res_mode) ri = i;
            if (res_mode[i] == g_default_res_mode) rdef = i;
        }
        float sx, sy;
        renderer_get_scale(&sx, &sy);
        char d[400];
        int n = snprintf(d, sizeof d, "How sharp the 3D image is. Match display: your screen's own resolution. Dynamic: "
                                      "as sharp as your GPU keeps at full speed. 1x-4x: the Switch's 1080p times that%s. "
                                      "75%% / 50%% / 25%%: a share of the display, for slower machines.\n\nRendering at "
                                      "%.0f x %.0f.", g_gpu_integrated ? " (1x: 720p on this GPU)" : "", 1920.0 * sx, 1080.0 * sy);
        if (g_settings.res_mode == RES_DYNAMIC)
            snprintf(d + n, sizeof d - n, " Dynamic: %.0f%% of the display right now.", output_scale_dynamic_factor() * 100);
        if (opt_choice("Internal resolution", &ri, res, NRES, rdef, d, false))
            g_settings.res_mode = res_mode[ri], g_settings.res_auto = false;
    }
    {   // each OS's own upscaler next to bilinear (UPSCALE_SHARP: MetalFX on macOS, AMD FSR 1 on Windows)
#ifdef __APPLE__
        static const char *ups[] = {"Bilinear", "MetalFX"};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. MetalFX: Apple's "
                         "upscaler, sharper. Bilinear: plain and soft.";
#else
        static const char *ups[] = {"Bilinear", "AMD FSR 1"};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. AMD FSR 1: "
                         "edge-aware upscaling and sharpening, sharper. Bilinear: plain and soft.";
#endif
        int ui = g_settings.upscaler == UPSCALE_BILINEAR ? 0 : 1;
        char udm[512];
        if (g_settings.res_mode == RES_MATCH) {  // (renders at the window's size: nothing to scale up)
            snprintf(udm, sizeof udm, "%s\n\nNot used at Match display: the game already renders at your display's resolution.", ud);
            ud = udm;
            opt_disable_next();
        }
        if (opt_choice("Upscaler", &ui, ups, 2, g_default_upscaler == UPSCALE_BILINEAR ? 0 : 1, ud))
            g_settings.upscaler = ui ? UPSCALE_SHARP : UPSCALE_BILINEAR, g_settings.upscaler_auto = false;
    }
    static const char *aa[] = {"Off", "SMAA"};
    opt_choice("Anti-aliasing", &g_settings.antialias, aa, 2, AA_OFF,
               "Smooths jagged edges of the game image before it is scaled to the window. SMAA: finds the edges and "
               "blends only along them, so textures stay sharp. Most useful at a lower internal resolution.");

    opt_section("Game");
    opt_toggle("Remove blur", &g_settings.blur_removal, false,
               "Turns off the tilt-shift blur at the top and bottom of the screen.");
    opt_toggle("Hide Hero Mode icon", &g_settings.hide_hero_icon, false,
               "Hides the Hero Mode emblem in the corner of the screen while playing a Hero Mode file.");
    opt_end();
}

extern "C" bool cosmetics_seed_on(void);  // runtime/cosmetics_seed.c

// Cosmetics: tunic / heart colours, live (runtime/nvn/texpatch.c), and recolours by draw (runtime/nvn/recolour*.c);
// a seed made with "Random cosmetics" fills the rows left on Default (shown as "Seed: <colour>")
static void tab_cosmetics(void) {
    opt_begin("##cosmetics", ImGui::GetFontSize() / 34);
    CosColour *rows[] = {&g_settings.tunic, &g_settings.hearts, &g_settings.hair, &g_settings.skin, &g_settings.shield,
                         &g_settings.leaves, &g_settings.grass, &g_settings.sand};
    if (opt_button("Randomize all", "Every color below gets a random color."))
        for (CosColour *c : rows) c->pick = 1 + rand() % (COL_RANDOM - 1);  // (a named colour: shown as what it is)
    if (opt_button("Default all", "Everything on this page back to the game's own look.")) {
        for (CosColour *c : rows) c->pick = COL_DEFAULT;
        g_settings.enemy_colours = 0;
        g_settings.screen_filter = 0;
    }
    opt_section("Colors");
    opt_colour("Tunic color", &g_settings.tunic, COL_SLOT_TUNIC,
               "Link's tunic color. Default: the game's own (or the randomizer seed's).");
    opt_colour("Heart color", &g_settings.hearts, COL_SLOT_HEARTS, "The color of the hearts on the HUD.");
    opt_colour("Hair color", &g_settings.hair, COL_SLOT_HAIR, "Link's hair color.");
    opt_colour("Body color", &g_settings.skin, COL_SLOT_SKIN, "Link's body color.");  // (larecomp.ini: skin)
    opt_colour("Shield color", &g_settings.shield, COL_SLOT_SHIELD, "The color of Link's shield.");
    opt_section("Overworld");
    {   // Season: fills Leaves, Grass and Sand at once (palette: Orange 2, Gold 3, Pink 9, White 10); "Mixed" when
        // they match none
        static const char *const names[] = {"Spring", "Autumn", "Winter", "Cherry blossom", "Mixed"};
        static const int preset[4][3] = {{COL_DEFAULT, COL_DEFAULT, COL_DEFAULT}, {2, 3, 3}, {10, 10, 10},
                                         {9, COL_DEFAULT, COL_DEFAULT}};
        int cur = 4;  // (as shown: a row on Default shows the seed's colour, if it has its own)
        for (int i = 0; i < 4; i++)
            if (col_pick(&g_settings.leaves, COL_SLOT_LEAVES) == preset[i][0] &&
                col_pick(&g_settings.grass, COL_SLOT_GRASS) == preset[i][1] &&
                col_pick(&g_settings.sand, COL_SLOT_SAND) == preset[i][2])
                cur = i;
        int v = cur;
        if (opt_choice("Season", &v, names, 5, 0, "Sets the leaves, grass and sand colors below.") && v != cur) {
            if (v == 4) v = cur == 0 ? 3 : 0;  // (Mixed is never chosen: past the ends, back round)
            g_settings.leaves.pick = preset[v][0];
            g_settings.grass.pick = preset[v][1];
            g_settings.sand.pick = preset[v][2];
        }
    }
    opt_colour("Leaves color", &g_settings.leaves, COL_SLOT_LEAVES, "The overworld's tree leaves and bushes.");
    opt_colour("Grass color", &g_settings.grass, COL_SLOT_GRASS, "The overworld's grass.");
    opt_colour("Sand color", &g_settings.sand, COL_SLOT_SAND, "The beaches, the desert and the sandy south.");
    opt_section("Enemies");
    {   // (Off in a seed with its own cosmetics: "Seed", its enemies are per seed)
        const char *modes[N_ENEMY_COLOUR_MODES];
        for (int i = 0; i < N_ENEMY_COLOUR_MODES; i++) modes[i] = g_enemy_colour_modes[i];
        if (cosmetics_seed_on()) modes[0] = "Seed";
        opt_choice("Enemy colors", &g_settings.enemy_colours, modes, N_ENEMY_COLOUR_MODES, 0,
                   "Each kind of enemy in another color. Enemies whose color shows their strength or type keep theirs.");
    }
    opt_section("Screen");
    opt_choice("Screen filter", &g_settings.screen_filter, g_screen_filters, N_SCREEN_FILTERS, 0,
               "A filter over the whole picture, the HUD included.");
    opt_end();
}

static void tab_audio(void) {
    if (!g_game_las) {  // (SMRPG: the volume; the reverb and the movie audio were Link's Awakening's)
        opt_begin("##audio", ImGui::GetFontSize() / 34);
        opt_section("Sound");
        float pct = roundf(g_settings.volume * 100);
        if (opt_slider("Volume", &pct, 0, 200, 5, 100, "%g%%",
                       "The game's volume. 100% is its own; above that, louder."))
            g_settings.volume = pct / 100.0f;
        opt_section("Settings");
        if (g_settings.volume == g_settings_defaults.volume) opt_disable_next();
        if (opt_action("Reset audio", "", "Puts the volume back to its default (100%)."))
            settings_reset_audio();
        opt_end();
        return;
    }
    opt_begin("##audio", ImGui::GetFontSize() / 34);
    opt_section("Sound");
    float pct = roundf(g_settings.volume * 100);
    if (opt_slider("Volume", &pct, 0, 200, 5, 100, "%g%%",
                   "The game's volume. 100% is the game's own level; above it, louder."))
        g_settings.volume = pct / 100.0f;
    opt_toggle("Room reverb", &g_settings.reverb, true,
               "The game's echo in caves, dungeons and big rooms.");
    opt_toggle("Movie audio", &g_settings.movie_audio, true,
               "Sound in the game's movies (the opening and the ending). Applies the next time a movie starts.");
    opt_end();
}

// Quit: back to the start screen (in game: the game reboots to it) or out to the desktop. Each asks first (progress
// since the last save is lost); B / Esc or Cancel backs out.
extern "C" void rt_request_reboot(int kind);  // runtime/main.c (RT_REBOOT_TITLE = 2: the start screen)
static void tab_quit(float scale, bool in_game) {
    static int ask;  // 0: the two choices; 1: start screen?; 2: desktop?
    if (ask && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        ask = 0;
    if (!g_game_las) {  // (SMRPG: only to the desktop; the start screen is Link's Awakening's launcher)
        opt_begin("##quit", scale);
        opt_section("Quit");
        const char *d = "Closes " APP_NAME ". Anything not saved is lost (the game autosaves when you change areas).";
        if (!ask) {
            restart_rows();
            if (opt_action("Quit to desktop", "", d)) ask = 2;
            opt_section("About");
            opt_action("Version", APP_VERSION,
                       APP_NAME " " APP_VERSION ". Numbering x.y.z (Semantic Versioning): z goes up with fixes, y "
                       "with new things and x with what breaks earlier ones; beta: playable from start to finish and "
                       "still being tested. The details of each version are in CHANGELOG.txt.");
        } else {
            if (opt_action("Yes, quit", "", d)) {
                settings_save();
                SDL_Event q = {};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
                ask = 0;
            }
            if (opt_action("Cancel", "", "Back to the game.")) ask = 0;
        }
        opt_end();
        return;
    }
    const char *lost = in_game ? "\n\nProgress since your last save is lost." : "";
    char d1[200], d2[200];
    snprintf(d1, sizeof d1, "Leaves the game and goes back to the " APP_NAME " start screen.%s", lost);
    snprintf(d2, sizeof d2, "Closes " APP_NAME ".%s", lost);
    opt_begin("##quit", scale);
    opt_section("Quit");
    if (!ask) {
        if (!in_game) opt_disable_next();  // (already on it)
        if (opt_action("Quit to Start Screen", "", in_game ? d1 : "You're on the start screen.")) ask = 1;
        if (opt_action("Quit to Desktop", "", d2)) ask = 2;
    } else {
        if (opt_action(ask == 1 ? "Yes, quit to Start Screen" : "Yes, quit to Desktop", "", ask == 1 ? d1 : d2)) {
            settings_save();
            if (ask == 1) {
                rt_request_reboot(2);
                set_open(false);
            } else {
                SDL_Event q = {};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
            }
            ask = 0;
        }
        if (opt_action("Cancel", "", "Stays where you are.")) ask = 0;
    }
    opt_end();
}

extern "C" bool inventory_overlay_wanted(void);
extern "C" bool tracker_overlay_wanted(void);
extern "C" bool tracker_dungeon_wanted(void);
extern "C" void tracker_dungeon_draw(ImVec2 display, ImFont *font);
extern "C" void tracker_overlay_draw(ImVec2 display, ImFont *font);
extern "C" bool chaos_overlay_wanted(void);
extern "C" void chaos_overlay_draw(ImVec2 display, ImFont *font);
extern "C" void inventory_overlay_draw(ImVec2 display, ImFont *font);
extern "C" bool race_overlay_wanted(void);
extern "C" void race_overlay_draw(ImVec2 display, ImFont *font, float scale);
extern "C" bool pickup_flyin_wanted(void);  // (silent pickups: pickup_flyin.mm)
extern "C" void pickup_flyin_draw(ImVec2 display, ImFont *font, float scale);
extern "C" bool race_overlay_key(char *out, size_t cap);

// ---- reusing the overlay's last drawing (LARECOMP_OVERLAY_CACHE=0: off, a new ImGui frame every present as before)
// While only the FPS counter and / or the run timer are up (no menu, no input, nothing interactive or animated), the
// overlay draws the same thing frame after frame: ImGui's NewFrame + layout + Render then run for nothing on the
// render thread. Their drawing is described by a key (the text shown, the window size); while it stays the same, the
// last frame's draw data (ImGui keeps it until the next NewFrame) is drawn again. Only after three frames in a row
// built with that key: an auto-sized window (the FPS counter) settles over the frames after its text changes.
// The FPS counter then shows the same average as ImGui's io.Framerate (the last 60 frame times), sampled twice a
// second instead of every frame.
static int g_ov_cache = -1;
static char g_ov_key[96];
static int g_ov_same;  // frames built in a row with g_ov_key
static float g_fps_dt[60];
static int g_fps_n, g_fps_i;
static double g_fps_sum, g_fps_shown, g_fps_at;
static void fps_note(double now, float dt) {
    if (g_fps_n == 60) g_fps_sum -= g_fps_dt[g_fps_i];
    else g_fps_n++;
    g_fps_dt[g_fps_i] = dt;
    g_fps_sum += dt;
    g_fps_i = (g_fps_i + 1) % 60;
    if (now - g_fps_at >= 0.5 || now < g_fps_at) {
        g_fps_at = now;
        g_fps_shown = g_fps_sum > 0 ? g_fps_n / g_fps_sum : 0;
    }
}

// Reboot (runtime/main.c): the game is gone - menu and name dialog close; the ImGui context stays.
RT_ON_RESET(menu_reset) {
    set_open(false);
    g_kbd = 0;
    osk_cancel();
    pthread_mutex_lock(&q_lock);
    q_events.clear();
    pthread_mutex_unlock(&q_lock);
}

static void keyboard_draw(ImGuiIO &io, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 120));
    float w = fminf(io.DisplaySize.x * 0.9f, 1100 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::Begin("##kbd", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(g_font_big);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "Enter your name");
    ImGui::PopFont();
    static bool was_open, was_pad;
    bool pad = g_last_pad.load(), appearing = !was_open || pad != was_pad;  // (switching device: focus the new one)
    was_open = true, was_pad = pad;
    if (!pad) {  // keyboard: type straight into the field
        if (appearing) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::PushFont(g_font_big);
        ImGui::InputText("##name", g_kbd_text, (size_t)(g_kbd_max * 4 + 1 < (int)sizeof g_kbd_text ? g_kbd_max * 4 + 1 : sizeof g_kbd_text),
                         ImGuiInputTextFlags_CallbackCharFilter,
                         [](ImGuiInputTextCallbackData *d) -> int { return d->EventChar < 0x20 ? 1 : 0; });
        ImGui::PopFont();
        int units = kbd_cut(g_kbd_text, g_kbd_max);
        ImGui::TextDisabled("Enter  confirm      Esc  cancel      (%d / %d)", units, g_kbd_max);
    } else {  // controller: on-screen keyboard (osk.mm, shared with our menus' text fields)
        int r = osk_keys(g_kbd_text, sizeof g_kbd_text, g_kbd_max, appearing, scale, g_font_big);
        if (r) g_kbd = r > 0 ? 2 : 3;  // (Start / + confirms in menu_handle_event too)
    }
    ImGui::End();
    if (g_kbd.load() != 1) was_open = false;
}

// Game thread (runtime/hle/swkbd.c): show the dialog, re-present the last frame until it's answered.
// Returns 1 confirmed (out = text, UTF-8), 0 cancelled.
extern "C" int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    extern void backend_represent(void);
    snprintf(g_kbd_text, sizeof g_kbd_text, "%s", initial ? initial : "");
    g_kbd_max = max_len > 0 && max_len < (int)sizeof g_kbd_text / 4 ? max_len : 8;
    input_reset();
    g_kbd = 1;
    // (test) LARECOMP_KBD_AUTO=<text>: answer at once
    if (getenv("LARECOMP_KBD_AUTO")) { snprintf(g_kbd_text, sizeof g_kbd_text, "%s", getenv("LARECOMP_KBD_AUTO")); g_kbd = 2; }
    while (g_kbd.load() == 1 && !g_rt_unwind) {  // ~60 Hz (a reboot cancels)
        uint64_t t0 = SDL_GetTicks();
        backend_represent();
        uint64_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay((uint32_t)(16 - dt));
    }
    kbd_cut(g_kbd_text, g_kbd_max);  // (whatever entered it: typed, on-screen keys, the test hook)
    bool ok = g_kbd.load() == 2 && g_kbd_text[0];
    if (ok) snprintf(out, cap, "%s", g_kbd_text);
    g_kbd = 0;
    input_reset();
    return ok;
}

// LARECOMP_MENU_FOCUS_LOG=1 (test; tools/menu_focus_sweep.sh): every Down / Up on a page of option rows is checked a
// few frames later against the rows' order and logged "[menu] focus check ... OK / FAIL": the first Down after a tab
// switch (however it came) or the menu opening, and Down from the tab bar, land on the page's first enabled row; Down
// goes to the next row (the last one stays), Up to the row before, and Up from the top row onto the selected tab.
static void focus_check(bool down, bool up, bool any_down, int focused_tab, int tab, const char *tab_name) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_MENU_FOCUS_LOG") != NULL;
    if (!on) return;
    static int shown = -1000, due_in, want, from, prev_focus = -1;  // (want: a row index, -1: the selected tab)
    static bool first_down_due;
    static const char *what;
    if (tab != shown || g_page_fresh_seen) shown = tab, first_down_due = true, g_page_fresh_seen = false, prev_focus = -1;
    int n = opt_row_count(), first = opt_first_row(), focus = opt_focused_row();
    if (any_down && !down) first_down_due = false;  // (a held Down's repeat: the first Down was checked or skipped)
    if ((down || up) && !due_in && n > 0) {
        from = prev_focus, due_in = 4;
        if (down && (first_down_due || from < 0)) want = first, what = first_down_due ? "first Down" : "Down from the tab bar";
        else if (down) want = from + 1 < n ? from + 1 : from, what = "Down";
        else if (from > 0) want = from - 1, what = "Up";
        else if (from == 0) want = -1, what = "Up from the top row";
        else due_in = 0;  // (Up on the tab bar: not a page move)
        if (down) first_down_due = false;
    } else if (down && n == 0 && first_down_due) {
        first_down_due = false;
        LOG("[menu] focus check: first Down on %s SKIP (no option rows)\n", tab_name);
    }
    prev_focus = focus;
    if (!due_in || --due_in) return;
    bool ok = want >= 0 ? focus == want : focused_tab == tab;
    char got[160];
    if (focus >= 0) snprintf(got, sizeof got, "\"%s\"", opt_row_label(focus));
    else snprintf(got, sizeof got, "tab %d", focused_tab);
    char exp[160];
    if (want >= 0) snprintf(exp, sizeof exp, "\"%s\"", opt_row_label(want));
    else snprintf(exp, sizeof exp, "tab %d", tab);
    LOG("[menu] focus check: %s on %s from %s -> %s (want %s) %s\n", what, tab_name,
        from >= 0 ? opt_row_label(from) : "the tab bar", got, exp, ok ? "OK" : "FAIL");
}

// The tab bar and its pages, into the current window: the in-game menu, and the start screen's Settings (in_game
// false: nothing of the game is loaded yet - the tabs that need it are greyed out; prelaunch: the start screen's own
// first tab, game files and fonts, drawn by launcher.mm).
static void tabs_body(float scale, bool in_game, void (*prelaunch)(float)) {
    // (id: the page; -1 = Prelaunch)
    struct Tab { const char *name; int id; bool on; };
    Tab tabs[11];
    int ntabs = 0;
    if (prelaunch) tabs[ntabs++] = {"Prelaunch", -1, true};
    static const char *names[] = {"Video", "Audio", "Save Editor", "Randomizer", "Warp", "Cheats", "Gameplay",
                                  "Cosmetics", "Controls", "Quit"};
    static const int order[] = {8, 0, 1, 6, 7, 3, 5, 4, 2, 9};  // Controls, Video, Audio, Gameplay, Cosmetics,
                                                                // Randomizer,
                                                                // Cheats, Warp, Save Editor, Quit (ids stay: test
                                                                // hooks)
                                                                // (SMRPG) only what applies to Super Mario RPG: the
                                                                // rest (save editor, randomizer, warps, cheats,
                                                                // gameplay, cosmetics) is Link's Awakening's and reads
                                                                // its addresses
    static const char *names_smrpg[] = {"Video", "Audio", "", "", "", "", "", "", "Controls", "Quit"};
    static const int order_smrpg[] = {8, 0, 1, 9};
    if (!g_game_las) {
        for (int i : order_smrpg)
            if (i != 9 || !prelaunch) tabs[ntabs++] = {tr(names_smrpg[i]), i, true};
    } else
    for (int i : order)  // (Quit: in game only - the start screen has its own QUIT, and an eleventh tab cut names short)
        if (i != 9 || !prelaunch) tabs[ntabs++] = {names[i], i, in_game || (i != 2 && i != 4 && i != 5)};
    int cur = 0;  // g_tab / g_tab_request hold ids
    for (int i = 0; i < ntabs; i++) if (tabs[i].id == g_tab) cur = i;
    if (!tabs[cur].on) cur = 0;
    // L/R (or Q/E) switch tabs, skipping greyed-out ones (not while typing: Q / E are letters then, and the on-screen
    // keyboard has the controller)
    if (g_tab_request_ext >= 0) { g_tab_request = g_tab_request_ext; g_tab_request_ext = -1; }
    bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();
    static int focused_tab = -99;  // the tab the controller's cursor is on (from the frame before; -99: none)
    bool switching = false;  // (L / R this frame: the page changes next frame)
    if (!any_popup && !typing) {
        int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                 : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1 : 0;
        if (step) {
            int i = cur;
            do i = (i + step + ntabs) % ntabs; while (!tabs[i].on);
            g_tab_request = tabs[i].id;
            // the cursor goes onto the new tab (left behind on the tab bar, A on it switched straight back - user
            // 2026-10-11; left in the old page, it stayed on a row that is gone, and Down went on from where that row
            // had been)
            g_focus_tab = tabs[i].id, g_focus_frames = 0;
            switching = true;
        }
    }
    bool down = !any_popup && !typing && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown) ||
                                          ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown) ||
                                          ImGui::IsKeyPressed(ImGuiKey_DownArrow));
    bool up = !any_popup && !typing && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp) ||
                                        ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp) || ImGui::IsKeyPressed(ImGuiKey_UpArrow));
    // The first Down on a page just switched to (L / R, a click or A on a tab, the menu opening), or Down with the
    // cursor on the tab bar: the page's first enabled row, top to bottom - never ImGui's pick from wherever its cursor
    // was (a row of the page before, the tab bar's x). Option rows take it (opt_focus_first); a page without them gets
    // its first control (g_page_focus_next).
    if (ImGui::GetIO().MouseClicked[0]) g_page_fresh = false;  // (the mouse went into the page)
    bool down_from_tab = down && !switching &&
                         ((focused_tab != -99 && ImGui::GetCurrentContext()->NavCursorVisible) || g_page_fresh);
    if (down_from_tab) {
        ImGui::NavMoveRequestCancel();  // (ImGui's own move this frame)
        g_page_fresh = false;
        g_focus_tab = -99, g_focus_frames = 0;  // (the cursor stops going back to the tab)
    }
    opt_focus_first(down_from_tab);
    // Up / Down on a page of option rows: the next / previous row in order (opt_nav_step), not ImGui's spatial pick
    if ((down || up) && !down_from_tab && !switching && opt_nav_step(down ? 1 : -1)) ImGui::NavMoveRequestCancel();
    opt_frame_reset();
    // The cursor moved out of the page (ImGui's pick, applied this frame for last frame's press - Up from the page's
    // top control): onto the selected tab. ImGui's pick was the tab nearest the control's x (Controls, from most
    // rows), or the close button
    ImGuiContext &g = *ImGui::GetCurrentContext();
    ImGuiWindow *root = ImGui::GetCurrentWindow()->RootWindow;
    static bool nav_in_page;  // (the frame before: the cursor on a control of the page - its child windows)
    if (nav_in_page && g.NavJustMovedToId && g.NavWindow == root && !any_popup) g_focus_tab = -100, g_focus_frames = 0;
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTabListScrollingButtons)) {
        for (int k = 0; k < ntabs; k++) {
            const Tab &t = tabs[k];
            ImGuiTabItemFlags f = g_tab_request == t.id && t.on ? ImGuiTabItemFlags_SetSelected : 0;
            char lbl[64];
            snprintf(lbl, sizeof lbl, prelaunch ? " %s " : "  %s  ", t.name);  // (ten tabs: tighter, so none is cut short)
            ImGui::BeginDisabled(!t.on);  // (out of the game: Save Editor, Warp, Cheats)
            // focus (controller / keyboard): the tab lights up a little with a gold bar along its bottom, instead of
            // ImGui's outline (it sits 4 px outside the tab and the tab bar's clipping could cut its top off on macOS)
            // (focused_tab: from the frame before - colours go in before the tab is drawn)
            bool lit = focused_tab == t.id && ImGui::GetCurrentContext()->NavCursorVisible;
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            if (lit) {
                ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.19f, 0.27f, 0.22f, 1));
                ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.19f, 0.27f, 0.22f, 1));
            }
            if (g_focus_tab == -1 && !prelaunch) g_focus_tab = 8;  // (no Prelaunch in game: Controls, the first tab)
            bool focus_here = g_focus_tab == t.id || (g_focus_tab == -100 && t.id == g_tab);
            bool open = ImGui::BeginTabItem(lbl, NULL, f);
            ImGui::PopStyleColor(lit ? 3 : 1);
            if (focus_here) {  // the controller's cursor starts on this tab (SetKeyboardFocusHere skips tabs: no tab stop)
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
                // a few frames: the page's child window appearing makes ImGui pick its first widget once it's laid out
                if (++g_focus_frames >= 4) g_focus_tab = -99, g_focus_frames = 0;
                else g_focus_tab = t.id;
            }
            if (lit) {
                ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                float h = 4 * scale > 2 ? 4 * scale : 2;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + 2, b.y - h), ImVec2(b.x - 2, b.y),
                                                          IM_COL32(236, 194, 84, 255));  // (the menu's gold)
            }
            if (ImGui::IsItemFocused()) focused_tab = t.id;
            else if (focused_tab == t.id) focused_tab = -99;
            ImGui::EndDisabled();
            if (open) {
                if (t.id != g_shown_tab) g_shown_tab = t.id, g_page_fresh = true, g_page_focus_next = false;
                g_tab = t.id;
                ImGui::Spacing();
                ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_None,  // (the rest of the window: no hint line)
                                  ImGuiWindowFlags_NavFlattened);
                if (g_page_focus_next) {  // (a page without option rows: its first control)
                    g_page_focus_next = false;
                    ImGui::SetKeyboardFocusHere();
                    ImGui::SetNavCursorVisible(true);
                }
                switch (t.id) {
                case -1: prelaunch(scale); break;
                case 0: tab_display(); break;
                case 1: tab_audio(); break;
                case 2:
                    if (game_in_play()) save_editor_draw(scale);
                    else ImGui::TextDisabled("Load a save file and get into the game to edit it.");
                    break;
                case 3: rando_tab_draw(scale); break;
                case 4: if (warp_tab_draw(scale)) set_open(false); break;
                case 5: cheats_draw(scale); break;
                case 6: difficulty_draw(scale); break;
                case 7: tab_cosmetics(); break;
                case 8: controls_tab_draw(scale); break;
                case 9: tab_quit(scale, in_game); break;
                }
                static const char *scroll_test = getenv("LARECOMP_MENU_SCROLL");  // test hook: page scroll, 0..1
                if (scroll_test) ImGui::SetScrollY(ImGui::GetScrollMaxY() * (float)atof(scroll_test));
                // no enabled option row took the cursor: the page's first control, next frame (SetKeyboardFocusHere:
                // the first item that takes the cursor, in order)
                if (opt_focus_first_left() && down_from_tab) g_page_focus_next = true;
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        g_tab_request = -2;
        opt_focus_first_left();  // (no page drawn: nothing took it)
        ImGui::EndTabBar();
    }
    nav_in_page = g.NavWindow && g.NavWindow != g.NavWindow->RootWindow && g.NavWindow->RootWindow == root;
    // (checks: fresh presses only - a held key's repeats come faster than a check's few frames)
    bool down1 = down && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown, false) || ImGui::IsKeyPressed(ImGuiKey_DownArrow, false));
    bool up1 = up && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, false) ||
                      ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp, false) || ImGui::IsKeyPressed(ImGuiKey_UpArrow, false));
    for (int k = 0; k < ntabs; k++)
        if (tabs[k].id == g_tab) focus_check(down1, up1, down, focused_tab, g_tab, tabs[k].name);
}

void menu_tabs_draw(float scale, ImFont *font, ImFont *font_big, bool in_game, void (*prelaunch)(float)) {
    ImFont *f0 = g_font, *f1 = g_font_big;
    g_font = font, g_font_big = font_big;
    ImGuiStyle saved = ImGui::GetStyle();  // the menu's look (the start screen has its own style)
    style_init();
    ImGui::GetStyle().ScaleAllSizes(scale);
    tabs_body(scale, in_game, prelaunch);
    ImGui::GetStyle() = saved;
    g_font = f0, g_font_big = f1;
}

// Whether menu_render draws anything: the menu, the keyboard, an overlay (FPS, inventory, tracker, chaos, race, a
// pickup card).
static bool render_needed(bool open, bool kbd, bool inv, bool trk, bool trkd, bool chaos, bool race, bool flyin) {
    return open || kbd || g_settings.show_fps || inv || trk || trkd || chaos || race || flyin;
}

#ifdef __APPLE__
void menu_render(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target) {
    const double tw = target.width, th = target.height;
#else
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h) {
    const double tw = target_w, th = target_h;
#endif
    bool open = g_open.load();
    bool inv = !open && inventory_overlay_wanted();
    bool trk = !open && tracker_overlay_wanted();
    bool trkd = !open && tracker_dungeon_wanted();
    bool chaos = !open && chaos_overlay_wanted();
    bool kbd = g_kbd.load() == 1;
    bool race = race_overlay_wanted();  // (run timer, race seed hash: race_overlay.mm)
    bool flyin = pickup_flyin_wanted() && !open;  // (a silent pickup's card)
    if (!render_needed(open, kbd, inv, trk, trkd, chaos, race, flyin)) {
        if (g_inited) { pthread_mutex_lock(&q_lock); q_events.clear(); pthread_mutex_unlock(&q_lock); }
        g_ov_same = 0;  // (shown again: built afresh first)
        return;
    }
#ifdef __APPLE__
    if (!g_inited) init(dev);
#else
    if (!g_inited) init();
#endif
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)tw, (float)th);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    static double last;
    double now = SDL_GetTicks() / 1000.0;
    io.DeltaTime = last > 0 && now > last ? (float)(now - last) : 1.0f / 60;
    last = now;
    if (g_ov_cache < 0) g_ov_cache = !getenv("LARECOMP_OVERLAY_CACHE") || atoi(getenv("LARECOMP_OVERLAY_CACHE")) != 0;
    char key[sizeof g_ov_key];
    key[0] = 0;
    if (g_ov_cache) {
        fps_note(now, io.DeltaTime);
        char rk[64];
        // (only the FPS counter and the run timer; input only reaches ImGui while the menu or the name entry is up)
        if (!open && !kbd && !inv && !trk && !trkd && !chaos && !flyin && race_overlay_key(rk, sizeof rk)) {
            pthread_mutex_lock(&q_lock);
            bool events = !q_events.empty();
            pthread_mutex_unlock(&q_lock);
            if (!events) snprintf(key, sizeof key, "%.0fx%.0f fps %d %.0f %d | %s", tw, th, (int)g_settings.show_fps,
                                  g_settings.show_fps ? g_fps_shown : 0.0, (int)vk_fg_generating(), rk);
        }
        ImDrawData *dd = ImGui::GetDrawData();
        if (key[0] && g_ov_same >= 3 && !strcmp(key, g_ov_key) && dd && dd->Valid) {
            g_text_busy = false;  // (the menu is closed)
#ifdef __APPLE__
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = target;
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            ImGui_ImplMetal_NewFrame(rp);  // (the backend's target description only)
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            ImGui_ImplMetal_RenderDrawData(dd, cb, enc);
            [enc endEncoding];
#else
            void *oc = vk_overlay_begin(cmd);  // (as below)
            ImGui_ImplVulkan_RenderDrawData(dd, (VkCommandBuffer)oc);
            vk_overlay_end(cmd, oc);
#endif
            return;
        }
        if (key[0] && !strcmp(key, g_ov_key)) g_ov_same++;
        else { memcpy(g_ov_key, key, sizeof key); g_ov_same = key[0] ? 1 : 0; }
    }
    drain_events(io);
    // fonts are built at 34/56 px for a ~1080 px tall output; scale to the window
    float scale = (float)(th / 1080.0);
    if (scale < 0.45f) scale = 0.45f;
    io.FontGlobalScale = scale;

#ifdef __APPLE__
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
#endif
    // spacing/rounding follow the output size too (style tuned for 1080 p)
    static ImGuiStyle base;
    static float style_scale = -1;
    if (style_scale < 0) base = ImGui::GetStyle();
    if (fabsf(style_scale - scale) > 0.01f) {
        ImGui::GetStyle() = base;
        ImGui::GetStyle().ScaleAllSizes(scale);
        style_scale = scale;
    }
#ifdef __APPLE__
    ImGui_ImplMetal_NewFrame(rp);
#else
    ImGui_ImplVulkan_NewFrame();
#endif
    ImGui::NewFrame();

    if (g_settings.show_fps) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16 * scale, 16 * scale), 0, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("##fps", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        double f = g_ov_cache ? g_fps_shown : io.Framerate;
        // (SMRPG) with frame generation, also what is displayed: double
        if (vk_fg_generating()) ImGui::Text(tr("%.0f fps (%.0f with generation)"), f, 2 * f);
        else ImGui::Text("%.0f fps", f);
        ImGui::End();
    }

    if (race) race_overlay_draw(io.DisplaySize, g_menu_font_big, scale);
    if (flyin) pickup_flyin_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont(), scale);
    if (trk) tracker_overlay_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    if (chaos) chaos_overlay_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    if (trkd) tracker_dungeon_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    if (inv) {
        inventory_overlay_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    }

    if (open) {
        // dim the game behind the menu
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
        float w = fminf(io.DisplaySize.x * 0.9f, 1500 * scale), h = io.DisplaySize.y - 2 * 18 * scale;  // (all the height: a slim margin)
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::Begin("##menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImFont *keep_big = g_font_big;  // (the window in the menu's font; header() uses g_font_big)
        g_font_big = g_menu_font_big;
        ImGui::PushFont(g_menu_font);
        // header as on the start screen's Settings: the title, and a close X in the corner (B / Esc too). ("Title
        // screen" - rt_request_reboot(RT_REBOOT_TITLE) - is to find a new home; user 2026-10-11.)
        ImGui::PushFont(g_font_big);
        ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", tr("Settings"));
        ImGui::PopFont();
        {
            float bx = 44 * scale;
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            bool close = ImGui::InvisibleButton("##close", ImVec2(bx, bx), ImGuiButtonFlags_EnableNav);
            ImGui::PopStyleColor();
            bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            ImU32 col = hot ? IM_COL32(236, 194, 84, 255) : IM_COL32(220, 216, 205, 255);
            float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * scale * 1.5f;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (hot) dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), IM_COL32(255, 255, 255, 20), 8 * scale);
            dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), col, t);
            dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), col, t);
            if (close) set_open(false);
        }

        bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();  // (before the
        tabs_body(scale, true, NULL);                                                                          //  pages run)
        // B with nothing focused-deeper closes the menu (not the Esc / B that ends typing: typing is from the frame
        // before, so it's still true the frame a field lets go)
        if (!any_popup && !typing && !ui_text_busy() && !controls_ui_took_back() && !rando_tab_took_back() &&
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            set_open(false);
        ImGui::PopFont();
        g_font_big = keep_big;
        ImGui::End();
        osk_draw(g_font_big, scale);  // (a text field activated with the controller)
    }

    if (kbd) keyboard_draw(io, scale);
    ImGui::Render();
    g_text_busy = open && ui_text_busy();
#ifdef __APPLE__
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
    [enc endEncoding];
#else
    // ImGui records its commands itself. Not into cmd, the window image's command buffer, while the recorder thread
    // (record.c) may still be recording it (one thread at a time on a command buffer and its pool: f054d8b, the device
    // loss entering Mabe Village), and without waiting for the recorder to catch up every frame either (a full drain:
    // 20-30 ms a frame on the Steam Deck): into a secondary command buffer of device.c's, which the recorder executes
    // in cmd in order - after the uploads queued above (overlay icons). Decided right here, where the draw data is
    // final, so an overlay turning on during this function can't miss it. (Recorder off: into cmd directly.)
    void *oc = vk_overlay_begin(cmd);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)oc);  // inside the window image's rendering
    vk_overlay_end(cmd, oc);
#endif
}
