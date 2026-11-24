// LAS Recomp entry: load modules, set up the kernel, run nn::init::Start on the main thread.
#include "app_name.h"
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "game_addresses.h"
#include "gamelib.h"
#include "paths.h"
#include "settings.h"
#include "ui/tr.h"
#include "hle.h"
#include "kernel.h"
#include "rando.h"

const char *g_romfs_dir;

// rtld normally passes these to nn::init::Start: run every module's init/fini arrays.
static void run_array(Cpu *c, uint64_t arr, uint64_t size) {
    for (uint64_t i = 0; i < size / 8; i++) {
        uint64_t fn = ((uint64_t *)arr)[i];
        if (fn) guest_call(c, fn);
    }
}

// nn::init::Start (sdk 0xfa3c0): prologue moves x2->x21 and x3->x20, then runs:
// nnosInitialize -> blr x21 (EARLY) -> _init_libc1 -> nninitInitializeSdkModule -> nninitStartup
// (mounts the central heap via StandardAllocator::Initialize) -> _init_libc2 -> blr x20 (LATE)
// -> nnMain -> blr x19.
// Decoded from rtld itself (probe_rtld4/5): rtld's real calls are
//   nn::init::Start(w19, base+0x4000, rtld+0x13cc, rtld+0x860, rtld+0x8b0)
// where x2 (early) is rtld's RELA/REL applier (module list walk; the runtime already applied
// every reloc at load time, so ours is a no-op) and x3 (late) walks the module list calling
// module+0x40 == each module's DT_INIT -- i.e. on hardware ALL module ctors (sdk and main) run
// in the LATE slot, after nninitStartup mounted the heap and _init_libc2 initialized musl.
// That is why both crash signatures (sdk ctor in early, main ctor in early) died in
// CentralHeap::Alloc with central==NULL: no ctor may run before the heap is mounted.
// A module's DT_INIT registers it (and its TLS template) with musl via __nnmusl_init_dso and
// then runs its init_array; fall back to the array if there is no DT_INIT.
static void module_init(Cpu *c, Module *m) {
    LOG("init %s (DT_INIT %llx, %llu init_array entries)\n", m->name, m->init, m->init_array_sz / 8);
    if (m->init) guest_call(c, m->init);
    else run_array(c, m->init_array, m->init_array_sz);
}

static void rtld_init_sdk(Cpu *c) { module_init(c, &g_modules[1]); }
static void rtld_init_main(Cpu *c) { module_init(c, &g_modules[0]); }
static void rtld_noop(Cpu *c) { (void)c; }
// sdk first: main imports sdk globals, and on hardware the sdk module is registered before main.
static void rtld_init_all(Cpu *c) { rtld_init_sdk(c); rtld_init_main(c); }

// Claim the guest address range before anything else can map there. The host allocator places
// regions at random addresses in the same window; on a collision, re-exec for a fresh layout.
// LARECOMP_REEXEC_FORCE=1 (test): the first process skips the reservation once and starts again, as when the range is
// taken (the new process has LARECOMP_REEXEC set and reserves normally).
#define REEXEC_FORCED "forced by LARECOMP_REEXEC_FORCE"
__attribute__((constructor(101))) static void reserve_guest_space(void) {
    const char *force = getenv("LARECOMP_REEXEC_FORCE");
    bool forced = force && *force && strcmp(force, "0") && !getenv("LARECOMP_REEXEC");
    if (forced) setenv("LARECOMP_REEXEC_WHY", REEXEC_FORCED, 1);
    else if (plat_reserve_guest(GUEST_LO, GUEST_HI)) return;
    else setenv("LARECOMP_REEXEC_WHY", "", 0);  // (the platform may have said what holds the range; else nothing more)
    const char *tries = getenv("LARECOMP_REEXEC");
    int n = tries ? atoi(tries) : 0;
    if (n >= 20) {
        fprintf(stderr, "FATAL: guest address range %llx-%llx is not free\n", GUEST_LO, GUEST_HI);
        exit(1);
    }
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n + 1);
    setenv("LARECOMP_REEXEC", buf, 1);
    plat_exec(NULL);
    perror("re-exec");
    exit(1);
}

// ------------------------------------------------------------------ boot / in-place reboot
// Switching seeds (or vanilla) and going back to the start screen reboot the game inside this process: the window,
// GPU device, settings and shader caches stay. rt_request_reboot (menu, any thread) makes ui_run return on the
// main thread, which then: stops every guest thread (kernel_stop_threads: each unwinds at its next svc / HLE call /
// wait), resets the renderer (textures, address-keyed caches) and all per-boot host state (RT_ON_RESET hooks:
// services, HLE features, seed data, audio), empties the kernel, wipes guest memory, and boots again.
static _Atomic int g_reboot;
void rt_request_reboot(int kind) {
    LOG("[reboot] requested (%s)\n", kind == RT_REBOOT_TITLE ? "start screen" : "boot");
    g_reboot = kind;
}
int rt_reboot_requested(void) { return g_reboot; }

// LARECOMP_REBOOT_AT=<frame>:<seed | vanilla | title>[,...] (test): at that presented frame, switch to the seed /
// vanilla in place, or go back to the start screen (with LARECOMP_LAUNCHER_AUTOPLAY it plays the saved choice).
// Frames count on across reboots. Called by the presenter every frame.
void reboot_test_frame(uint64_t frame) {
    static const char *next;
    static int init;
    if (!init) { init = 1; next = getenv("LARECOMP_REBOOT_AT"); }
    long at;
    char what[128];
    if (!next || !*next || sscanf(next, "%ld:%127[^,]", &at, what) != 2 || (long)frame < at) return;
    next = strchr(next, ',');
    if (next) next++;
    if (!strcmp(what, "title")) { rt_request_reboot(RT_REBOOT_TITLE); return; }
    // (a test run: the next boot's seed without saving it - larecomp.ini is the player's choice)
    strlcpy(g_settings.rando_seed, strcmp(what, "vanilla") ? what : "", sizeof g_settings.rando_seed);
    unsetenv("LARECOMP_RANDO_SEED");
    rt_request_reboot(RT_REBOOT_GAME);
}

#ifdef __APPLE__
extern const ResetFn __start_resets[] __asm__("section$start$__DATA$__rt_reset");
extern const ResetFn __stop_resets[] __asm__("section$end$__DATA$__rt_reset");
#elif defined(__linux__)
extern const ResetFn __start_resets[], __stop_resets[];  // (ELF: defined by the linker)
#else
// PE/COFF: .rst$a < .rst$m (entries, hle.h) < .rst$z (the linker sorts by the text after '$')
__attribute__((section(".rst$a"), used)) static const ResetFn __start_resets[1];
__attribute__((section(".rst$z"), used)) static const ResetFn __stop_resets[1];
#endif

void backend_reset(void);             // renderer: GPU idle, game textures and address-keyed caches dropped
void backend_guest_memory_reset(void);  // renderer: guest memory was replaced (re-wrap the heap)
void backend_clear_window(void);      // a black frame while the game is down
void backend_quit(void);              // quitting: the render thread stopped, the GPU idle, caches kept (before exit())

static void reboot_idle(void) {  // (main thread, while guest threads stop) keep the window alive
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_QUIT) { LOG("window closed\n"); backend_quit(); exit(0); }
}

static void teardown(void) {
    uint64_t t0 = plat_now_ns();
    kernel_stop_threads(reboot_idle);
    backend_clear_window();
    backend_reset();
    const ResetFn *start = __start_resets, *stop = __stop_resets;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    int n = 0;
    for (const ResetFn *p = start; p < stop; p++)
        if (*p) { (*p)(); n++; }
    kernel_reset();
    if (!plat_reset_guest(GUEST_LO, GUEST_HI)) FATAL("cannot reset guest memory\n");
    backend_guest_memory_reset();
    g_nmodules = 0;
    g_reboot = RT_REBOOT_NONE;
    LOG("[reboot] game torn down in %llums (%d reset hooks)\n", (unsigned long long)((plat_now_ns() - t0) / 1000000), n);
}

static void boot(const char *root) {
    char exefs[1024], romfs[1024], npdm_path[1024];
    snprintf(exefs, sizeof exefs, "%s/exefs", root);
    snprintf(romfs, sizeof romfs, "%s/romfs", root);
    snprintf(npdm_path, sizeof npdm_path, "%s/main.npdm", exefs);
    if (!g_romfs_dir) g_romfs_dir = strdup(romfs);

#ifdef __linux__
    {   // the window and graphics device first (normally made by the start screen or ui_run), so the guest heap can be
        // the driver's memory before any game thread runs (runtime/nvn/vulkan/exec.c backend_guest_heap_prepare)
        extern void ui_window_init(void);
        extern void backend_guest_heap_prepare(void);
        ui_window_init();
        backend_guest_heap_prepare();
    }
#endif
    char why[512];
    if (!game_version_select(root, why, sizeof why)) FATAL("%s\n", why);  // (the address table: game_addresses.h)
    if (!gamelib_load(root, why, sizeof why)) FATAL("%s\n", why);
    loader_load(exefs);
    rando_boot();  // the chosen seed's data patches go into the freshly loaded module
    { extern void settings_load(void); settings_load(); }
    kernel_init();
    dispatch_init();
    { extern void smrpg_movie_fit_init(void); smrpg_movie_fit_init(); }  // (SMRPG: hle/movie_fit.c, before it runs)

    uint32_t stack_size = 0x100000;
    int prio = 44;
    uint64_t program_id = 0, system_resource = 0;
    FILE *f = fopen(npdm_path, "rb");
    if (f) {
        // SMRPG: besides the main thread's priority and stack, the "system resource" (+0x14, enables nn::os's virtual
        // memory) and the ACI0's program ID (+0x10), for GetInfo.
        static uint8_t npdm[0x4000];
        size_t n = fread(npdm, 1, sizeof npdm, f);
        if (n >= 0x80 && !memcmp(npdm, "META", 4)) {
            prio = npdm[0xE];
            stack_size = *(uint32_t *)(npdm + 0x1C);
            system_resource = *(uint32_t *)(npdm + 0x14);
            uint32_t aci = *(uint32_t *)(npdm + 0x70);
            if (aci + 0x18 <= n && !memcmp(npdm + aci, "ACI0", 4)) program_id = *(uint64_t *)(npdm + aci + 0x10);
        }
        fclose(f);
    }
    kernel_set_process_info(program_id, system_resource);
    LOG("main thread: prio %d, stack 0x%x\n", prio, stack_size);

    uint64_t start = loader_find_symbol_prefix("_ZN2nn4init5StartE");
    if (!start) FATAL("nn::init::Start not found\n");
    uint64_t early_fn = hle_register("<rtld_noop>",       rtld_noop);       // x2: hw = rtld's reloc applier (done at load)
    uint64_t late_fn  = hle_register("<rtld_init_all>",   rtld_init_all);   // x3: hw = DT_INIT of every module, after the heap
    KThread *t = kernel_create_main_thread(start, stack_size, prio);
    kernel_run_main(t, t->handle, 0, early_fn, late_fn);  // nn::init::Start(thread_handle, args_region, early, late)
}

// The log's first line after the log's own: this build and the machine, for bug reports. Nothing that identifies the
// player (platform.h plat_system_info). Once per session: a restart of this process (the start screen re-executing
// itself for the game) appends to the same log and inherits LASRECOMP_INFO_LOGGED.
static void log_startup_info(void) {
    if (getenv("LASRECOMP_INFO_LOGGED")) return;
    setenv("LASRECOMP_INFO_LOGGED", "1", 1);
    extern const char *larecomp_version(void);  // runtime/ui/launcher.mm
    PlatSysInfo si = {0};
    plat_system_info(&si);
    char cores[96] = "";
    if (si.cores > 0) {
        int n = snprintf(cores, sizeof cores, " %d cores", si.cores);
        if (si.cores_detail[0]) n += snprintf(cores + n, sizeof cores - n, " (%s)", si.cores_detail);
        if (si.threads > si.cores) snprintf(cores + n, sizeof cores - n, " / %d threads", si.threads);
    } else if (si.threads > 0) snprintf(cores, sizeof cores, " %d threads", si.threads);
    unsigned long long tenths = (si.ram * 10 + (1ull << 29)) >> 30;  // (GB to a tenth; whole when it is: 64 GB, 14.5 GB)
    char ram[32] = "RAM unknown";
    if (si.ram && tenths % 10) snprintf(ram, sizeof ram, "%llu.%llu GB RAM", tenths / 10, tenths % 10);
    else if (si.ram) snprintf(ram, sizeof ram, "%llu GB RAM", tenths / 10);
    fprintf(stderr, "[info] " APP_NAME " " APP_VERSION " (base %s), %s, %s%s, %s%s%s\n", larecomp_version(),
            si.os[0] ? si.os : "unknown OS",
            si.cpu[0] ? si.cpu : "unknown CPU", cores, ram, si.package ? ", " : "", si.package ? si.package : "");
}

// (SMRPG) --build-game <game files folder>: the installer (tools/installer) builds the game code here, without a
// window: gamebuild_run (the lasgen generator and the zig compiler that ship with the program, as in LASRecomp's start
// screen) with its progress on standard output, one line every time a thousandth or the step changes: "progress <0..1>
// <step>"; at the end "done" (code 0) or "error <why>" (code 1). Concurrent compilers: the logical cores, but no more
// than the GB of RAM minus 2 (like the start screen); LARECOMP_BUILD_JOBS sets them.
#include "gamebuild.h"
static void build_cli_progress(double f, const char *step) {
    static int last = -1;
    static char last_step[128];
    int k = (int)(f * 1000);
    if (k == last && !strcmp(step, last_step)) return;
    last = k;
    snprintf(last_step, sizeof last_step, "%s", step);
    printf("progress %.3f %s\n", f, step);
    fflush(stdout);
}
static int build_game_cli(const char *game_dir) {
    int cores = SDL_GetNumLogicalCPUCores(), ram_gb = SDL_GetSystemRAM() / 1024;
    int jobs = cores < ram_gb - 2 ? cores : ram_gb - 2 > 1 ? ram_gb - 2 : 1;
    if (getenv("LARECOMP_BUILD_JOBS") && atoi(getenv("LARECOMP_BUILD_JOBS")) > 0) jobs = atoi(getenv("LARECOMP_BUILD_JOBS"));
    LOG("[gamebuild] from the installer: %s, %d compilers at a time\n", game_dir, jobs);
    GameBuildOpts o = {build_cli_progress, jobs, NULL};
    char why[256] = "";
    if (!gamebuild_run(game_dir, &o, why, sizeof why)) {
        printf("error %s%s\n", why, gamebuild_cfg_stopped() ? " (Windows exploit protection, \"Control Flow Guard\", "
                                                               "stopped the generator)"
                                 : gamebuild_killed() ? " (the system killed a compiler: not enough memory)" : "");
        fflush(stdout);
        return 1;
    }
    printf("done\n");
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv) {
    plat_init();
    log_startup_info();
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--build-game")) {
            static char dir[1024];
            if (!realpath(argv[i + 1], dir)) {  // (before rt_paths_init: the path is relative to where we started)
                printf("error the folder %s does not exist\n", argv[i + 1]);
                return 1;
            }
            rt_paths_init();
            setvbuf(stderr, NULL, _IONBF, 0);  // (the log line by line, as in a normal launch)
            return build_game_cli(dir);
        }
    if (getenv("LARECOMP_REEXEC")) {  // (reserve_guest_space started this process again)
        const char *why = getenv("LARECOMP_REEXEC_WHY"), *n = getenv("LARECOMP_REEXEC");
        if (!why) LOG("[boot] started again (try %s) (LARECOMP_REEXEC set by the caller)\n", n);
        else if (!strcmp(why, REEXEC_FORCED)) LOG("[boot] started again (try %s): %s\n", n, why);
        else LOG("[boot] started again (try %s): the guest address range was in use%s%s\n", n, *why ? ": " : "", why);
    }
#ifdef _WIN32
    setvbuf(stderr, NULL, _IONBF, 0);  // the Windows CRT treats _IOLBF as full buffering
#else
    setvbuf(stderr, NULL, _IOLBF, 0);
#endif
    const char *root = "extracted";  // (in the data root)
    // LARECOMP_NO_LAUNCHER=1 (tools/game_slot.sh sets it for every test run): never the start screen - it can
    // delete game files, and a test's extracted/ may be a link to someone else's. Where it would come up (a forced
    // start screen, "Title screen" in game) the app exits instead. =0: off (start screen tests, scratch data root).
    const char *nl = getenv("LARECOMP_NO_LAUNCHER");
    bool no_launcher = nl && *nl && strcmp(nl, "0");
    bool skip_launcher = no_launcher || getenv("LARECOMP_INPUT");  // tests boot straight in
    if (getenv("LARECOMP_FORCE_LAUNCHER")) skip_launcher = false;  // (test: the start screen, then the scripted input)
    static char arg_root[1024];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--play")) skip_launcher = true;
        else if (argv[i][0] != '-' && !realpath(argv[i], arg_root)) root = argv[i];  // (relative to where we started)
        else if (argv[i][0] != '-') root = arg_root;
    }
    rt_paths_init();  // the data root becomes the working directory (runtime/paths.h)
    const char *fl = getenv("LARECOMP_FORCE_LAUNCHER");
    if (!g_game_las && !(fl && *fl && strcmp(fl, "0"))) {
        // (SMRPG) straight to the game, also without --play (a double click on smrpg-recomp.exe): the start screen is
        // Link's Awakening's. If the game's files or its code are missing (a half-finished installation, another
        // version), a notice that says what to do instead of a technical error.
        skip_launcher = true;
        char m[1100];
        snprintf(m, sizeof m, "%s/exefs/main", root);
        FILE *f = fopen(m, "rb");
        bool files = f != NULL;
        if (f) fclose(f);
        if (!files || !gamelib_present(root)) {
            LOG("[main] %s: %s\n", root, !files ? "no game files" : "no game code library for them");
            // (the message box, only when opened with a double click or the shortcut: no console or inherited log, like
            // plat_fatal_notice; a test with its log redirected exits right away with code 3)
            if (!no_launcher && getenv("LASRECOMP_LOG_FILE"))
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME,
                                         !files ? tr("The game's files are missing (the \"extracted\" folder). Run the "
                                                     APP_NAME " installer and choose this folder.")
                                                : tr("The game code built on this PC is missing (extracted\\lasgame.dll), "
                                                     "belongs to another version of the program or was built on a PC with "
                                                     "another CPU. Run the " APP_NAME " installer and choose this same folder: "
                                                     "it will build it again for this PC."),
                                         NULL);
            return 3;
        }
    }
    for (bool title = !skip_launcher;;) {
        if (title && no_launcher) {
            fprintf(stderr, "[main] LARECOMP_NO_LAUNCHER is set: not showing the start screen - exiting\n");
            exit(3);
        }
        if (title) {  // start screen (runtime/ui/launcher.mm): game files setup, seed choice, settings
            extern void launcher_run(const char *root);
            launcher_run(root);
        }
        boot(root);
        int kind = ui_run();  // window and input on the main thread; returns when a reboot is requested
        teardown();
        title = kind == RT_REBOOT_TITLE;
    }
}
