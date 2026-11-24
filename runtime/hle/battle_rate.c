// (SMRPG) Battles: logic at 60 steps per second, drawing at whatever "FPS limit" the player has set.
//
// Battle logic (BattleManager.update_common, main 0x7001cd9360: turns, hits, the timed-hit windows, sounds, camera
// moves) advances in steps of 1/60 s: BattleManager.update (main 0x7001cd9250, replaced in tools/game_overrides.c)
// calls it once per frame (and twice if _frame_rate, +0x1c8, is 30, the debug mode). When a battle starts the game
// calls set_frame_rate (main 0x7001cd0500, also replaced) with 60. The character animators run on time
// (BattleCharacterAnimator.internal_set_animator_speed, main 0x7001e32420, only slows them down if a frame takes longer
// than normal), and so do the sound and the UI.
// Before: while the battle had its rate set, the vsync (hle/vi.c) ticked at 60 Hz - 60 frames and 60 steps per second,
// as on the console - because at 240 frames the battle took 240 steps, four times faster than its animations and its
// sound. Now the vsync ticks at the "FPS limit" in battle too and update paces the steps by real time: an accumulator
// owes 1/60 s per step, and each frame runs 0, 1, 2 or 3 depending on what has accumulated (at 240, one every four
// frames), with the debt capped at 4 steps so that a very slow frame doesn't start a spiral: the logic still takes
// exactly 60 steps per second.
// Button presses aren't lost between steps: the controller snapshot (ControllerObserver._npadState: buttons and
// preButtons; edges are computed on the fly, GetButtonDown = buttons & ~preButtons) is checked once per frame, and
// whatever was pressed or released since the previous step is latched and handed to the next step due, as if the button
// had been down for that whole console frame. The extra steps of a single frame get the real snapshot (two steps per
// frame already existed in 30 mode and the game copes well with it).
// With frame generation (vk_lsfg.c battle_slow): the battle needs 60 real frames to take its 60 steps in; if they don't
// fit with the multiplier and the display, generation is turned off while it lasts, as before.
// Tests: LARECOMP_BATTLE_FPS=30 (the debug mode: vsync at 60 Hz and two steps per frame, as before);
// LARECOMP_BATTLE_FIX=0 (as the game does it: the rate it asks for and one step per frame at the "FPS limit", the
// battle sped up, for comparison).
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../hle.h"
#include "../platform.h"

extern uint64_t renderer_present_count(void);  // (runtime/nvn: the frame being presented)

static volatile uint64_t bm_statics;  // BattleManager's static fields (_instance at +0x10)
static volatile int battle_on;        // (what the vsync thread reads: that thread doesn't touch game memory)

// ---- step pacing and press latching (tools/game_overrides.c, once per frame) ----
static int want;                       // 60, or 30 with LARECOMP_BATTLE_FPS=30
static uint64_t acc_ns, last_ns;       // what the logic owes, and when the previous frame was
static uint64_t pend_down, pend_up;    // edges seen since the previous step, latched until the next step due
static uint64_t last_buttons;          // the snapshot's buttons on the previous frame (our own edges)
static uint64_t npad_addr;             // ControllerObserver._npadState (class statics + 8)
static int pace_started;               // the battle's first frame: one step right away, like the original
static uint64_t battle_t0_ns;          // when the battle started, for the call rate at the end
static uint64_t steps_run;             // steps planned by the pacing (counted by whoever asks for them)
static uint64_t update_calls;          // ACTUAL calls to update_common (counted by whoever makes them: must match
                                       // steps_run - if not, someone is taking extra steps)
static void pace_clear(void) { acc_ns = last_ns = 0, pend_down = pend_up = last_buttons = 0, npad_addr = 0, pace_started = 0; steps_run = update_calls = 0; }

RT_ON_RESET(battle_rate_reset) { bm_statics = 0, battle_on = 0; pace_clear(); }  // (a restart rebuilds game memory)

static int fix_on(void) {
    static int on = -1;
    if (on < 0) on = !getenv("LARECOMP_BATTLE_FIX") || atoi(getenv("LARECOMP_BATTLE_FIX")) != 0;
    return on;
}

// the controller snapshot: ControllerObserver's class comes from its metadata slot (the value points to the class), the
// class statics are at +0xb8, and _npadState is its static field +0x8 (buttons +8, preButtons +0x28)
static uint64_t npad_state_addr(void) {
    if (npad_addr) return npad_addr;
    uint64_t slot = LD64(0x7003a59000ull + 0x4e8);
    uint64_t klass = slot ? LD64(slot) : 0;
    uint64_t statics = klass ? LD64(klass + 0xb8) : 0;
    npad_addr = statics ? statics + 0x8 : 0;
    return npad_addr;
}

// (tools/game_overrides.c, set_frame_rate; Unity's main thread) the rate of the battle that is starting
int rt_battle_frame_rate(uint64_t statics, int requested) {
    if (!fix_on()) {  // (LARECOMP_BATTLE_FIX=0: the rate the game asks for; the log still sees the battle)
        bm_statics = statics, want = 60, pace_clear();
        LOG("[battle] the game asks for %d FPS: %d (LARECOMP_BATTLE_FIX=0: one step per frame, as the game does)\n", requested, requested);
        return requested;
    }
    bm_statics = statics;  // (the battle is seen on the next frame, rt_battle_poll: _frame_rate is set on return)
    want = getenv("LARECOMP_BATTLE_FPS") && atoi(getenv("LARECOMP_BATTLE_FPS")) == 30 ? 30 : 60;
    pace_clear();  // (the first frame that sees the battle starts the pacing; no phantom edges from before)
    LOG("[battle] the game asks for %d FPS: %d (%s), drawing at the FPS limit while it lasts (frame %llu)\n", requested, want,
        want == 60 ? "60 logic steps per second spread over real time" : "debug mode: two steps per frame",
        (unsigned long long)renderer_present_count());
    return want;
}

// (nvn/vulkan/device.c backend_present: a game thread, every frame) is the battle still going, with its rate set?
// BattleManager._instance with _frame_rate != 0 (_instance is a static reference: as long as it points to the object,
// the object is still alive). Read by a game thread, which is stopped before the game's memory is rebuilt.
void rt_battle_poll(void) {
    uint64_t s = bm_statics;
    if (!s) return;
    uint64_t inst = LD64(s + 0x10);
    int on = inst && LD32(inst + 0x1c8) != 0;
    if (on && !battle_on) {
        battle_t0_ns = plat_now_ns();
        LOG("[battle] starts (frame %llu, %lld ms)\n", (unsigned long long)renderer_present_count(),
            (long long)(battle_t0_ns / 1000000));
    }
    if (!on && battle_on)
        LOG("[battle] ends (frame %llu, %lld ms, %llu logic steps, %llu calls to update_common, %.1f per second)\n",
            (unsigned long long)renderer_present_count(), (long long)(plat_now_ns() / 1000000),
            (unsigned long long)steps_run, (unsigned long long)update_calls,
            update_calls * 1000.0 / (double)((plat_now_ns() - battle_t0_ns) / 1000000 + 1));
    battle_on = on;
}

// (hle/vi.c, the vsync thread) 60 Hz in battle only in 30 mode (its two steps per frame); in 60 mode, the "FPS limit":
// BattleManager.update paces the steps by real time
int rt_battle_vsync_hz(int hz) { return battle_on && want == 30 ? 60 : hz; }

// (vk_lsfg.c battle_slow) true while a battle lasts: its logic takes 60 steps per second and needs 60 real frames to
// take them in; if they don't fit, generation is turned off while it lasts
bool rt_battle_vsync_60(void) { return battle_on != 0; }

// (tools/game_overrides.c, BattleManager.update: once per frame) the plan for this frame: -1 = the pacing doesn't apply
// (no battle, rate != 60, 30 mode, LARECOMP_BATTLE_FIX=0: the original path, one step per frame); 0..3 = logic steps
// from this pacing for the accumulated time (0: no step is due this frame, and update_common is NOT called). The
// address of the controller snapshot goes in *npad (0 only with -1). Along the way it latches the snapshot's edges
// since the previous step.
int rt_battle_plan(int rate, uint64_t *npad) {
    static const uint64_t PERIOD = 1000000000ull / 60;
    *npad = 0;
    if (!fix_on() || want != 60 || !battle_on || rate != 60) return -1;
    uint64_t n = npad_state_addr();
    if (!n) return -1;
    *npad = n;
    uint64_t b = LD64(n + 8);
    if (!pace_started) {  // (the battle's first frame: one step right away, like the original)
        pace_started = 1;
        last_buttons = b, acc_ns = PERIOD, last_ns = plat_now_ns();
    } else {
        pend_down |= b & ~last_buttons;   // (pressed since the previous step: the next step due gets it)
        pend_up |= ~b & last_buttons;      // (released: the same)
        last_buttons = b;
        uint64_t now = plat_now_ns();
        acc_ns += now - last_ns, last_ns = now;
    }
    if (acc_ns > 4 * PERIOD) acc_ns = 4 * PERIOD;  // (a very slow frame: lost logic, not a spiral)
    int steps = (int)(acc_ns / PERIOD);
    if (steps > 3) steps = 3;
    acc_ns -= (uint64_t)steps * PERIOD;
    steps_run += (uint64_t)steps;
    return steps;
}

// (tools/game_overrides.c, before each step) the snapshot this step sees: the first one, the real snapshot with the
// edges latched since the previous step; the rest, the real one as is
void rt_battle_step_view(int k, uint64_t npad, uint64_t *b_set, uint64_t *p_set) {
    uint64_t b = LD64(npad + 8), p = LD64(npad + 0x28);
    if (k == 0 && (pend_down || pend_up)) {
        *b_set = (b & ~pend_up) | pend_down;   // (pressed even if already released: as on that console frame)
        *p_set = (p & ~pend_down) | pend_up;
        pend_down = pend_up = 0;               // (entregados)
    } else { *b_set = b, *p_set = p; }
}

// (tools/game_overrides.c, on every call to update_common, on the original path too) counts the actual calls: they must
// match the planned steps, and be ~60 per second of battle
void rt_battle_step_note(void) { update_calls++; }

// (tools/game_overrides.c) BattleManager's statics, saved when the battle starts
uint64_t rt_battle_statics(void) { return bm_statics; }
