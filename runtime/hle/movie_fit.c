// (SMRPG) Movies (the CRI Sofdec videos) on a screen wider (or taller) than 16:9.
//
// The game plays them with MoviePlayer: its RawImage m_image (+0x18), with the CRI material that paints each video
// frame, and on top of it its Image m_fade_image (+0x50), the fade to black; both are 1280x720 16:9 boxes that fill
// their canvas (m_canvas, +0x48, 1280x720 reference) on the console. In widescreen (hle/unity_screen.c) the canvas
// takes up the whole screen, but those two stay in the central 16:9: the movie with bars at the sides (and whatever is
// behind showing in them) and the fades not reaching the sides.
// "Widescreen movies" (Menu > Video > Display):
//   - Black bars: the whole movie in the center (the sides, black with "True black": vulkan/exec.c lut_black_fix);
//   - Fill the screen: the RawImage grows at the movie's aspect ratio until it covers the canvas (on 21:9 it is cropped
//     at the top and bottom; on 16:10, at the sides). Nothing is stretched. Movies only: the looping videos (the title
//     background, movie_017.usm, with the logo right at the top and the castle at the bottom, which were being cut off)
//     stay in their 16:9. The loop is the _loop (+0x80) of its CriManaMovieControllerForUI (m_player, +0x20); its file,
//     _moviePath (+0x78), goes to the log with each new video (movie_001.usm: the opening, no loop).
// The fade covers the whole canvas in both modes, so that it darkens the sides too.
// MoviePlayer.Update (main 0x7001c14dc0) is only called through a pointer (Unity, every frame while a MoviePlayer is
// active): hle_override wraps it and, after the original, the sizes are adjusted with
// RectTransform.SetSizeWithCurrentAnchors (it grows on both sides, from the center). The canvas size in its own units
// comes from Canvas.pixelRect / Canvas.scaleFactor. The base size is the one it has the first time it is seen as a 16:9
// box spanning the full height (or width) of the canvas; if the game changes it on its own, the new one becomes the
// base. That way changing the setting, the window's aspect ratio or widescreen takes effect on the fly.
// LARECOMP_LOG_MOVIE_FIT=1: the sizes it sees (the first few times).
//
// (SMRPG 0.49) The transition fade, GS.Fade (the black when entering an area from the map, when entering a house...),
// has the same problem: its Image m_image (+0x30) is a 16:9 box spanning the full height of the canvas, and on a wide
// screen the sides were left uncovered: you could see the camera's background color while the area loads (khaki) or the
// new area too early (seen in a video of a test on 2026-11-17; test route: the save in wld00, the map, and A).
// GS.Fade.Update (main 0x7001959d70, only through a pointer) is wrapped the same way: after the original, the Image
// covers its whole canvas (Graphic.get_canvas). At 16:9 nothing changes.
//
// (SMRPG 0.49.7) The title -> movie fade to white and the scene transitions don't use GS.Fade: they go through
// FadeWithEffect, whose quad is a DrawCaptureImage (m_capture, +0x38) with two GameObjects (m_bg +0x28, m_fade +0x20)
// on its own canvas (m_canvas, +0x58); and the transition from the field into battle is EncountEffect (m_img, +0x20).
// In widescreen they stayed in the central 16:9 (measured at 1720x720: bars at 160-168 versus the center's 193 during
// the fade). DrawCaptureImage.Update (main 0x7001d92100) and EncountEffect.Update (main 0x7001d92580) are wrapped just
// like GS.Fade: after the original, the quads cover their whole canvas. The GameObjects come through
// GameObject.get_transform: their own transform is the quad's rectangle. At 16:9 nothing changes.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"

#define MP_UPDATE 0x7001c14dc0ull               // MoviePlayer.Update
#define GRAPHIC_RECT_TRANSFORM 0x7001c459f0ull  // Graphic.get_rectTransform
#define RT_GET_RECT 0x70017a5670ull             // RectTransform.get_rect -> (x, y, width, height) in s0-s3
#define RT_SET_SIZE 0x70017a6f20ull             // RectTransform.SetSizeWithCurrentAnchors(Axis axis, float size)
#define CANVAS_PIXEL_RECT 0x70022fafd0ull       // Canvas.get_pixelRect -> (x, y, width, height) in pixels, in s0-s3
#define CANVAS_SCALE 0x70022fb080ull             // Canvas.get_scaleFactor -> s0
#define FADE_UPDATE 0x7001959d70ull             // GS.Fade.Update
#define GRAPHIC_CANVAS 0x7001c451f0ull          // Graphic.get_canvas
#define DCI_UPDATE 0x7001d92100ull              // DrawCaptureImage.Update (its own quad in other scenes)
#define FWE_UPDATE 0x7001d935a0ull              // FadeWithEffect.Update (the title -> movie fade to white)
#define ENC_UPDATE 0x7001d92580ull              // EncountEffect.Update (the transition from the field into battle)
#define GO_TRANSFORM 0x700179b510ull            // GameObject.get_transform

static GuestFn mp_update, f_rect_transform, f_get_rect, f_set_size, f_pixel_rect, f_scale, fade_update_orig, f_canvas;
static GuestFn dci_update_orig, enc_update_orig, fwe_update_orig, f_go_transform;
static int dbg = -1;

static float sreg(Cpu *c, int n) {
    float f;
    memcpy(&f, &c->v[n], 4);
    return f;
}
static void set_sreg(Cpu *c, int n, float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    c->v[n] = b;
}
static void gcall(Cpu *c, GuestFn f) {
    c->x[30] = MP_UPDATE;  // (a game address as the return address, for traces)
    f(c);
}
static bool alive(uint64_t o) { return o && LD64(o + 0x10); }  // (a UnityEngine.Object with its native object: m_CachedPtr)

// The canvas size in its own units (those of the RectTransforms inside it); false if it can't be determined
static bool canvas_size(Cpu *c, uint64_t canvas, float *cw, float *ch) {
    if (!alive(canvas)) return false;
    c->x[0] = canvas, c->x[1] = 0;
    gcall(c, f_pixel_rect);
    float pw = sreg(c, 2), ph = sreg(c, 3);
    c->x[0] = canvas, c->x[1] = 0;
    gcall(c, f_scale);
    float s = sreg(c, 0);
    if (!(pw > 0 && ph > 0 && s > 0)) return false;
    *cw = pw / s, *ch = ph / s;
    return true;
}

static void set_size(Cpu *c, uint64_t rt, int axis, float v) {
    c->x[0] = rt, c->x[1] = (uint32_t)axis, c->x[2] = 0;
    set_sreg(c, 0, v);
    gcall(c, f_set_size);
}

enum { FIT_MOVIE, FIT_COVER, FIT_FADE };  // (FIT_FADE: like FIT_COVER, the GS.Fade fade, with its own log line)
typedef struct { uint64_t obj; float w0, h0, w1, h1; } Fit;  // (the base size and the last one set)
static Fit fits[16];
static int nfit, next_slot;
RT_ON_RESET(movie_fit_reset) { nfit = 0, next_slot = 0; }  // (objects from before an in-process restart of the game)

static void fit_rt(Cpu *c, uint64_t g, uint64_t rt, int kind, float cw, float ch, bool loop);
static void fit(Cpu *c, uint64_t g, int kind, float cw, float ch, bool loop) {
    if (!alive(g)) return;
    c->x[0] = g, c->x[1] = 0;
    gcall(c, f_rect_transform);
    uint64_t rt = c->x[0];
    if (!alive(rt)) return;
    fit_rt(c, g, rt, kind, cw, ch, loop);
}

// The body of the fit, with the RectTransform already resolved (DrawCaptureImage's quads come as GameObjects: their own
// transform IS the quad's rectangle, without going through Graphic.get_rectTransform)
static void fit_rt(Cpu *c, uint64_t g, uint64_t rt, int kind, float cw, float ch, bool loop) {
    c->x[0] = rt, c->x[1] = 0;
    gcall(c, f_get_rect);
    float w = sreg(c, 2), h = sreg(c, 3);
    const double a = 16.0 / 9.0, ca = (double)cw / ch;
    bool wide = ca > a * 1.01, tall = ca < a * 0.99;
    // (a 16:9 box that fills the canvas vertically on a wide screen, or horizontally on a taller one)
    bool box = h > 0 && fabs(w / h - a) < 0.01 * a && (tall ? fabsf(w - cw) < 0.02f * cw : fabsf(h - ch) < 0.02f * ch);
    if (dbg > 0) dbg--, LOG("[video] %s %#llx: %.1fx%.1f on a %.1fx%.1f canvas\n", kind == FIT_MOVIE ? "image" : "fade",
                            (unsigned long long)g, w, h, cw, ch);
    int k = -1;
    for (int i = 0; i < nfit; i++) if (fits[i].obj == g) k = i;
    if (k < 0) {
        if (!box || (!wide && !tall)) return;
        if (nfit < (int)(sizeof fits / sizeof fits[0])) k = nfit++;
        else k = next_slot, next_slot = (next_slot + 1) % (int)(sizeof fits / sizeof fits[0]);  // (the oldest one)
        fits[k] = (Fit){g, w, h, w, h};
    } else if (fabsf(w - fits[k].w1) > 1 || fabsf(h - fits[k].h1) > 1) {  // (the game changed it: that's the base size)
        if (!box) {
            fits[k] = fits[--nfit];
            if (next_slot >= nfit) next_slot = 0;
            return;
        }
        fits[k].w0 = w, fits[k].h0 = h;
    }
    float tw = fits[k].w0, th = fits[k].h0;
    // (the base size must fill the current canvas: when the video changes, one frame gives the canvas without its
    // scale, 2560x1080 instead of 1707x720, and with it the video came out 1.5 times larger for that frame)
    if (tall ? fabsf(tw - cw) > 0.02f * cw : fabsf(th - ch) > 0.02f * ch) return;
    if (kind == FIT_COVER || kind == FIT_FADE) {  // the fade: the whole canvas
        if (wide || tall) tw = cw, th = ch;
    } else if (g_settings.movie_fit && !loop) {  // the video: cover the canvas at its aspect (not the title background)
        if (wide) tw = cw, th = (float)(cw / a);
        else if (tall) th = ch, tw = (float)(ch * a);
    }
    bool change = fabsf(tw - w) > 0.5f || fabsf(th - h) > 0.5f;
    if (fabsf(tw - w) > 0.5f) set_size(c, rt, 0, tw);
    if (fabsf(th - h) > 0.5f) set_size(c, rt, 1, th);
    if (change && kind == FIT_MOVIE) {
        static int nlog;
        if (nlog++ < 12)
            LOG("[video] %s: %.0fx%.0f -> %.0fx%.0f (%.0fx%.0f canvas)\n",
                g_settings.movie_fit && !loop && (wide || tall) ? "fill the screen" : "in its 16:9", w, h, tw, th, cw, ch);
    }
    if (change && kind == FIT_FADE) {
        static int nlog;
        if (nlog++ < 6) LOG("[fade]: %.0fx%.0f -> %.0fx%.0f (the whole canvas)\n", w, h, tw, th);
    }
    fits[k].w1 = tw, fits[k].h1 = th;
}

static void movie_update(Cpu *c) {
    uint64_t self = c->x[0], ret = c->x[30];
    mp_update(c);
    float cw, ch;
    bool loop = false;
    if (alive(self)) {  // (one line per new video: its file and whether it loops)
        static uint64_t last_path;
        uint64_t pl = LD64(self + 0x20), path = pl ? LD64(pl + 0x78) : 0;
        loop = pl && (LD8(pl + 0x80) & 1);  // (CriManaMovieMaterial._loop: the title background, movie_017)
        if (path != last_path) {
            last_path = path;
            char s[96] = {0};
            int n = path ? (int)LD32(path + 0x10) : 0;
            for (int i = 0; i < n && i < 95; i++) s[i] = (char)LD16(path + 0x14 + 2 * i);
            LOG("[video] %s, loop %d\n", s, pl ? LD8(pl + 0x80) : -1);
        }
    }
    if (alive(self) && canvas_size(c, LD64(self + 0x48), &cw, &ch)) {
        uint64_t img = LD64(self + 0x18), fade = LD64(self + 0x50);
        if (img) fit(c, img, FIT_MOVIE, cw, ch, loop);
        if (fade) fit(c, fade, FIT_COVER, cw, ch, loop);
    }
    c->x[30] = ret;
}

// GS.Fade.Update: after the original, its Image (m_image, +0x30) covers its whole canvas
static void fade_update(Cpu *c) {
    uint64_t self = c->x[0], ret = c->x[30];
    fade_update_orig(c);
    uint64_t img = alive(self) ? LD64(self + 0x30) : 0;
    if (alive(img)) {
        c->x[0] = img, c->x[1] = 0;
        gcall(c, f_canvas);
        uint64_t canvas = c->x[0];
        float cw, ch;
        if (canvas_size(c, canvas, &cw, &ch)) fit(c, img, FIT_FADE, cw, ch, false);
    }
    c->x[30] = ret;
}

// DrawCaptureImage.Update (FadeWithEffect's quad: the title -> movie fade to white and other scene transitions; also
// GS.TestFade): after the original, its two quads (m_bg +0x28 and m_fade +0x20, GameObjects: their own transform is the
// quad's rectangle) cover its whole canvas (m_canvas, +0x58)
static void dci_update(Cpu *c) {
    uint64_t self = c->x[0], ret = c->x[30];
    dci_update_orig(c);
    if (dbg > 0) dbg--, LOG("[fade] DCI %#llx: bg %#llx fade %#llx canvas %#llx\n",
                            (unsigned long long)self, (unsigned long long)LD64(self + 0x28),
                            (unsigned long long)LD64(self + 0x20), (unsigned long long)LD64(self + 0x58));
    float cw, ch;
    if (!alive(self) || !canvas_size(c, LD64(self + 0x58), &cw, &ch)) {
        c->x[30] = ret;
        return;
    }
    for (int k = 0; k < 2; k++) {
        uint64_t go = LD64(self + (k ? 0x28 : 0x20));
        if (!alive(go)) continue;
        c->x[0] = go, c->x[1] = 0;
        gcall(c, f_go_transform);
        uint64_t rt = c->x[0];
        if (alive(rt)) fit_rt(c, go, rt, FIT_FADE, cw, ch, false);
    }
    c->x[30] = ret;
}

// EncountEffect.Update (the transition when entering battle from the field): after the original, its m_img (+0x20)
// covers its whole canvas
static void enc_update(Cpu *c) {
    uint64_t self = c->x[0], ret = c->x[30];
    enc_update_orig(c);
    uint64_t img = alive(self) ? LD64(self + 0x20) : 0;
    if (alive(img)) {
        c->x[0] = img, c->x[1] = 0;
        gcall(c, f_canvas);
        uint64_t canvas = c->x[0];
        float cw, ch;
        if (canvas_size(c, canvas, &cw, &ch)) fit(c, img, FIT_FADE, cw, ch, false);
    }
    c->x[30] = ret;
}

// FadeWithEffect.Update (the title -> movie fade to white and the scene transitions): after the original, the quad
// (m_capture, +0x38: a DrawCaptureImage) covers its whole canvas with its two GameObjects (m_bg +0x28 and m_fade +0x20;
// the GameObject's transform is the quad's rectangle). DrawCaptureImage.Update only runs in other scenes; on this path,
// this is the one that gets called.
static void fwe_update(Cpu *c) {
    uint64_t self = c->x[0], ret = c->x[30];
    fwe_update_orig(c);
    if (dbg > 0) dbg--, LOG("[fade] FWE %#llx: capture %#llx\n",
                            (unsigned long long)self, (unsigned long long)(alive(self) ? LD64(self + 0x38) : 0));
    uint64_t dci = alive(self) ? LD64(self + 0x38) : 0;
    float cw, ch;
    if (!alive(dci) || !canvas_size(c, LD64(dci + 0x58), &cw, &ch)) {
        c->x[30] = ret;
        return;
    }
    for (int k = 0; k < 2; k++) {
        uint64_t go = LD64(dci + (k ? 0x28 : 0x20));
        if (!alive(go)) continue;
        c->x[0] = go, c->x[1] = 0;
        gcall(c, f_go_transform);
        uint64_t rt = c->x[0];
        if (alive(rt)) fit_rt(c, go, rt, FIT_FADE, cw, ch, false);
    }
    c->x[30] = ret;
}

// (main.c, after dispatch_init: before the game runs)
void smrpg_movie_fit_init(void) {
    dbg = getenv("LARECOMP_LOG_MOVIE_FIT") ? 40 : 0;
    mp_update = dispatch_lookup(MP_UPDATE);
    f_rect_transform = dispatch_lookup(GRAPHIC_RECT_TRANSFORM);
    f_get_rect = dispatch_lookup(RT_GET_RECT);
    f_set_size = dispatch_lookup(RT_SET_SIZE);
    f_pixel_rect = dispatch_lookup(CANVAS_PIXEL_RECT);
    f_scale = dispatch_lookup(CANVAS_SCALE);
    fade_update_orig = dispatch_lookup(FADE_UPDATE);
    f_canvas = dispatch_lookup(GRAPHIC_CANVAS);
    dci_update_orig = dispatch_lookup(DCI_UPDATE);
    enc_update_orig = dispatch_lookup(ENC_UPDATE);
    fwe_update_orig = dispatch_lookup(FWE_UPDATE);
    f_go_transform = dispatch_lookup(GO_TRANSFORM);
    if (!mp_update || !f_rect_transform || !f_get_rect || !f_set_size || !f_pixel_rect || !f_scale) {
        LOG("[video] game functions missing: movies stay in their 16:9\n");
        return;
    }
    static bool registered;  // (boot() runs again on an in-process restart; hle_override only adds)
    if (!registered) {
        registered = true;
        hle_override(MP_UPDATE, movie_update, "MoviePlayer.Update");
        if (fade_update_orig && f_canvas) hle_override(FADE_UPDATE, fade_update, "GS.Fade.Update");
        else LOG("[fade] game functions missing: the transitions' fade stays in its 16:9\n");
        if (dci_update_orig && f_go_transform) hle_override(DCI_UPDATE, dci_update, "DrawCaptureImage.Update");
        if (fwe_update_orig && f_go_transform) hle_override(FWE_UPDATE, fwe_update, "FadeWithEffect.Update");
        if (enc_update_orig && f_canvas) hle_override(ENC_UPDATE, enc_update, "EncountEffect.Update");
    }
}
