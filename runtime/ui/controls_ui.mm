// The settings' Controls tab (in game: menu.mm; start screen: launcher.mm, both through menu_tabs_draw), in option rows
// (runtime/ui/option_rows.h), and its Configure inputs page for the bindings (runtime/input_bind.h): DEVICE and the
// categories on the left, the chosen category's bindings on the right. A on a category moves the cursor to its
// bindings; A on a binding waits for the new button / key (Esc or 5 s without a press cancels; a button already in
// use swaps over); B goes back a step (bindings -> categories -> the tab). Drawn in whichever ImGui context is current.
#include "../app_name.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"  // (SetFocusID, NavCursorVisible)
#include "option_rows.h"
#include "tr.h"

extern "C" bool g_game_las;  // runtime/game_addresses.c (false en SMRPG)

extern "C" {
#include "../input_bind.h"
#include "../settings.h"
int input_pad_family(void);              // runtime/hle/input.c
SDL_JoystickID input_active_pad(void);   // runtime/hle/input.c: the controller the game reads (0: none yet)
void input_apply_background(void);
}

// ------------------------------------------------------------------ Configure inputs: what's on it
// (SMRPG: in the player's language, and each button with what it does in Super Mario RPG, as in the tab's guide:
// guide_rows)
static const char *T(const char *las, const char *smrpg) { return g_game_las ? las : tr(smrpg); }
enum { CAT_BUTTONS, CAT_TRIGGERS, CAT_STICKS, CAT_DPAD, N_CAT };
static const char *cat_name(int c) {
    static const char *const names[N_CAT] = {"Buttons", "Triggers", "Sticks", "D-pad"};
    return tr(names[c]);
}
static const char *const k_cat_hook[N_CAT] = {"buttons", "triggers", "sticks", "dpad"};  // (LARECOMP_CONTROLS_SUB)

struct Bind { int slot; const char *label; const char *section; };  // section: a header before this row
static int cat_rows(int cat, bool pad, Bind *out) {
    int n = 0;
    auto add = [&](int slot, const char *label, const char *section = NULL) { out[n++] = {slot, label, section}; };
    switch (cat) {
    case CAT_BUTTONS:
        add(BIND_A, T("A", "A · talk / confirm")), add(BIND_B, T("B", "B · jump / cancel")),
            add(BIND_X, T("X", "X · items")), add(BIND_Y, T("Y", "Y · specials")), add(BIND_PLUS, T("Plus", "+ button · menu")),
            add(BIND_MINUS, T("Minus", "- button · gauge attack"));
        break;
    case CAT_TRIGGERS:
        add(BIND_L, T("L", "L · walk")), add(BIND_R, T("R", "R · walk")), add(BIND_ZL, T("ZL", "ZL · walk")),
            add(BIND_ZR, T("ZR", "ZR · walk"));
        break;
    case CAT_STICKS: {  // (a controller's sticks stay its sticks: only their presses are buttons)
        const char *press = T("Press", "Press"), *up = T("Up", "Up"), *down = T("Down", "Down"), *left = T("Left", "Left"),
                   *right = T("Right", "Right");
        add(BIND_LSTICK, press, T("Left stick", "Left stick (move)"));
        if (!pad) add(BIND_LS_UP, up), add(BIND_LS_DOWN, down), add(BIND_LS_LEFT, left), add(BIND_LS_RIGHT, right);
        add(BIND_RSTICK, press, T("Right stick", "Right stick"));
        if (!pad) add(BIND_RS_UP, up), add(BIND_RS_DOWN, down), add(BIND_RS_LEFT, left), add(BIND_RS_RIGHT, right);
        break;
    }
    case CAT_DPAD:
        add(BIND_UP, T("Up", "Up")), add(BIND_DOWN, T("Down", "Down")), add(BIND_LEFT, T("Left", "Left")),
            add(BIND_RIGHT, T("Right", "Right"));
        break;
    }
    return n;
}

// (SMRPG) The names in Spanish: of the keys (SDL gives them in English), of the controller buttons and of the Switch
// inputs. Latin-1 characters only: the menu's font has no others (menu.mm, its default ranges).
static const char *key_name(int sc) {
    const char *n = bind_key_label(sc);
    if (ui_spanish() && !strncmp(n, "Keypad ", 7) && strcmp(n, "Keypad Enter")) {  // "Keypad 8" -> "8 (num.)"
        static char buf[8][32];  // (several on one line: a few buffers)
        static int bi;
        char *b = buf[bi++ & 7];
        snprintf(b, sizeof buf[0], "%s (num.)", n + 7);
        return b;
    }
    return trc("key", n);
}
static const char *pad_name(int host, int fam) { return trc("pad", bind_pad_label(host, fam)); }
static const char *input_name(int slot) { return trc("input", bind_input_name(slot)); }  // (the Switch's: "A", "D-pad Up"...)
static int slot_cat(int slot) {
    Bind b[16];
    for (int c = 0; c < N_CAT; c++)
        for (int i = 0, n = cat_rows(c, false, b); i < n; i++)
            if (b[i].slot == slot) return c;
    return CAT_BUTTONS;
}

// Devices: each connected controller by name (they share one set of bindings; the name picks the button names), or
// "Controller" when none is connected (bindings for one connected later), then the keyboard.
struct Device { SDL_JoystickID id; char name[64]; int fam; bool pad; };
static int pad_fam(SDL_JoystickID id) {
    if (!id || getenv("LARECOMP_PAD_TYPE")) return bind_label_family();  // (none / a test pretending one)
    return input_family_of_type(SDL_GetGamepadTypeForID(id));
}
static int list_devices(Device *d, int cap) {
    int n = 0, np = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&np);
    for (int i = 0; i < np && n < cap - 1; i++) {
        const char *nm = SDL_GetGamepadNameForID(ids[i]);
        d[n].id = ids[i], d[n].pad = true, d[n].fam = pad_fam(ids[i]);
        snprintf(d[n].name, sizeof d[n].name, "%s", nm && *nm ? nm : T("Controller", "Controller"));
        if (strlen(d[n].name) > 30) strcpy(d[n].name + 27, "...");  // (fits beside DEVICE)
        n++;
    }
    SDL_free(ids);
    if (!n) d[n].id = 0, d[n].pad = true, d[n].fam = pad_fam(0), snprintf(d[n].name, sizeof d[n].name, "%s", T("Controller", "Controller")), n++;
    d[n].id = 0, d[n].pad = false, d[n].fam = 0, snprintf(d[n].name, sizeof d[n].name, "%s", T("Keyboard", "Keyboard")), n++;
    return n;
}

// ------------------------------------------------------------------ state
static bool g_sub;                 // on the Configure inputs page
static int g_cat = CAT_BUTTONS;    // the category shown on the right
static bool g_dev_pad = true;      // the device chosen: a controller (g_dev_id) or the keyboard
static SDL_JoystickID g_dev_id;
static bool g_in_right;            // the cursor is on a binding (last frame)
static int g_last_frame = -10;     // (a frame without this tab: the page starts over)
static int g_back_frame = -10;     // the frame B / Esc went back a step here (the menu mustn't close on it)
enum { FOCUS_NONE, FOCUS_CONFIGURE, FOCUS_DEVICE, FOCUS_CAT, FOCUS_RIGHT };
static int g_focus;                // put the cursor on this row when it's drawn

// B / Esc this frame went back a step on the Configure inputs page (menu.mm / launcher.mm: don't close too)
extern "C" bool controls_ui_took_back(void) { return g_back_frame == ImGui::GetFrameCount(); }

static void take_focus(int want) {  // (right after the row: it's the last item)
    if (g_focus != want) return;
    g_focus = FOCUS_NONE;
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisible(true);
    ImGui::SetScrollHereY(0.3f);
}

// The device the page opens on: the keyboard when it was opened with one (Enter / Space), else the controller the game
// reads (or the first connected; "Controller" when there's none)
static void pick_device(bool keyboard) {
    SDL_JoystickID act = input_active_pad();
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (!act && n > 0) act = ids[0];
    SDL_free(ids);
    g_dev_pad = !keyboard, g_dev_id = act;
}
static bool keyboard_pressed(void) {
    return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
           ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);
}

// A finished capture keeps larecomp.ini current; a swap is said under the list (another row changed too)
static void watch_results(int fam, char *msg, size_t cap) {
    static int seen = -1;
    int rdev, rslot, rother, serial, kind = bind_last_result(&rdev, &rslot, &rother, &serial);
    if (seen < 0) seen = serial;
    if (serial == seen) return;
    seen = serial;
    msg[0] = 0;
    if (kind == 2)
        snprintf(msg, cap, T("%s moved to %s (swapped).", "%s moved to %s (swapped)."), input_name(rother),
                 rdev == BIND_DEV_PAD ? pad_name(g_bind_pad[rother], fam) : key_name(g_bind_key[rother]));
    if (kind == 1 || kind == 2 || kind == 5) settings_save();
}

static bool nav_on_item(void) { return ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible; }

static void configure_inputs(float scale) {
    Device devs[9];
    int nd = list_devices(devs, 9), di = -1;
    for (int i = 0; i < nd; i++)
        if (devs[i].pad == g_dev_pad && (!g_dev_pad || devs[i].id == g_dev_id)) di = i;
    if (di < 0) di = g_dev_pad ? 0 : nd - 1;  // (that controller is gone: the first one)
    g_dev_pad = devs[di].pad, g_dev_id = devs[di].id;
    const int dev = g_dev_pad ? BIND_DEV_PAD : BIND_DEV_KEY, fam = devs[di].fam;

    int cd, cs, ms;
    bind_capture_active(&cd, &cs, &ms);
    static char msg[160];
    watch_results(fam, msg, sizeof msg);

    // B / Esc: from the bindings back to their category, from the categories back to the tab (a capture takes Esc and
    // every controller button itself: runtime/hle/bindings.c)
    if (cd < 0 && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        g_back_frame = ImGui::GetFrameCount();
        if (g_in_right) g_focus = FOCUS_CAT;
        else { g_sub = false, g_focus = FOCUS_CONFIGURE, msg[0] = 0; return; }  // (the tab is drawn instead)
    }
    // Left on a binding: back to its category too
    if (g_in_right && cd < 0 && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ||
                                 ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft)))
        g_focus = FOCUS_CAT;

    opt_begin("##configure", scale);
    opt_section(T("Configure inputs", "Configure controls"));
    const char *names[9];
    for (int i = 0; i < nd; i++) names[i] = devs[i].name;
    int ndi = di;
    if (opt_choice(T("Device", "Device"), &ndi, names, nd, di, NULL)) {
        bind_capture_cancel();
        g_dev_pad = devs[ndi].pad, g_dev_id = devs[ndi].id, msg[0] = 0;
    }
    take_focus(FOCUS_DEVICE);
    for (int c = 0; c < N_CAT; c++) {
        if (g_in_right && c == g_cat) opt_mark_next();
        bool open = opt_link(cat_name(c), NULL);
        if (c == g_cat) take_focus(FOCUS_CAT);
        if (nav_on_item() && g_cat != c) g_cat = c, msg[0] = 0;  // (moving down the list shows each)
        if (open) { if (g_cat != c) msg[0] = 0; g_cat = c, g_focus = FOCUS_RIGHT; bind_capture_cancel(); }
    }
    if (bind_is_default(dev)) opt_disable_next();
    if (opt_action(T("Restore default controls", "Restore default controls"), "",
                   g_dev_pad ? T("Puts every controller button back to its default.", "Every controller button goes "
                                                                                      "back to its default.")
                             : T("Puts every key back to its default.", "Every key goes back to its default."))) {
        bind_capture_cancel();
        bind_reset(dev);
    }
    // (the cursor or the mouse on a row with a description: it shows on the right instead of the bindings)
    bool desc_right = (ImGui::IsItemHovered() || nav_on_item()) && g_focus != FOCUS_RIGHT;
    opt_split();
    if (desc_right) {
        opt_desc_text();
        g_in_right = false;
    } else {
        Bind rows[16];
        int n = cat_rows(g_cat, g_dev_pad, rows);
        opt_section(cat_name(g_cat));
        bool in_right = false;
        for (int i = 0; i < n; i++) {
            const Bind &b = rows[i];
            if (b.section) opt_section(b.section);
            bool waiting = cd == dev && cs == b.slot;
            char val[64];
            if (waiting)
                snprintf(val, sizeof val, "%s...  %d", g_dev_pad ? T("Press a button", "Press a button") : T("Press a key", "Press a key"),
                         (ms + 999) / 1000);
            else snprintf(val, sizeof val, "%s", g_dev_pad ? pad_name(g_bind_pad[b.slot], fam) : key_name(g_bind_key[b.slot]));
            // the same button / key on two inputs (a hand-edited larecomp.ini): red
            const int16_t *t = g_dev_pad ? g_bind_pad : g_bind_key;
            bool dup = false;
            for (int j = 0, m = g_dev_pad ? BIND_PAD_COUNT : BIND_KEY_COUNT; j < m; j++)
                dup |= j != b.slot && t[j] == t[b.slot] && t[b.slot] > (g_dev_pad ? -1 : 0);
            if (dup && !waiting) opt_warn_next();
            ImGui::PushID(b.slot);
            bool hit = opt_action(b.label, val, NULL, true, waiting || !bind_is_default_slot(dev, b.slot));
            ImGui::PopID();
            if (i == 0) take_focus(FOCUS_RIGHT);
            if (ImGui::IsItemFocused()) in_right = true;
            if (waiting && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);  // (a capture started by a test hook)
            if (hit) {
                if (waiting) bind_capture_cancel();
                else { msg[0] = 0; bind_capture_begin(dev, b.slot); }
            }
        }
        g_in_right = in_right;
        if (cd == dev) ImGui::TextDisabled("%s", T("Esc cancels.", "Esc cancels."));
        else if (msg[0]) ImGui::TextDisabled("%s", msg);
    }
    opt_split_end();
}

// ------------------------------------------------------------------ (SMRPG) the guide: what each button does
// What each Switch button does in Super Mario RPG, with the key and the controller button bound to it now (the player's
// bindings: it changes with them). Looked up in the controls section of Super Mario Wiki ("Super Mario RPG (Nintendo
// Switch)", read on 2026-11-16) and in the game's own interface (on the field: X items, Y specials, + menu). What
// neither of them says (the D-pad, the right stick, the stick clicks) is left out. A / click on a row: its entry in
// "Configure controls", to change it.
static void guide_list(char *out, size_t cap, const int *slots, int n, bool pad, int fam) {
    size_t k = 0;
    out[0] = 0;
    for (int i = 0; i < n && k < cap; i++) {
        int s = slots[i];
        if (pad ? g_bind_pad[s] < 0 : g_bind_key[s] <= 0) continue;  // (not bound)
        k += snprintf(out + k, cap - k, "%s%s", k ? " " : "", pad ? pad_name(g_bind_pad[s], fam) : key_name(g_bind_key[s]));
    }
    if (!out[0]) snprintf(out, cap, "%s", tr("(not bound)"));
}
static void guide_rows(void) {
    struct Row { const char *label; int slots[4]; int n, cat; const char *desc; };
    static const Row rows[] = {
        {"Move", {BIND_LS_UP, BIND_LS_LEFT, BIND_LS_DOWN, BIND_LS_RIGHT}, 4, CAT_STICKS,
         "Mario runs when he moves; hold L, R, ZL or ZR to walk (the game's System menu can swap them). In battle, "
         "picks the action and the target."},
        {"Talk / confirm", {BIND_A}, 1, CAT_BUTTONS, "Talk, examine and confirm. In battle: attack."},
        {"Jump / cancel", {BIND_B}, 1, CAT_BUTTONS,
         "Jump and cancel. In battle: the other options (such as defending or running away)."},
        {"Items", {BIND_X}, 1, CAT_BUTTONS, "The item menu, in battle too."},
        {"Specials", {BIND_Y}, 1, CAT_BUTTONS, "The specials menu, in battle too. Riding Yoshi: jump."},
        {"Game menu", {BIND_PLUS}, 1, CAT_BUTTONS, "The game's menu."},
        {"Gauge attack", {BIND_MINUS}, 1, CAT_BUTTONS,
         "In battle, with the gauge full: its attack (with three party members, the triple move). In Bowser's Keep: "
         "give up one of the door challenges."},
        {"Walk (hold)", {BIND_L, BIND_R, BIND_ZL, BIND_ZR}, 4, CAT_TRIGGERS,
         "Hold one: walk instead of run. In the monster list, L and R turn the page."},
    };
    int fam = bind_label_family();
    for (const Row &r : rows) {
        char kb[96], pd[96], val[200];
        guide_list(kb, sizeof kb, r.slots, r.n, false, fam);
        if (r.cat == CAT_STICKS) snprintf(pd, sizeof pd, "%s", tr("Left stick"));  // (controller sticks are fixed)
        else guide_list(pd, sizeof pd, r.slots, r.n, true, fam);
        snprintf(val, sizeof val, "%s · %s", kb, pd);
        ImGui::PushID(r.label);
        bool hit = opt_action(r.label, val, r.desc);
        ImGui::PopID();
        if (hit) {
            pick_device(keyboard_pressed());
            g_sub = true, g_cat = r.cat, g_focus = FOCUS_RIGHT, g_in_right = false;
        }
    }
    opt_action("Settings menu", "Esc / F1 · Home", "This menu (video, audio and controls). On a controller: Home, "
                                                     "or Back + Start.");
    opt_action("Window / full screen", "F11 / Alt+Enter",
               "Switches between the window and the last full-screen mode (Video > Display mode).");
}

// ------------------------------------------------------------------ the tab
extern "C" void controls_tab_draw(float scale) {
    int frame = ImGui::GetFrameCount();
    if (frame - g_last_frame > 1) g_sub = false, g_focus = FOCUS_NONE, g_in_right = false;  // (back on the tab)
    g_last_frame = frame;
    // (test) LARECOMP_CONTROLS_SUB=<buttons|triggers|sticks|dpad>[:right][:key]: on that page, the cursor on the category
    // (right: on its first binding; key: the keyboard's) - for a few frames, past the menu putting it on the tab bar
    static bool hook_done;
    static int hook_frames, hook_focus;
    if (!hook_done) {
        hook_done = true;
        if (const char *h = getenv("LARECOMP_CONTROLS_SUB")) {
            pick_device(strstr(h, ":key") != NULL);
            g_sub = true, hook_focus = strstr(h, "right") ? FOCUS_RIGHT : FOCUS_CAT, hook_frames = 10;
            for (int c = 0; c < N_CAT; c++) if (!strncmp(h, k_cat_hook[c], strlen(k_cat_hook[c]))) g_cat = c;
        }
    }
    if (hook_frames > 0) hook_frames--, g_focus = hook_focus;
    {   // a capture started elsewhere (LARECOMP_BIND_TEST): show it waiting
        int cd, cs;
        if (bind_capture_active(&cd, &cs, NULL) && (!g_sub || g_dev_pad != (cd == BIND_DEV_PAD) || g_cat != slot_cat(cs))) {
            if (!g_sub) pick_device(cd == BIND_DEV_KEY);
            g_sub = true, g_dev_pad = cd == BIND_DEV_PAD, g_cat = slot_cat(cs);
        }
    }
    if (g_sub) {
        configure_inputs(scale);
        if (g_sub) return;
    }

    opt_begin("##controls", scale);
    if (!g_game_las) {  // (SMRPG: in the player's language and without the Link's Awakening parts - moving Link with
                        // the D-pad, 360 degrees, the controller light with his health, the button icons that change
                        // its textures)
        opt_section("Keys and buttons");
        guide_rows();
        opt_section("Inputs");
        if (opt_link("Configure controls", "Choose the game's buttons and keys (with the Switch's names: A is "
                                             "confirm / talk, B jump / cancel). The menu always opens with F1, Esc, "
                                             "Home or Back + Start, so no binding can lock you out of it.")) {
            pick_device(keyboard_pressed());
            g_sub = true, g_focus = FOCUS_DEVICE, g_in_right = false;
        }
        take_focus(FOCUS_CONFIGURE);
        if (opt_toggle("Controllers in the background", &g_settings.bg_input, false,
                       "Controllers keep working even when another window is in front."))
            input_apply_background();
        opt_section("Controller");
        opt_toggle("Rumble", &g_settings.rumble, true, "Controller rumble when the game asks for it.");
        if (g_settings.rumble) {
            static const char *const pct[] = {"25 %", "50 %", "75 %", "100 %"};
            int i = g_settings.rumble_strength / 25 - 1;
            if (i < 0 || i > 3) i = 3;
            opt_indent(1);
            if (opt_choice("Intensity", &i, pct, 4, 3, "How strong the rumble is.", false))
                g_settings.rumble_strength = (i + 1) * 25;
            opt_indent(-1);
        }
        {
            static const char *const names[] = {"As in the game", "As on the controller"};
            opt_choice("Confirm in menus", &g_settings.menu_confirm, names, 2, MENU_CONFIRM_CONTROLLER,
                       "Which button confirms in " APP_NAME "'s own menus (not in the game's).");
        }
        opt_end();
        return;
    }
    opt_section("Inputs");
    if (opt_link("Configure inputs", "Choose the buttons and keys for the game. The menu always opens with F1, Esc, "
                                     "Home or Back + Start, so no binding can lock you out.")) {
        pick_device(keyboard_pressed());
        g_sub = true, g_focus = FOCUS_DEVICE, g_in_right = false;
    }
    take_focus(FOCUS_CONFIGURE);
    if (opt_toggle("Allow background inputs", &g_settings.bg_input, false,
                   "Controllers keep working while another window is in front. macOS may ask for Input Monitoring "
                   "permission."))
        input_apply_background();

    opt_section("Movement");
    opt_toggle("D-pad moves Link", &g_settings.dpad_move, false, "The d-pad moves Link like the control stick.");
    opt_toggle("360-degree movement", &g_settings.move_360, false,
               "Link walks in the exact direction of the stick instead of snapping to 8 directions.");

    opt_section("Controller");
    opt_toggle("Rumble", &g_settings.rumble, true, "Controller vibration.");
    if (g_settings.rumble) {
        static const char *const pct[] = {"25%", "50%", "75%", "100%"};
        int i = g_settings.rumble_strength / 25 - 1;
        if (i < 0 || i > 3) i = 3;
        opt_indent(1);
        if (opt_choice("Intensity", &i, pct, 4, 3, "How strong the vibration is.", false))
            g_settings.rumble_strength = (i + 1) * 25;
        opt_indent(-1);
    }
    opt_toggle("Controller light", &g_settings.controller_light, true, "The light bar shows Link's health.");
    {
        static const char *const fam[] = {"Nintendo", "Xbox", "PlayStation"};
        char autol[64];
        snprintf(autol, sizeof autol, "Auto (%s)", fam[input_pad_family() % 3]);
        const char *names[] = {autol, "Nintendo", "Xbox", "PlayStation"};
        opt_choice("Button icons", &g_settings.prompts, names, 4, PROMPTS_AUTO,
                   "The button icons the game shows. Auto: the connected controller's.");
    }
    {
        static const char *const names[] = {"Game layout", "Controller convention"};
        opt_choice("Confirm button", &g_settings.menu_confirm, names, 2, MENU_CONFIRM_CONTROLLER,
                   "Which button confirms in " APP_NAME "'s menus.");
    }

    opt_end();
}
