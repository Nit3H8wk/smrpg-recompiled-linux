// test_builder.exe <builder.dll> <installed folder> [threads] [cancel_s] [repeat]: tests builder.dll outside the
// installer, in this same process (the progress on standard output). Exit code: build_game_code's (0 = OK). With
// cancel_s (> 0), it cancels after that many seconds (measures how long it takes to return) and then builds again in
// the same process: the second build waits for what the first one left running and reuses what was already compiled.
// With "repeat", if the build fails, it repeats it once in the same process (the safety net:
// SMRPG_BUILD_TEST_FAIL).
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>

typedef void (*ProgressFn)(void *ctx, double frac, const wchar_t *step);
typedef int (*BuildFn)(const wchar_t *dest, int jobs, ProgressFn progress, void *ctx, volatile long *cancel,
                           wchar_t *why, int why_cap);

static volatile long g_cancel;
static DWORD g_cancel_ms;

static void progress(void *ctx, double f, const wchar_t *step) {
    static int last = -1;
    static wchar_t last_step[64];
    int k = (int)(f * 100);
    (void)ctx;
    if (k == last && !wcsncmp(step, last_step, 63)) return;
    last = k;
    wcsncpy(last_step, step, 63);
    char s[600];
    WideCharToMultiByte(CP_UTF8, 0, step, -1, s, sizeof s, NULL, NULL);
    printf("%3d %%  %s\n", k, s);
    fflush(stdout);
}

static DWORD WINAPI canceller(void *p) {
    Sleep(g_cancel_ms);
    printf("--- canceling ---\n");
    fflush(stdout);
    InterlockedExchange(&g_cancel, 1);
    (void)p;
    return 0;
}

static int run(BuildFn f, const wchar_t *dest, int jobs, const char *name) {
    wchar_t why[1024] = L"";
    DWORD t0 = GetTickCount();
    int r = f(dest, jobs, progress, NULL, &g_cancel, why, 1024);
    char w[2048];
    WideCharToMultiByte(CP_UTF8, 0, why, -1, w, sizeof w, NULL, NULL);
    printf("%s: result %d in %.1f s%s%s\n", name, r, (GetTickCount() - t0) / 1000.0, w[0] ? ": " : "", w);
    fflush(stdout);
    return r;
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 3) { fprintf(stderr, "usage: test_builder <builder.dll> <folder> [threads] [cancel_s]\n"); return 2; }
    HMODULE m = LoadLibraryExW(argv[1], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) { fprintf(stderr, "cannot load %ls (error %lu)\n", argv[1], GetLastError()); return 2; }
    BuildFn f = (BuildFn)(void *)GetProcAddress(m, "build_game_code");
    if (!f) { fprintf(stderr, "no build_game_code\n"); return 2; }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int jobs = argc > 3 && _wtoi(argv[3]) > 0 ? _wtoi(argv[3]) : (int)si.dwNumberOfProcessors;
    if (argc > 4 && _wtoi(argv[4]) > 0) {
        g_cancel_ms = (DWORD)_wtoi(argv[4]) * 1000;
        HANDLE h = CreateThread(NULL, 0, canceller, NULL, 0, NULL);
        DWORD t = GetTickCount();
        run(f, argv[2], jobs, "first (canceled)");
        printf("came back %.1f s after canceling\n", (GetTickCount() - t - g_cancel_ms) / 1000.0);
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
        InterlockedExchange(&g_cancel, 0);
        return run(f, argv[2], jobs, "second");
    }
    int r = run(f, argv[2], jobs, "build");
    if (r != 0 && argc > 5 && !wcscmp(argv[5], L"repeat")) r = run(f, argv[2], jobs, "repeated");
    return r;
}
