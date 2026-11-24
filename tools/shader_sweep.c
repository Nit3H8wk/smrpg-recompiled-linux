// tools/shader_sweep.c: the shader translator against all of the game's shaders, without playing. Reads the packs
// nxnative extracts (dm/gfx/tools/shader_split.py: records of "BTCDBTCD" + 8-byte key + header up to 0x38 with the
// control and code lengths at 0x28, then the code and the control), translates each shader to GLSL
// (runtime/nvn/maxwell/translate.c) and compiles it to SPIR-V with shaderc like the renderer. Counts the failures by
// reason and writes a sample of each (and its code in <output>/<key>.bin, to disassemble it separately).
// Build (MSYS2 UCRT64, in the repository folder):
//   clang -O2 -Iruntime -o build/shader_sweep.exe tools/shader_sweep.c runtime/nvn/maxwell/translate.c -lshaderc_shared
// Usage: build/shader_sweep.exe <packs folder> <output folder> [max per pack]
#include <dirent.h>
#include <shaderc/shaderc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double t_trad, t_comp;

#include "nvn/maxwell/shader.h"

typedef struct { char why[160]; int n; char key[17]; } Fail;
static Fail fails[4096];
static int nfails;

static const char *g_pkg = "";
static void note_fail(const char *why, const char *key, const uint8_t *code, uint32_t len, const char *out) {
    char w[160];
    snprintf(w, sizeof w, "%s", why);
    for (char *p = w; *p; p++) if (*p == '\n' || *p == '\r') { *p = 0; break; }
    fprintf(stderr, "[fail] %s %s: %s\n", g_pkg, key, w);
    // (grouping: an unknown instruction by its opcode, the top 12 bits; the rest without the register and offset
    // digits, which change from one shader to another)
    char g[160];
    if (!strncmp(w, "unknown instruction ", 20) && strlen(w) >= 23) snprintf(g, sizeof g, "unknown instruction %.3s...", w + 20);
    else {
        size_t k = 0;
        for (const char *p = w; *p && k + 1 < sizeof g; p++)
            if (!((*p >= '0' && *p <= '9') || (*p == 'x' && p > w && p[-1] == '0'))) g[k++] = *p;
        g[k] = 0;
    }
    for (int i = 0; i < nfails; i++)
        if (!strcmp(fails[i].why, g)) { fails[i].n++; return; }
    if (nfails == 4096) return;
    Fail *f = &fails[nfails++];
    snprintf(f->why, sizeof f->why, "%s", g);
    snprintf(f->key, sizeof f->key, "%s", key);
    f->n = 1;
    char path[600];
    snprintf(path, sizeof path, "%s/%s.bin", out, key);
    FILE *b = fopen(path, "wb");
    if (b) { fwrite(code, 1, len, b); fclose(b); }
    fprintf(stderr, "[new] %s (%s): %s\n", key, w, g);
}

static shaderc_shader_kind kind_of(int st) {
    return st == STAGE_VERTEX ? shaderc_vertex_shader : st == STAGE_FRAGMENT ? shaderc_fragment_shader
         : st == STAGE_GEOMETRY ? shaderc_geometry_shader : shaderc_compute_shader;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: shader_sweep <bundles> <output> [maximum per bundle]\n"); return 2; }
    int maxper = argc > 3 ? atoi(argv[3]) : 0;
    mkdir(argv[2]);
    DIR *d = opendir(argv[1]);
    if (!d) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    shaderc_compiler_t comp = shaderc_compiler_initialize();
    long total = 0, ok = 0, bad_trans = 0, bad_glsl = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *n = e->d_name;
        size_t ln = strlen(n);
        if (ln < 5 || strcmp(n + ln - 4, ".bin")) continue;
        int st = !strncmp(n, "vs_", 3) ? STAGE_VERTEX : (!strncmp(n, "ps_", 3) || !strncmp(n, "fs_", 3)) ? STAGE_FRAGMENT
               : !strncmp(n, "cs_", 3) ? STAGE_COMPUTE : !strncmp(n, "gs_", 3) ? STAGE_GEOMETRY : -1;
        if (st < 0) { fprintf(stderr, "(skipping %s: unknown stage)\n", n); continue; }
        char path[600];
        snprintf(path, sizeof path, "%s/%s", argv[1], n);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc((size_t)sz + 16);
        if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); continue; }
        fclose(f);
        int done = 0;
        for (long o = 0; o + 0x38 <= sz;) {
            if (memcmp(buf + o, "BTCDBTCD", 8)) { o++; continue; }
            char key[17];
            for (int i = 0; i < 8; i++) snprintf(key + 2 * i, 3, "%02x", buf[o + 8 + i]);
            uint32_t ctl = *(uint32_t *)(buf + o + 0x28), cl = *(uint32_t *)(buf + o + 0x2c);
            const uint8_t *code = buf + o + 0x38;
            o += 0x38 + cl + ctl;
            g_pkg = n;
            if (maxper && done >= maxper) continue;
            done++, total++;
            ShaderKey k = {0};
            k.glsl = 1;
            k.vs_out_mask = 0xffffffffu;
            // (like the renderer: the code ends where the compiler's constants begin, control + 0x700; nvn/objects.c
            // const_offset; without it, the translator read the constants as instructions)
            uint32_t coff = ctl >= 0x704 ? *(const uint32_t *)(code + cl + 0x700) : 0;
            size_t maxs = coff && coff <= cl ? coff : cl;
            double t0 = now_ms();
            ShaderInfo *si = maxwell_translate(code, maxs, st, &k);
            t_trad += now_ms() - t0;
            if (!si || !si->msl) {
                bad_trans++;
                note_fail(si && si->error[0] ? si->error : "translation without a message", key, code, cl, argv[2]);
                if (si) maxwell_free(si);
                continue;
            }
            shaderc_compile_options_t opt = shaderc_compile_options_initialize();
            shaderc_compile_options_set_target_env(opt, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
            double t1 = now_ms();
            shaderc_compilation_result_t r = shaderc_compile_into_spv(comp, si->msl, strlen(si->msl), kind_of(st), key, "main", opt);
            t_comp += now_ms() - t1;
            if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
                bad_glsl++;
                char why[200];
                const char *m = shaderc_result_get_error_message(r);
                const char *c2 = strstr(m, "error:");
                snprintf(why, sizeof why, "GLSL: %s", c2 ? c2 + 6 : m);
                note_fail(why, key, code, cl, argv[2]);
                char gp[600];
                snprintf(gp, sizeof gp, "%s/%s.glsl", argv[2], key);
                FILE *g = fopen(gp, "wb");
                if (g) { fputs(si->msl, g); fputs("\n/* ", g); fputs(m, g); fputs(" */\n", g); fclose(g); }
            } else ok++;
            shaderc_result_release(r);
            shaderc_compile_options_release(opt);
            maxwell_free(si);
            if (total % 2000 == 0) fprintf(stderr, "... %ld shaders (%ld ok)\n", total, ok);
        }
        free(buf);
    }
    closedir(d);
    printf("time: translating %.0f ms (%.2f ms per shader), compiling GLSL %.0f ms (%.2f ms per shader)\n", t_trad, t_trad / (total ? total : 1), t_comp, t_comp / (total ? total : 1));
    printf("shaders %ld: ok %ld, translation failures %ld, GLSL compilation failures %ld\n", total, ok, bad_trans, bad_glsl);
    // from most to least frequent
    for (int i = 0; i < nfails; i++)
        for (int j = i + 1; j < nfails; j++)
            if (fails[j].n > fails[i].n) { Fail t = fails[i]; fails[i] = fails[j]; fails[j] = t; }
    for (int i = 0; i < nfails; i++) printf("%6d  %s  (e.g. %s)\n", fails[i].n, fails[i].why, fails[i].key);
    return 0;
}
