// (SMRPG) Game functions that the runtime replaces inside lasgame.dll without regenerating the code: this object is
// linked first, with --allow-multiple-definition (Makefile), so its definition wins over the generated one for every
// call, including those inside the same generated file and the indirect jump table.
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"

// runtime/hle/unity_screen.c (lo exporta smrpg-recomp.exe)
extern void rt_unity_screen_size(int32_t *w, int32_t *h);

// main 0x70006b7204 (UnityPlayer, Switch): Unity's screen resolution. In the game: 1280x720, or 1920x1080 depending on
// the operation mode (TV) and the performance mode, through a Unity player setting (+0x1c of the object from
// 0x7000631100). The runtime is always in TV mode: 1920x1080, widened to the aspect ratio of the player's display if
// the "Widescreen" setting asks for it (rt_unity_screen_size). (x0, x1) = (int *width, int *height).
void f_70006b7204(Cpu *c) {
    int32_t *w = (int32_t *)(uintptr_t)c->x[0], *h = (int32_t *)(uintptr_t)c->x[1];
    *w = 1920;
    *h = 1080;
    rt_unity_screen_size(w, h);
}

// main 0x700062f1d0 (UnityPlayer, the Switch screen manager): SetResolution(width, height). The same as the original
// except for two things, so that the game's screen can follow the player's display on the fly (21:9, 16:10...):
//  - the original stores the size clamped to the Switch's (+0xc0 = min(width, 1920), +0xc4 = min(height, 1080): what
//    Screen.width / height read): with 2560x1080 requested on the fly, the scene stayed 1920 wide and the runtime
//    stretched it; here it is stored whole, as at startup (that path doesn't clamp);
//  - the original sets +0xc9 ("a custom resolution was applied") for any size that isn't 1280x720 or 1920x1080, and
//    with it set its mode change handlers no longer do anything; here it isn't set.
// (and without its "Requested resolution ... is not a 16:9 aspect ratio" warning). It is used by the handler of the
// performance mode change notification (0x700062ed24, vtable +0x30), which runtime/hle/unity_screen.c triggers when the
// window's aspect ratio changes.
void f_7000de69c0(Cpu *c);
void f_700057bef0(Cpu *c);
void f_7000237cf0(Cpu *c);
void f_7000256b4c(Cpu *c);
static uint32_t display_mode_now(Cpu *c) {  // (the current display mode entry: +0x30 + index +0x4c * 0xa8, +0x64)
    f_7000de69c0(c);
    uint64_t d = c->x[0];
    return LD32(LD64(d + 0x30) + (uint64_t)((int64_t)(int32_t)LD32(d + 0x4c) * 0xa8) + 0x64);
}
void f_700062f1d0(Cpu *c) {
    uint64_t self = c->x[0], ret = c->x[30];
    uint32_t w = (uint32_t)c->x[1], h = (uint32_t)c->x[2];
    uint32_t mode_before = LD32(self + 0xcc);
    c->x[30] = 0x700062f1fcull;
    uint32_t mode_new = display_mode_now(c);
    bool mode_changed = mode_before != mode_new, size_changed = true;
    c->x[0] = self, c->pc = LD64(LD64(self) + 0xb0), c->x[30] = 0x700062f228ull;  // (the current width)
    rt_dispatch(c);
    if ((uint32_t)c->x[0] == w) {
        c->x[0] = self, c->pc = LD64(LD64(self) + 0xb8), c->x[30] = 0x700062f240ull;  // (the current height)
        rt_dispatch(c);
        size_changed = (uint32_t)c->x[0] != h;
        if (!mode_changed && !size_changed) {
            c->x[0] = 0, c->x[30] = ret;
            return;
        }
    }
    c->x[30] = 0x700062f2bcull;
    f_700057bef0(c);
    uint64_t g = c->x[0];
    c->x[0] = g, c->pc = LD64(LD64(g) + 0x6c0), c->x[30] = 0x700062f2c8ull;
    rt_dispatch(c);
    ST32(self + 0xc0, w);  // (without the clamp to 1920x1080)
    ST32(self + 0xc4, h);
    c->x[30] = 0x700062f2ecull;
    ST32(self + 0xcc, display_mode_now(c));
    ST8(self + 0xc9, 0);  // (never "custom")
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    ST8(self + 0xc8, size_changed);
    ST8(self + 0xd0, mode_changed);
    c->x[30] = 0x700062f344ull;
    f_7000237cf0(c);
    c->x[30] = 0x700062f348ull;
    f_7000256b4c(c);
    c->x[0] = 1, c->x[30] = ret;
    static int logged;
    if (logged++ < 8) fprintf(stderr, "[unity] SetResolution %ux%u (no cropping)\n", w, h);
}

// ---------------------------------------------------------------- the UI in widescreen
// main 0x7001fdb520 = UnityEngine.UI.CanvasScaler.HandleScaleWithScreenSize (Unity 2021.3's uGUI; names and addresses
// from the IL2CPP dump). It does the same as the original (uGUI's open-source code) with one difference: the screen
// size first goes through rt_unity_ui_scale_screen (runtime/hle/unity_screen.c), which on a screen wider (or taller)
// than 16:9 gives the 16:9 that fits inside. The branch for secondary displays (targetDisplay > 0) isn't here: the
// Switch has one.
//   fields (CanvasScaler): 0x1c m_ReferencePixelsPerUnit, 0x24 m_ReferenceResolution (x, y), 0x2c m_ScreenMatchMode
//   (0 width or height, 1 expand, 2 shrink), 0x30 m_MatchWidthOrHeight, 0x48 m_Canvas, 0x50 m_PrevScaleFactor,
//   0x54 m_PrevReferencePixelsPerUnit.
extern void rt_unity_ui_scale_screen(float *w, float *h);
void f_70022fb850(Cpu *c);  // Canvas.get_renderingDisplaySize: Vector2 en (s0, s1)
void f_70022fb0d0(Cpu *c);  // Canvas.set_scaleFactor(s0)
void f_70022fb170(Cpu *c);  // Canvas.set_referencePixelsPerUnit(s0)

static inline float sreg(Cpu *c, int n) {
    float f;
    memcpy(&f, &c->v[n], 4);
    return f;
}
static inline void set_sreg(Cpu *c, int n, float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    c->v[n] = b;
}
static inline float ldf(uint64_t a) {
    float f;
    memcpy(&f, (const void *)(uintptr_t)a, 4);
    return f;
}
static inline void stf(uint64_t a, float f) { memcpy((void *)(uintptr_t)a, &f, 4); }

// (diagnostics, LARECOMP_LOG_CANVAS=1) the object name of each CanvasScaler the first time it is scaled:
// UnityEngine.Object.get_name (main 0x700179fc20) returns a System.String (length at +0x10, UTF-16 at +0x14)
void f_700179fc20(Cpu *c);
static void log_canvas(Cpu *c, uint64_t self, float sw, float sh, float scale) {
    static int on = -1;
    static uint64_t seen[256];
    static int nseen;
    if (on < 0) on = getenv("LARECOMP_LOG_CANVAS") != NULL;
    if (!on || nseen >= 256) return;
    for (int i = 0; i < nseen; i++) if (seen[i] == self) return;
    seen[nseen++] = self;
    uint64_t lr = c->x[30];
    c->x[0] = self, c->x[1] = 0, c->x[30] = 0x7001fdb520ull;
    f_700179fc20(c);
    c->x[30] = lr;
    char name[96] = "?";
    uint64_t s = c->x[0];
    if (s) {
        int32_t n = (int32_t)LD32(s + 0x10), k = 0;
        for (int32_t i = 0; i < n && k < 95; i++) {
            uint16_t ch = *(const uint16_t *)(uintptr_t)(s + 0x14 + 2 * (uint64_t)i);
            name[k++] = ch < 128 ? (char)ch : '?';
        }
        name[k] = 0;
    }
    char line[300];  // (a single write, like the runtime's rt_log: the UCRT is the same one, and so is stderr)
    int len = snprintf(line, sizeof line, "[canvas] %s: ref %.0fx%.0f mode %d match %.2f screen %.0fx%.0f scale %.3f\n",
                       name, ldf(self + 0x24), ldf(self + 0x28), (int)LD32(self + 0x2c), ldf(self + 0x30), sw, sh, scale);
    if (len > 0) fwrite(line, 1, (size_t)len < sizeof line ? (size_t)len : sizeof line - 1, stderr);
}

void f_7001fdb520(Cpu *c) {
    uint64_t self = c->x[0], lr = c->x[30];
    uint64_t canvas = LD64(self + 0x48);
    if (!canvas) return;  // (the original would throw a NullReferenceException in the Canvas method)
    c->x[0] = canvas, c->x[1] = 0, c->x[30] = 0x7001fdb568ull;
    f_70022fb850(c);
    float sw = sreg(c, 0), sh = sreg(c, 1);
    float ow = sw, oh = sh;
    rt_unity_ui_scale_screen(&sw, &sh);
    float rx = ldf(self + 0x24), ry = ldf(self + 0x28), scale = 0;
    switch ((int32_t)LD32(self + 0x2c)) {
    case 0: {  // Mathf.Log(x, 2) in double precision, like Math.Log; Mathf.Lerp with t in [0, 1]; 2 raised to the result
        float lw = (float)(log((double)(sw / rx)) / log(2.0)), lh = (float)(log((double)(sh / ry)) / log(2.0));
        float t = ldf(self + 0x30);
        t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
        scale = exp2f(lw + (lh - lw) * t);
        break;
    }
    case 1: scale = fminf(sw / rx, sh / ry); break;
    case 2: scale = fmaxf(sw / rx, sh / ry); break;
    }
    log_canvas(c, self, ow, oh, scale);
    // SetScaleFactor and SetReferencePixelsPerUnit: only if they change
    if (scale != ldf(self + 0x50)) {
        c->x[0] = LD64(self + 0x48), c->x[1] = 0, c->x[30] = 0x7001fdb754ull;
        set_sreg(c, 0, scale);
        f_70022fb0d0(c);
        stf(self + 0x50, scale);
    }
    float ppu = ldf(self + 0x1c);
    if (ppu != ldf(self + 0x54)) {
        c->x[0] = LD64(self + 0x48), c->x[1] = 0, c->x[30] = 0x7001fdb778ull;
        set_sreg(c, 0, ppu);
        f_70022fb170(c);
        stf(self + 0x54, ppu);
    }
    c->x[30] = lr;
}

// ---------------------------------------------------------------- URP motion vectors
// main 0x7001838ee0 = UniversalRenderer.GetRenderPassInputs(ref RenderingData) (URP 12.1): what the passes in the queue
// ask for (depth, normals, color, motion). With "motion", URP enqueues its MotionVectorRenderPass, which draws
// _MotionVectorTexture (RG16F: the camera's motion, from the depth, and each object's, with its matrices from the
// previous frame). The game never asks for it; the runtime forces it (rt_force_motion_vectors) for the temporal
// upscalers, which need it, along with the depth texture the camera's motion comes from.
// The same as the original (URP's open-source code) in everything else. Fields: UniversalRenderer +0xf8
// m_ActiveRenderPassQueue (List: +0x10 items, +0x18 count), +0x410 m_RenderingMode (1 deferred); ScriptableRenderPass
// +0x10 renderPassEvent, +0xc0 m_Input (1 depth, 2 normals, 4 color, 8 motion).
// Returns RenderPassInputSummary (16 bytes) in x0 (six booleans, bytes 0-5) and x1 (two RenderPassEvent).
extern int rt_force_motion_vectors(void);  // runtime/hle/unity_screen.c
void f_70000c4150(Cpu *c);  // il2cpp: initialize the metadata a method uses (once)
void f_7001838ee0(Cpu *c) {
    uint64_t self = c->x[0];
    if (!(LD8(0x7003cb4bc3ull) & 1)) {  // (the same three as the original, the first time)
        uint64_t lr = c->x[30];
        c->x[0] = LD64(0x7003a547d0ull), c->x[30] = 0x7001838f1cull;
        f_70000c4150(c);
        c->x[0] = LD64(0x7003a56520ull), c->x[30] = 0x7001838f28ull;
        f_70000c4150(c);
        c->x[0] = LD64(0x7003a56528ull), c->x[30] = 0x7001838f34ull;
        f_70000c4150(c);
        ST8(0x7003cb4bc3ull, 1);
        c->x[30] = lr;
    }
    int32_t before_main = (int32_t)LD32(self + 0x410) == 1 ? 210 : 250;  // BeforeRenderingGbuffer / BeforeRenderingOpaques
    bool depth = false, prepass = false, normals = false, color = false, color_created = false, motion = false;
    int32_t normal_at = 250, earliest = 550;  // BeforeRenderingOpaques, BeforeRenderingPostProcessing
    uint64_t list = LD64(self + 0xf8), items = list ? LD64(list + 0x10) : 0;
    int32_t n = list ? (int32_t)LD32(list + 0x18) : 0;
    uint64_t dbuf = LD64(LD64(0x7003a547d0ull));  // (the DBufferRenderPass class)
    for (int32_t i = 0; i < n && items; i++) {
        uint64_t pass = LD64(items + 0x20 + 8 * (uint64_t)i);
        if (!pass) continue;
        uint32_t in = LD32(pass + 0xc0);
        int32_t ev = (int32_t)LD32(pass + 0x10);
        bool nd = in & 1, nn = (in & 2) != 0, nc = (in & 4) != 0, nm = (in & 8) != 0;
        if (dbuf) {  // pass is DBufferRenderPass: its class hierarchy (depth at +0x12c, table at +0xc8)
            uint64_t k = LD64(pass);
            uint8_t kd = LD8(k + 0x12c), dd = LD8(dbuf + 0x12c);
            if (dd && kd >= dd && LD64(LD64(k + 0xc8) + 8 * (uint64_t)dd - 8) == dbuf) color_created = true;
        }
        depth |= nd;
        prepass |= nn || (nd && ev <= before_main);
        normals |= nn;
        color |= nc;
        motion |= nm;
        if (nd && ev < earliest) earliest = ev;
        if ((nn || nd) && ev < normal_at) normal_at = ev;
    }
    if (rt_force_motion_vectors()) motion = depth = true;
    c->x[0] = (uint64_t)depth | (uint64_t)prepass << 8 | (uint64_t)normals << 16 | (uint64_t)color << 24 |
              (uint64_t)color_created << 32 | (uint64_t)motion << 40;
    c->x[1] = (uint64_t)(uint32_t)normal_at | (uint64_t)(uint32_t)earliest << 32;
}

// ---------------------------------------------------------------- screen captures as the background, in widescreen
// The field menus (Menu.StartScreenCapture, coroutine main 0x7001f4dd40) and the minigames (their ScreenCapture) copy
// the whole screen (Screen.width x Screen.height, ReadPixels: 2560x1080 with the screen widened) and set it as the
// background in a RawImage, directly or blurred by GaussianBlur (at half the size, same aspect ratio). That RawImage
// measures 1920x1080 on the canvas, the console's 16:9: in widescreen (the canvas takes up the full width,
// rt_unity_ui_scale_screen) it stayed in the center with the capture squeezed inside it and the canvas's background at
// the sides ("the game shrinks when you open a menu"; measured: horizontal scale 0.75 with correlation 0.988, gray in
// the 320 pixels on each side of 2560x1080).
// main 0x7001e93320 = UnityEngine.UI.RawImage.set_texture: the same as the original (uGUI: if it changes, it stores it
// and marks vertices and material dirty), and then, if the new texture has the aspect ratio of the game's screen and
// the RawImage is a 16:9 box spanning the full height of the canvas (>= 1000 of the 1080 units), its width becomes the
// texture's (RectTransform.SetSizeWithCurrentAnchors, centered: it grows on both sides). With a taller screen (16:10),
// its height. If it later gets another texture, it goes back to its own size.
// (DrawCaptureImage, another capture with its own camera and a 16:9 quad, hasn't been seen in use: only GS.TestFade,
// for testing, and the odd scene FadeWithEffect enable it. Left alone.)
void f_700179f800(Cpu *c);  // UnityEngine.Object.op_Equality
void f_70000c4330(Cpu *c);  // il2cpp: a class's static constructor (once)
void f_70000c4090(Cpu *c);  // il2cpp: the garbage collector's write barrier (field, value)
void f_7001c459f0(Cpu *c);  // Graphic.get_rectTransform
void f_70017a5670(Cpu *c);  // RectTransform.get_rect -> (x, y, width, height) in s0-s3
void f_70017a6f20(Cpu *c);  // RectTransform.SetSizeWithCurrentAnchors(Axis axis, float size)
extern double rt_unity_screen_aspect(void);
static int tex_dim(Cpu *c, uint64_t tex, int slot) {  // Texture.width (vtable +0x188) / height (+0x1a8)
    uint64_t vt = LD64(tex);
    c->x[0] = tex, c->x[1] = LD64(vt + slot + 8), c->pc = LD64(vt + slot), c->x[30] = 0x7001e933f0ull;
    rt_dispatch(c);
    return (int32_t)c->x[0];
}
static void rect_size(Cpu *c, uint64_t rt, int axis, float v) {
    c->x[0] = rt, c->x[1] = (uint32_t)axis, c->x[2] = 0, c->x[30] = 0x7001e933f0ull;
    set_sreg(c, 0, v);
    f_70017a6f20(c);
}
static void capture_image_fit(Cpu *c, uint64_t img, uint64_t tex) {
    static struct { uint64_t img; float w0, h0, w1, h1; } fitted[32];
    static int nfit;
    double ga = rt_unity_screen_aspect(), a169 = 16.0 / 9.0;
    c->x[0] = img, c->x[1] = 0, c->x[30] = 0x7001e933f0ull;
    f_7001c459f0(c);
    uint64_t rt = c->x[0];
    if (!rt || !LD64(rt + 0x10)) return;
    c->x[0] = rt, c->x[1] = 0, c->x[30] = 0x7001e933f0ull;
    f_70017a5670(c);
    float rw = sreg(c, 2), rh = sreg(c, 3);
    int k = -1;
    for (int i = 0; i < nfit; i++) if (fitted[i].img == img) k = i;
    double ta = 0;
    if (tex && LD64(tex + 0x10)) {
        int tw = tex_dim(c, tex, 0x188), th = tex_dim(c, tex, 0x1a8);
        if (tw > 0 && th > 0) ta = (double)tw / th;
    }
    bool wide_screen = fabs(ga - a169) > 0.01 * a169;
    bool screen_tex = ta > 0 && fabs(ta - ga) < 0.02 * ga;
    if (wide_screen && screen_tex) {
        bool box169 = rh > 0 && fabs(rw / rh - a169) < 0.01 * a169;
        if (k < 0 && !(box169 && (ga > a169 ? rh >= 1000 : rw >= 1800))) return;
        if (k >= 0 && (fabsf(rw - fitted[k].w1) > 1 || fabsf(rh - fitted[k].h1) > 1)) return;  // (someone else moved it)
        float w = ga > a169 ? (float)(rh * ta) : rw, h = ga > a169 ? rh : (float)(rw / ta);
        if (k < 0 && nfit < 32) k = nfit++, fitted[k].img = img, fitted[k].w0 = rw, fitted[k].h0 = rh;
        if (k >= 0) fitted[k].w1 = w, fitted[k].h1 = h;
        if (w != rw) rect_size(c, rt, 0, w);
        if (h != rh) rect_size(c, rt, 1, h);
        static int nlog;
        if (nlog++ < 6)
            fprintf(stderr, "[unity] background screenshot: RawImage %.0fx%.0f -> %.0fx%.0f (texture aspect %.3f)\n",
                    rw, rh, w, h, ta);
    } else if (k >= 0) {  // (another texture: back to the previous size, if nobody changed it)
        if (fabsf(rw - fitted[k].w1) <= 1 && fabsf(rh - fitted[k].h1) <= 1) {
            if (fitted[k].w0 != rw) rect_size(c, rt, 0, fitted[k].w0);
            if (fitted[k].h0 != rh) rect_size(c, rt, 1, fitted[k].h0);
        }
        fitted[k] = fitted[--nfit];
    }
}
void f_7001e93320(Cpu *c) {  // RawImage.set_texture(Texture value)
    uint64_t self = c->x[0], value = c->x[1], ret = c->x[30];
    if (!(LD8(0x7003cb7d8full) & 1)) {
        c->x[0] = LD64(0x7003a477b8ull), c->x[30] = 0x7001e9335cull;
        f_70000c4150(c);
        ST8(0x7003cb7d8full, 1);
    }
    uint64_t old = LD64(self + 0xd0);
    uint64_t cls = LD64(LD64(0x7003a477b8ull));  // (UnityEngine.Object: its static constructor)
    if (!LD32(cls + 0xe0)) {
        c->x[0] = cls, c->x[30] = 0x7001e9337cull;
        f_70000c4330(c);
    }
    c->x[0] = old, c->x[1] = value, c->x[2] = 0, c->x[30] = 0x7001e9338cull;
    f_700179f800(c);
    if (c->x[0] & 1) {  // (the same one)
        c->x[30] = ret;
        return;
    }
    ST64(self + 0xd0, value);
    c->x[0] = self + 0xd0, c->x[1] = value, c->x[30] = 0x7001e933b4ull;
    f_70000c4090(c);
    uint64_t vt = LD64(self);  // SetVerticesDirty, SetMaterialDirty (virtuales)
    c->x[0] = self, c->x[1] = LD64(vt + 0x300), c->pc = LD64(vt + 0x2f8), c->x[30] = 0x7001e933c8ull;
    rt_dispatch(c);
    vt = LD64(self);
    c->x[0] = self, c->x[1] = LD64(vt + 0x310), c->pc = LD64(vt + 0x308), c->x[30] = 0x7001e933f0ull;
    rt_dispatch(c);
    capture_image_fit(c, self, value);
    c->x[30] = ret;
}

// ---------------------------------------------------------------- battles at 60 FPS (runtime/hle/battle_rate.c)
// main 0x7001cd0500 = BattleManager.set_frame_rate(int frame_rate), at the start of every battle. The same as the
// original: if it already has a rate, nothing; it saves QualitySettings.vSyncCount (+0x1cc) and
// Application.targetFrameRate (+0x1d0) for reset_frame_rate, sets _frame_rate (+0x1c8) and vSyncCount = 2 for 30 or 1
// for 60. The difference: the rate goes through the runtime (rt_battle_frame_rate: 60, the one the game asks for, one
// logic step per frame), which also keeps the vsync at 60 Hz while it lasts. The class's static fields (so that the
// runtime can see when it ends: _instance at +0x10) come from the object itself: its class (+0x0) and, in it,
// static_fields (+0xb8), the way BattleManager.update reads them.
extern int rt_battle_frame_rate(uint64_t statics, int requested);
void f_7001678140(Cpu *c);  // QualitySettings.get_vSyncCount
void f_7001666000(Cpu *c);  // Application.get_targetFrameRate
void f_7001678180(Cpu *c);  // QualitySettings.set_vSyncCount(int)
void f_7001cd0500(Cpu *c) {
    uint64_t self = c->x[0], ret = c->x[30];
    int requested = (int32_t)c->x[1];
    if (LD32(self + 0x1c8)) return;
    c->x[0] = 0, c->x[30] = 0x7001cd0530ull;
    f_7001678140(c);
    ST32(self + 0x1cc, (uint32_t)c->x[0]);
    c->x[0] = 0, c->x[30] = 0x7001cd053cull;
    f_7001666000(c);
    ST32(self + 0x1d0, (uint32_t)c->x[0]);
    uint64_t klass = LD64(self), statics = klass ? LD64(klass + 0xb8) : 0;
    int rate = rt_battle_frame_rate(statics, requested);
    ST32(self + 0x1c8, (uint32_t)rate);
    if (rate == 30 || rate == 60) {
        c->x[0] = rate == 30 ? 2 : 1, c->x[1] = 0, c->x[30] = 0x7001cd0568ull;
        f_7001678180(c);
    }
    c->x[30] = ret;
}

// main 0x7001cd9250 = BattleManager.update (static; once per frame from the Update of an object in the scene). The
// original: the _instance == null guard and ONE call to update_common, and a second one if _frame_rate is 30 (the debug
// mode, which at a 60 Hz vsync gives 30 frames with two steps). With battle pacing (hle/battle_rate.c): while the
// battle has its rate of 60 set, the vsync ticks at the "FPS limit" and the update_common steps are paced here by real
// time (60 per second: 0, 1, 2 or 3 per frame; 0 doesn't call update_common); on the step that is due, the controller
// snapshot (ControllerObserver._npadState) carries the press/release edges seen since the previous step, so that no
// press is lost between steps. With no battle, with the rate at 0 (the tail end of the battle), with
// LARECOMP_BATTLE_FPS=30 or with LARECOMP_BATTLE_FIX=0 (the plan answers -1): the usual behavior.
extern uint64_t rt_battle_statics(void);
extern int rt_battle_plan(int rate, uint64_t *npad);
extern void rt_battle_step_view(int k, uint64_t npad, uint64_t *b_set, uint64_t *p_set);
extern void rt_battle_step_note(void);
void f_7001cd9360(Cpu *c);  // BattleManager.update_common
void f_7001cd9250(Cpu *c) {
    uint64_t inst = 0, s = rt_battle_statics();
    if (s) inst = LD64(s + 0x10);
    if (!inst) return;  // (the original's guard: _instance == null)
    int rate = (int)LD32(inst + 0x1c8);
    uint64_t npad = 0;
    int n = rt_battle_plan(rate, &npad);
    if (n < 0) {  // (the pacing doesn't apply: like the original, one step per frame, two with a rate of 30)
        rt_battle_step_note();
        c->x[0] = inst, c->x[30] = 0x7001cd92fcull, f_7001cd9360(c);
        if (rate == 30) rt_battle_step_note(), c->x[0] = inst, c->x[30] = 0x7001cd92fcull, f_7001cd9360(c);
        return;
    }
    if (n > 0) {
        uint64_t ob = LD64(npad + 8), op = LD64(npad + 0x28);  // (the real snapshot, to restore it)
        for (int k = 0; k < n; k++) {
            uint64_t b_set, p_set;
            rt_battle_step_view(k, npad, &b_set, &p_set);
            ST64(npad + 8, b_set), ST64(npad + 0x28, p_set);
            rt_battle_step_note();
            c->x[0] = inst, c->x[30] = 0x7001cd92fcull, f_7001cd9360(c);
        }
        ST64(npad + 8, ob), ST64(npad + 0x28, op);
    }
    // (n == 0: no step is due this frame; the edges seen stay latched for the next step due)
}
