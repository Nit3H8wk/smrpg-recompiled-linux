// builder.dll: builds the game code (extracted\lasgame.dll) INSIDE the process that loads it, the installer
// (tools/installer/installer.c), with no child processes (before: smrpg-recomp.exe --build-game, the frozen generator
// and one zig per file, 30 processes at the same time; now everything inside a single process, with no child
// processes):
//   1. LAS Recomp's generator (gen.py) in an embedded Python (python312.dll from python.org's "embeddable"
//      distribution, isolated by its python312._pth: it looks at neither the PC's Python nor its variables), on one
//      thread: exefs\sdk and exefs\main to C, like `make gen`;
//   2. clang as a library (MSYS2's libclang-cpp and libLLVM: the same tools and the same -cc1 line as `make`) compiles
//      each chunk of C in a thread pool, as the generator writes them;
//   3. lld as a library (its MinGW driver, `make`'s link line) links lasgame.dll against smrpg-recomp.exe's import
//      library, with game_overrides.o ahead of the generated code: with multiple definitions of a function the first
//      one wins (--allow-multiple-definition).
// Everything it uses ships with it, in builder\: Python and the generator's packages, the clang and MinGW-w64 headers
// the game code includes, the MinGW-w64 CRT and libgcc for linking. From the installing user's PC, only Windows (64-bit
// 10 or 11).
// The CPU: the game code is compiled for x86-64-v3 (AVX2, FMA, BMI2..., like `make`) if the CPU has it; if not, for
// x86-64-v2 or x86-64 (slower, but it works): lasgame.dll.cpu says so and smrpg-recomp.exe checks it.
// Resuming: the generated code is reused if the game and the generator are the same (gen.key), and each object if
// neither its C (<chunk>.src) nor the way of compiling (base.txt) changed. Log: <folder>\build.log.
#include <windows.h>
#include <bcrypt.h>
#include <cpuid.h>
#include <psapi.h>

#include <malloc.h>
#include <setjmp.h>

#include <algorithm>
#include <exception>
#include <new>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "lld/Common/Driver.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"

LLD_HAS_DRIVER(mingw)

#ifndef GAMELIB_ID
#define GAMELIB_ID "dev"
#endif
#define BUILDER_VERSION "builder 1"
// what 0.51.0-beta signed its builds with. Since then only names and comments changed in what decides the compiled code:
// with the same signatures, what it compiled is still good and nothing is built again.
#define BUILDER_SIG "construir 1"
#define SIG_TOOLS L"<construir>"
static const struct { const char *now, *then; } SAME_GEN[] = {  // the generator's files: sha256 now and then
    {"5f4c0ce4e19b3aa474c9a8c58dca921616c5710caf3a3851b117b9a6b9bbebd7",
     "d70b5e0672888127a000f70edf23ad7ed0dceb08b7ad6602253e752cb89885b0"},  // gen.py
    {"cbec8ff5476ca5f23918723d730eeb5f516f5c2741a6b72e788499b25912e125",
     "971f2f286ce6cfce4b56fc707a40314b2449c8e7aab1221d97f81ad6975c6813"},  // lifter.py
};

// The texts for whoever is installing, in the installer's language: it gives its tr() (builder_set_tr); without it, English.
typedef const wchar_t *(*TrFn)(const wchar_t *);
static TrFn g_tr;
extern "C" __declspec(dllexport) void builder_set_tr(TrFn f) { g_tr = f; }

typedef void (*ProgressFn)(void *ctx, double frac, const wchar_t *step);

namespace {

// ------------------------------------------------------------------ text, files, log
std::string u8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}
std::wstring wide(const std::string &s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
std::wstring lp(const std::wstring &p) {  // \\?\ in front: long paths
    return p.compare(0, 4, L"\\\\?\\") == 0 ? p : L"\\\\?\\" + p;
}
bool exists(const std::wstring &p) { return GetFileAttributesW(lp(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
bool read_file(const std::wstring &p, std::string &out) {
    out.clear();
    HANDLE h = CreateFileW(lp(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart < (1ll << 31);
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ok = sz.QuadPart == 0 || (ReadFile(h, &out[0], (DWORD)sz.QuadPart, &got, NULL) && got == (DWORD)sz.QuadPart);
    }
    CloseHandle(h);
    return ok;
}
bool write_file(const std::wstring &p, const std::string &data) {  // (.tmp, then renamed: never half-written)
    std::wstring t = p + L".tmp";
    HANDLE h = CreateFileW(lp(t).c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &put, NULL) && put == data.size());
    CloseHandle(h);
    return ok && MoveFileExW(lp(t).c_str(), lp(p).c_str(), MOVEFILE_REPLACE_EXISTING);
}
void mkdirs(const std::wstring &p) {
    std::wstring l = lp(p);
    for (size_t i = 7; i < l.size(); i++)
        if (l[i] == L'\\') { l[i] = 0; CreateDirectoryW(l.c_str(), NULL); l[i] = L'\\'; }
    CreateDirectoryW(l.c_str(), NULL);
}
void remove_tree(const std::wstring &dir) {  // (only what this DLL created: the old cache, .gamebuild)
    DWORD a = GetFileAttributesW(lp(dir).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) {  // (a link: the link is removed, without going into what it points to)
        RemoveDirectoryW(lp(dir).c_str());
        return;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(lp(dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) remove_tree(p);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(lp(p).c_str());
            else DeleteFileW(lp(p).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(lp(dir).c_str());
}
uint64_t file_bytes(const std::wstring &p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(lp(p).c_str(), GetFileExInfoStandard, &a)) return 0;
    return ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

// "" if BCrypt fails: whoever compares takes "" as different from everything (otherwise any file would "match" any
// other)
std::string sha256_hex(const std::string &data) {
    static BCRYPT_ALG_HANDLE alg;
    static std::once_flag once;
    std::call_once(once, [] {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0))) alg = NULL;
    });
    unsigned char h[32] = {0};
    if (!alg || !BCRYPT_SUCCESS(BCryptHash(alg, NULL, 0, (PUCHAR)data.data(), (ULONG)data.size(), h, 32))) return "";
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
    return hex;
}
std::string file_sha(const std::wstring &p) {
    std::string d;
    return read_file(p, d) ? sha256_hex(d) : std::string();
}

// the contents of a whole folder (its relative paths and what each file holds, in order)
void list_files(const std::wstring &dir, const std::wstring &rel, std::vector<std::wstring> &out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(lp(dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) list_files(dir + L"\\" + n, rel + n + L"\\", out);
        else out.push_back(rel + n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
std::string dir_sha(const std::wstring &dir) {
    std::vector<std::wstring> fs;
    list_files(dir, L"", fs);
    std::sort(fs.begin(), fs.end());
    std::string all;
    for (auto &f : fs) all += u8(f) + " " + file_sha(dir + L"\\" + f) + "\n";
    return sha256_hex(all);
}

std::wstring wide(const std::string &s);
const wchar_t *trw(const wchar_t *en) { return g_tr ? g_tr(en) : en; }
std::string TR(const char *en) { return g_tr ? u8(g_tr(wide(en).c_str())) : std::string(en); }

struct Log {
    HANDLE h = INVALID_HANDLE_VALUE;
    std::mutex m;
    void open(const std::wstring &p) {
        h = CreateFileW(lp(p).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    }
    void close() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    void operator()(const char *fmt, ...) {
        char b[4096];
        SYSTEMTIME t;
        GetLocalTime(&t);
        int n = snprintf(b, sizeof b, "%02d:%02d:%02d [builder] ", t.wHour, t.wMinute, t.wSecond);
        va_list ap;
        va_start(ap, fmt);
        n += vsnprintf(b + n, sizeof b - n - 2, fmt, ap);
        va_end(ap);
        if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
        b[n++] = '\n';
        std::lock_guard<std::mutex> g(m);
        DWORD w;
        if (h != INVALID_HANDLE_VALUE) WriteFile(h, b, (DWORD)n, &w, NULL);
    }
} g_log;

// ------------------------------------------------------------------ la CPU (CPUID)
std::string cpu_level() {
    char env[32];
    DWORD en = GetEnvironmentVariableA("SMRPG_BUILD_CPU", env, sizeof env);  // (tests: only the three levels)
    if (en && en < sizeof env && (!strcmp(env, "x86-64") || !strcmp(env, "x86-64-v2") || !strcmp(env, "x86-64-v3")))
        return env;
    unsigned a, b, c, d, c1, d1, b7 = 0, c81 = 0;
    if (!__get_cpuid(1, &a, &b, &c1, &d1)) return "x86-64";
    if (__get_cpuid_max(0, NULL) >= 7) __cpuid_count(7, 0, a, b7, c, d);
    if (__get_cpuid(0x80000001, &a, &b, &c81, &d)) {}
    bool sse3 = c1 & 1, ssse3 = c1 & (1 << 9), sse41 = c1 & (1 << 19), sse42 = c1 & (1 << 20), popcnt = c1 & (1 << 23);
    bool cx16 = c1 & (1 << 13), lahf = c81 & 1;
    bool v2 = sse3 && ssse3 && sse41 && sse42 && popcnt && cx16 && lahf;
    bool osxsave = c1 & (1 << 27), avx = c1 & (1 << 28), fma = c1 & (1 << 12), f16c = c1 & (1 << 29), movbe = c1 & (1 << 22);
    bool avx2 = b7 & (1 << 5), bmi1 = b7 & (1 << 3), bmi2 = b7 & (1 << 8), lzcnt = c81 & (1 << 5);
    bool ymm = false;
    if (osxsave) {
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        ymm = (lo & 6) == 6;  // (the system saves the XMM and YMM registers)
    }
    if (v2 && avx && ymm && avx2 && fma && bmi1 && bmi2 && f16c && movbe && lzcnt) return "x86-64-v3";
    return v2 ? "x86-64-v2" : "x86-64";
}

// ------------------------------------------------------------------ large-stack threads, and their safety net
// clang and LLVM recurse very deep in the generated code's huge functions: with the 2 MB of a normal thread of this
// process, MemorySSAUpdater::recursePhi overflowed the stack (seen with gdb, 0xC00000FD, the process went down without
// warning). MSYS2's clang.exe (`make`'s) runs with 16 MB and zig's clang (0.47) with 46: these threads reserve 64 MB
// (address space only; the memory is committed as it is used). They run at below-normal priority, so that the PC stays
// responsive while they compile.
constexpr SIZE_T kStack = 64u << 20;

DWORD WINAPI big_thread_main(void *p) {
    std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()> *>(p));
    ULONG g = 256u << 10;  // (should the stack run out even so, there is room left to catch it: guard_veh)
    SetThreadStackGuarantee(&g);
    (*f)();
    return 0;
}

HANDLE big_thread(std::function<void()> fn) {
    auto *p = new std::function<void()>(std::move(fn));
    HANDLE h = CreateThread(NULL, kStack, big_thread_main, p, STACK_SIZE_PARAM_IS_A_RESERVATION | CREATE_SUSPENDED, NULL);
    if (!h) {
        delete p;
        return NULL;
    }
    SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
    ResumeThread(h);
    return h;
}

// The safety net: a crash inside clang/LLVM on a compiler thread (a Windows exception, or a fatal LLVM error, which
// would otherwise call exit and close the installer) comes back to compile_unit as that file's error. Only on those
// threads (TLS): on the generator's, unicorn raises and catches its own access faults (seen with gdb), and LLVM's
// CrashRecoveryContext, with its process-wide handler, was disabled entirely by the first of them.
struct Guard {
    jmp_buf jb;
    DWORD code;
    char why[300];
};
DWORD g_guard_tls = TLS_OUT_OF_INDEXES;
PVOID g_guard_veh;
// After catching a crash, the process is left tainted: what clang had half done was not freed, and if the crash came
// with a lock held (the heap's, LLVM's) it is still held. Later calls do not build: they ask to reopen the installer
// (what was compiled is kept, so it comes cheap)
std::atomic<bool> g_tainted{false};

Guard *guard_here() {
    if (g_guard_tls == TLS_OUT_OF_INDEXES) return nullptr;
    DWORD le = GetLastError();  // (TlsGetValue sets it to 0: keep it for the thread's code handling its exception)
    Guard *g = static_cast<Guard *>(TlsGetValue(g_guard_tls));
    SetLastError(le);
    return g;
}

LONG CALLBACK guard_veh(EXCEPTION_POINTERS *e) {
    Guard *g = guard_here();
    if (!g) return EXCEPTION_CONTINUE_SEARCH;
    switch (e->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        g->code = e->ExceptionRecord->ExceptionCode;
        TlsSetValue(g_guard_tls, NULL);
        longjmp(g->jb, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void guard_fatal(void *, const char *reason, bool) {  // (report_fatal_error, and out of memory: report_bad_alloc_error)
    if (Guard *g = guard_here()) {
        snprintf(g->why, sizeof g->why, "%s", reason ? reason : "");
        g->code = 0;
        TlsSetValue(g_guard_tls, NULL);
        longjmp(g->jb, 1);
    }
    g_log("fatal LLVM error outside a compilation: %s", reason ? reason : "");
    llvm::sys::Process::Exit(1);  // (during the link, with lldMain's CrashRecoveryContext, it returns to lldMain)
}

void guard_install() {
    static std::once_flag once;
    std::call_once(once, [] {
        g_guard_tls = TlsAlloc();
        g_guard_veh = AddVectoredExceptionHandler(1, guard_veh);
        llvm::install_fatal_error_handler(guard_fatal, nullptr);
        llvm::install_bad_alloc_error_handler(guard_fatal, nullptr);
        // out of memory in a `new` (std::vector, the IR...): without exceptions, it would end with abort without saying
        // anything
        std::set_new_handler([] { guard_fatal(nullptr, "out of memory", false); });
        std::set_terminate([] {
            g_log("ended by an internal error (std::terminate)");
            abort();
        });
    });
}

// fn(ctx) with the safety net in place: false if it crashed (why says how). After a crash, the thread must not keep
// compiling: what clang had half done stays unfreed.
__attribute__((noinline)) bool run_guarded(void (*fn)(void *), void *ctx, std::string &why) {
    Guard g;
    g.code = 0, g.why[0] = 0;
    if (setjmp(g.jb)) {
        if (g.code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        char b[400];
        if (g.code)
            snprintf(b, sizeof b, "exception 0x%08lX%s", g.code,
                     g.code == EXCEPTION_STACK_OVERFLOW     ? " (stack overflow)"
                     : g.code == EXCEPTION_ACCESS_VIOLATION ? " (invalid memory access)"
                                                            : "");
        else if (!strcmp(g.why, "out of memory"))
            snprintf(b, sizeof b, "out of memory");
        else
            snprintf(b, sizeof b, "error grave de LLVM: %s", g.why);
        why = b;
        return false;
    }
    TlsSetValue(g_guard_tls, &g);
    fn(ctx);
    TlsSetValue(g_guard_tls, NULL);
    return true;
}

// ------------------------------------------------------------------ the paths
struct Paths {
    std::wstring dest, game, work, tools, py, res_inc, mingw_inc, mingw_lib, src, implib;
};

std::wstring module_dir() {
    HMODULE m = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void *)&module_dir, &m);
    wchar_t p[1200];
    DWORD n = GetModuleFileNameW(m, p, 1200);
    std::wstring s(p, n);
    size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? s : s.substr(0, k);
}

// ------------------------------------------------------------------ the generator (embedded Python)
struct Py {
    HMODULE dll = NULL;
    int (*IsInitialized)(void) = nullptr;
    void (*InitializeEx)(int) = nullptr;
    void *(*SaveThread)(void) = nullptr;
    int (*GILEnsure)(void) = nullptr;
    void (*GILRelease)(int) = nullptr;
    int (*RunSimpleString)(const char *) = nullptr;
};
Py g_py;
std::mutex g_py_lock;

bool py_load(const std::wstring &dir, std::string &err) {
    std::lock_guard<std::mutex> g(g_py_lock);
    if (g_py.dll) return true;
    HMODULE h = LoadLibraryExW((dir + L"\\python312.dll").c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) { err = TR("python312.dll could not be loaded (error ") + std::to_string(GetLastError()) + ")"; return false; }
#define PYF(f, n) g_py.f = (decltype(g_py.f))(void *)GetProcAddress(h, n)
    PYF(IsInitialized, "Py_IsInitialized");
    PYF(InitializeEx, "Py_InitializeEx");
    PYF(SaveThread, "PyEval_SaveThread");
    PYF(GILEnsure, "PyGILState_Ensure");
    PYF(GILRelease, "PyGILState_Release");
    PYF(RunSimpleString, "PyRun_SimpleString");
#undef PYF
    if (!g_py.IsInitialized || !g_py.InitializeEx || !g_py.SaveThread || !g_py.GILEnsure || !g_py.GILRelease || !g_py.RunSimpleString) {
        err = TR("python312.dll lacks the expected API");
        return false;
    }
    g_py.dll = h;
    if (!g_py.IsInitialized()) {
        // (isolated from the PC's variables too (PYTHONUTF8, PYTHONMALLOC...): python312._pth sets the paths, but those
        // are read earlier; 3.12's global flags turn it all off, like python -I -E -s)
        for (const char *flag : {"Py_IgnoreEnvironmentFlag", "Py_IsolatedFlag", "Py_NoUserSiteDirectory"})
            if (int *f = (int *)(void *)GetProcAddress(h, flag)) *f = 1;
        g_py.InitializeEx(0);  // (no signal handlers: it is a library inside the installer)
        g_py.SaveThread();     // (releases the GIL: each run takes it with PyGILState_Ensure, from any thread)
    }
    return true;
}

// the path as a Python expression: its UTF-8 bytes in hex (any character, quotes or trailing backslashes included)
std::string pystr(const std::wstring &p) {
    std::string s = u8(p), out = "bytes.fromhex('";
    char h[3];
    for (unsigned char ch : s) snprintf(h, sizeof h, "%02x", ch), out += h;
    return out + "').decode('utf-8')";
}

// gen.py <name> <nso> <base> <outdir> [--hle <hle>]: its progress (stderr) to <log>, its exit code to <rc>
int py_gen(const Paths &P, const char *name, const char *base, const std::wstring &log, std::string &err) {
    std::wstring wn = wide(name), nso = P.game + L"\\exefs\\" + wn, out = P.work + L"\\" + wn;
    std::wstring rc = P.work + L"\\gen_" + wn + L".rc";
    DeleteFileW(lp(rc).c_str());
    std::string code =
        "import sys, runpy\n"
        "for _m in ('gen', 'lifter', 'analyze', 'nso', 'passes', 'patches', 'simd_c', 'game_addresses'):\n"
        "    sys.modules.pop(_m, None)\n"
        "sys.argv = ['gen.py', '" + std::string(name) + "', " + pystr(nso) + ", '" + base + "', " + pystr(out) +
        (strcmp(name, "sdk") == 0 ? ", '--hle', " + pystr(P.src + L"\\hle_symbols.txt") : std::string()) + "]\n"
        "_log = open(" + pystr(log) + ", 'w', encoding='utf-8', buffering=1)\n"
        "_old_err, _old_out = sys.stderr, sys.stdout\n"
        "sys.stderr = sys.stdout = _log\n"
        "_rc = 0\n"
        "try:\n"
        "    runpy.run_path(" + pystr(P.py + L"\\gen\\tools\\gen.py") + ", run_name='__main__')\n"
        "except SystemExit as _e:\n"
        "    _rc = _e.code if isinstance(_e.code, int) else (0 if _e.code is None else 1)\n"
        "    if _e.code is not None and not isinstance(_e.code, int):\n"
        "        print(_e.code, file=_log)\n"
        "except BaseException:\n"
        "    import traceback\n"
        "    traceback.print_exc(file=_log)\n"
        "    _rc = 1\n"
        "finally:\n"
        "    sys.stderr, sys.stdout = _old_err, _old_out\n"
        "    _log.close()\n"
        "with open(" + pystr(rc) + ", 'w') as _f:\n"
        "    _f.write(str(_rc))\n";
    int st = g_py.GILEnsure();
    int r = g_py.RunSimpleString(code.c_str());
    g_py.GILRelease(st);
    std::string rcs;
    if (r != 0 || !read_file(rc, rcs)) { err = TR("the generator (") + name + TR(") did not finish"); return 1; }
    int v = atoi(rcs.c_str());
    if (v != 0) {
        std::string l;
        read_file(log, l);
        size_t k = l.find_last_not_of("\r\n");
        l = k == std::string::npos ? "" : l.substr(0, k + 1);
        size_t s = l.find_last_of('\n');
        err = TR("the generator (") + name + TR(") failed: ") + (s == std::string::npos ? l : l.substr(s + 1));
    }
    return v;
}

// what the generator has written so far: ": progress <done>/<all> files <chunks>" (its log)
void gen_progress(const std::wstring &log, uint64_t &ci, uint64_t &all, int &files) {
    std::string t;
    if (!read_file(log, t)) return;
    size_t p = t.rfind(": progress ");
    if (p == std::string::npos) return;
    unsigned long long a = 0, b = 0;
    int f = 0;
    if (sscanf(t.c_str() + p, ": progress %llu/%llu files %d", &a, &b, &f) == 3) {
        ci = a, all = b;
        if (f > files) files = f;
    }
}

// ------------------------------------------------------------------ compiling (clang as a library)
struct Unit {
    std::wstring src, obj;
    std::string stem, define;  // define: an extra "-D" (gamelib_entry: the library's id)
    uint64_t bytes = 0;
};

std::vector<std::string> cc1_args(const Paths &P, const std::string &cpu) {
    // `make`'s -cc1 line (clang -### with GENFLAGS: -O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label
    // -fvisibility=hidden -D_FILE_OFFSET_BITS=64 -march=x86-64-v3), without -disable-free (here the memory is given
    // back: one process compiles a thousand files) or debug paths; the headers, the ones in the builder folder
    return {"-triple", "x86_64-w64-windows-gnu", "-O1", "-emit-obj", "-clear-ast-before-backend", "-disable-llvm-verifier",
            "-discard-value-names", "-mrelocation-model", "pic", "-pic-level", "2", "-mframe-pointer=none",
            "-relaxed-aliasing", "-fmath-errno", "-ffp-contract=on", "-fno-rounding-math", "-mconstructor-aliases",
            "-funwind-tables=2", "-fno-sized-deallocation", "-fno-use-init-array", "-target-cpu", cpu,
            "-debugger-tuning=gdb", "-resource-dir", u8(P.tools + L"\\clang"), "-D", "_FILE_OFFSET_BITS=64",
            "-I", u8(P.src), "-internal-isystem", u8(P.res_inc), "-internal-isystem", u8(P.mingw_inc),
            "-Wno-return-type", "-Wno-unused-label", "-ferror-limit", "19", "-fvisibility=hidden", "-fno-use-cxa-atexit",
            "-fgnuc-version=4.2.1", "-fskip-odr-check-in-gmf", "-exception-model=seh", "-faddrsig"};
}

// (tests of the safety net: SMRPG_BUILD_TEST_FAIL=<chunk>:1 writes to address 0 and <chunk>:2 exhausts the stack when
// compiling that chunk, as if clang failed)
int crash_test_mode(const std::string &stem) {
    char e[96];
    DWORD n = GetEnvironmentVariableA("SMRPG_BUILD_TEST_FAIL", e, sizeof e);
    if (!n || n >= sizeof e) return 0;
    std::string s = e;
    size_t k = s.find(':');
    return k != std::string::npos && s.substr(0, k) == stem ? atoi(s.c_str() + k + 1) : 0;
}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winfinite-recursion"  // (on purpose: exhausts the stack)
__attribute__((noinline, optnone)) int burn_stack(int n) {
    volatile char buf[4096];
    buf[0] = (char)n;
    return burn_stack(n + 1) + buf[0];
}
#pragma clang diagnostic pop

// an object that can be reused: x86-64 COFF (a power cut can leave the file renamed but full of zeros)
bool obj_ok(const std::wstring &p) {
    HANDLE h = CreateFileW(lp(p).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    unsigned char m[20] = {0};
    DWORD got = 0;
    bool ok = ReadFile(h, m, sizeof m, &got, NULL) && got == sizeof m && m[0] == 0x64 && m[1] == 0x86;
    CloseHandle(h);
    return ok;
}

bool compile_unit(const std::vector<std::string> &base, const Unit &u, std::string &diag) {
    std::vector<std::string> a = base;
    if (!u.define.empty()) a.push_back("-D"), a.push_back(u.define);
    std::wstring tmp = u.obj + L".tmp";
    std::string name = u8(u.src);
    size_t k = name.find_last_of("\\/");
    a.push_back("-main-file-name"), a.push_back(k == std::string::npos ? name : name.substr(k + 1));
    a.push_back("-o"), a.push_back(u8(tmp));
    a.push_back("-x"), a.push_back("c"), a.push_back(name);
    std::vector<const char *> argv;
    for (auto &s : a) argv.push_back(s.c_str());
    llvm::raw_string_ostream os(diag);
    struct Job {
        std::vector<const char *> *argv;
        llvm::raw_string_ostream *os;
        bool ok;
        int test;
    } job = {&argv, &os, false, crash_test_mode(u.stem)};
    std::string crash;
    bool ran = run_guarded(
        [](void *p) {
            Job &j = *static_cast<Job *>(p);
            if (j.test == 1) *(volatile int *)(uintptr_t)0 = 1;
            if (j.test == 2) burn_stack(0);
            clang::DiagnosticOptions dopts;
            clang::TextDiagnosticPrinter printer(*j.os, dopts);
            clang::DiagnosticsEngine diags(llvm::makeIntrusiveRefCnt<clang::DiagnosticIDs>(), dopts, &printer, false);
            auto inv = std::make_shared<clang::CompilerInvocation>();
            if (!clang::CompilerInvocation::CreateFromArgs(*inv, *j.argv, diags)) return;
            clang::CompilerInstance ci(inv);
            // (a file system of its own per compilation: its working directory is not shared between threads)
            ci.createVirtualFileSystem(
                llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>(llvm::vfs::createPhysicalFileSystem().release()), &printer);
            ci.createDiagnostics(&printer, false);
            clang::EmitObjAction act;
            j.ok = ci.ExecuteAction(act) && !ci.getDiagnostics().hasErrorOccurred();
        },
        &job, crash);
    os.flush();
    bool ok = ran && job.ok;
    if (!ran) {
        diag = "clang stopped: " + crash + (diag.empty() ? std::string() : "\n" + diag);
        g_tainted = true;  // (what clang had half done stays that way: see g_tainted)
    }
    if (ok) {  // (an antivirus may hold the previous object open for a moment)
        for (int i = 0; i < 20; i++) {
            if (MoveFileExW(lp(tmp).c_str(), lp(u.obj).c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
            Sleep(100);
        }
        diag = "could not write " + u8(u.obj) + " (error " + std::to_string(GetLastError()) + ")";
        ok = false;
    }
    DeleteFileW(lp(tmp).c_str());
    return ok;
}

// ------------------------------------------------------------------ linking (lld as a library)
bool g_lld_spent;  // (lld left through its fatal-error path: it cannot be used again in this process)

bool link_lib(const Paths &P, const std::vector<std::wstring> &objs, const std::wstring &out, std::string &diag) {
    std::string L = u8(P.mingw_lib);
    // (--threads=1: with more, lld leaves an LLVM thread pool alive in the process, and on exit its static destructor
    // waits for threads Windows has already killed: the process hung on closing. Measured: linking like this, 0.6 s)
    std::vector<std::string> a = {"ld.lld", "-m", "i386pep", "--shared", "-Bdynamic", "-e", "DllMainCRTStartup",
                                  "--enable-auto-image-base", "--threads=1", "-o", u8(out), L + "\\dllcrt2.o",
                                  L + "\\crtbegin.o", "-L" + L, u8(P.implib), "--allow-multiple-definition"};
    for (auto &o : objs) a.push_back(u8(o));
    for (const char *l : {"-lmingw32", "-lgcc_s", "-lgcc", "-lmoldname", "-lmingwex", "-lmsvcrt", "-lpthread", "-ladvapi32",
                          "-lshell32", "-luser32", "-lkernel32", "-lmingw32", "-lgcc_s", "-lgcc", "-lmoldname", "-lmingwex",
                          "-lmsvcrt", "-lkernel32"})
        a.push_back(l);
    a.push_back(L + "\\crtend.o");
    std::vector<const char *> argv;
    for (auto &s : a) argv.push_back(s.c_str());
    llvm::raw_string_ostream os(diag);
    // (lld, on a fatal error or past 20 errors, calls exit: with the CrashRecoveryContext on, lldMain catches it and
    // returns here. Only during the link: the generator is already done, and no compiler thread is still alive)
    llvm::CrashRecoveryContext::Enable();
    lld::Result r = lld::lldMain(argv, os, os, {{lld::MinGW, &lld::mingw::link}});
    llvm::CrashRecoveryContext::Disable();
    os.flush();
    if (!r.canRunAgain) g_lld_spent = true;
    return r.retCode == 0;
}

// ------------------------------------------------------------------ the build
// The generator and the compiler threads work on their own state, on the heap, not on the call's: on canceling or
// failing, the call returns right away and they finish on their own whatever they have in hand (a thread cannot be cut
// off halfway without leaving the process tainted, and a large chunk takes minutes); the next build waits for them
// (g_last_gen, g_last_build). If the installer is closed, the whole process ends along with them.
struct GenShared {
    Paths P;
    std::wstring log[2];                       // each one's log (another name if the last run's could not be deleted)
    std::string key;                           // gen.key, which it writes once both finish successfully
    std::atomic<bool> started[2] = {{false}, {false}};  // (its log is only read once it has started: not the old one)
    std::atomic<int> files_final[2] = {{0}, {0}};       // each one's chunks, read when it finishes (before state = 1)
    std::atomic<int> state[2] = {{0}, {0}};  // main, sdk: 0 running, 1 fine, 2 failed
    std::atomic<bool> running{false};
    std::mutex m;
    std::string err;
};
std::shared_ptr<GenShared> g_last_gen;
const char *const g_mods[2] = {"main", "sdk"};

struct Build {
    Paths P;
    std::vector<std::string> base;
    std::vector<Unit> units;
    std::deque<size_t> queue;
    std::mutex qm;
    std::condition_variable qcv;
    std::atomic<size_t> ndone{0}, nreused{0};
    std::atomic<uint64_t> bytes_done{0};
    std::atomic<bool> failed{false}, stop{false};
    std::string fail_msg;       // (con qm)
    std::atomic<int> alive{0};  // compiler threads running
};
std::shared_ptr<Build> g_last_build;

void add_unit(Build &b, const std::wstring &src, const std::string &stem, const std::string &define) {
    Unit u;
    u.src = src, u.stem = stem, u.define = define;
    u.obj = b.P.work + L"\\" + wide(stem) + L".o";
    u.bytes = file_bytes(src);
    std::lock_guard<std::mutex> g(b.qm);
    b.units.push_back(u);
    b.queue.push_back(b.units.size() - 1);
    b.qcv.notify_one();
}

void compile_worker(const std::shared_ptr<Build> &bs) {
    Build &b = *bs;
    for (;;) {
        Unit u;
        {
            std::unique_lock<std::mutex> g(b.qm);
            b.qcv.wait(g, [&] { return b.stop || !b.queue.empty(); });
            if (b.queue.empty() || b.failed) break;
            u = b.units[b.queue.front()];
            b.queue.pop_front();
        }
        // is the earlier object still good? (the same C, and the same way of compiling: base.txt)
        std::string src;
        std::string sha = read_file(u.src, src) ? sha256_hex(src + "\n" + u.define) : std::string();
        std::wstring srcf = b.P.work + L"\\" + wide(u.stem) + L".src";
        std::string prev;
        if (!sha.empty() && read_file(srcf, prev) && prev == sha && obj_ok(u.obj)) {
            b.nreused++, b.ndone++, b.bytes_done += u.bytes;
            continue;
        }
        DeleteFileW(lp(srcf).c_str());  // (its .src only exists while its .o is the one from that C)
        std::string diag;
        bool ok = !sha.empty() && compile_unit(b.base, u, diag);
        if (!ok) {
            if (sha.empty()) diag = "could not read " + u8(u.src);
            if (diag.empty()) diag = "clang did not say why";
            write_file(b.P.work + L"\\" + wide(u.stem) + L".log", diag);
            std::lock_guard<std::mutex> g(b.qm);
            if (!b.failed) {
                b.failed = true;
                size_t k = diag.find('\n');
                b.fail_msg = TR("the game code does not compile (") + u.stem + "): " + (k == std::string::npos ? diag : diag.substr(0, k));
            }
            b.stop = true;
            b.qcv.notify_all();
            break;
        }
        write_file(srcf, sha);
        b.ndone++, b.bytes_done += u.bytes;
    }
    b.alive--;
}

int fail(wchar_t *why, int cap, const std::string &msg) {
    g_log("ERROR: %s", msg.c_str());
    std::wstring w = wide(msg);
    if (why && cap > 0) wcsncpy(why, w.c_str(), cap - 1), why[cap - 1] = 0;
    return 1;
}

}  // namespace

extern "C" __declspec(dllexport) int build_game_code(const wchar_t *dest, int jobs, ProgressFn progress, void *ctx,
                                                     volatile long *cancel, wchar_t *why, int why_cap) {
    if (why && why_cap > 0) why[0] = 0;
    DWORD t0 = GetTickCount();
    Paths P;
    P.dest = dest;
    P.game = P.dest + L"\\extracted";
    P.work = P.game + L"\\.builder";
    P.tools = module_dir();
    P.py = P.tools + L"\\python";
    P.res_inc = P.tools + L"\\clang\\include";
    P.mingw_inc = P.tools + L"\\mingw\\include";
    P.mingw_lib = P.tools + L"\\mingw\\lib";
    P.src = P.dest + L"\\gamelib-src";
    P.implib = P.dest + L"\\smrpg-recomp.dll.a";
    g_log.open(P.dest + L"\\build.log");
    struct Close { ~Close() { g_log.close(); } } close_log;
    auto step = [&](double f, const wchar_t *s) { if (progress) progress(ctx, f, s); };
    auto cancelled = [&] { return cancel && *cancel; };

    // what is needed: the extracted game and what ships in builder\ and gamelib-src
    std::string nso;
    if (!read_file(P.game + L"\\exefs\\main", nso) || nso.size() < 0x60 || memcmp(nso.data(), "NSO0", 4))
        return fail(why, why_cap, TR("the game's program is missing (extracted\\exefs\\main)"));
    char bid[17];
    for (int i = 0; i < 8; i++) snprintf(bid + 2 * i, 3, "%02x", (unsigned char)nso[0x40 + i]);
    std::string full_id = std::string(GAMELIB_ID) + "-" + bid;
    // (with python312.zip missing or damaged, Python does not start and calls abort: checked beforehand)
    for (const std::wstring &f : {P.py + L"\\python312.dll", P.py + L"\\python312.zip", P.py + L"\\python312._pth",
                                  P.py + L"\\vcruntime140.dll", P.py + L"\\gen\\tools\\gen.py", P.res_inc + L"\\stdint.h",
                                  P.mingw_inc + L"\\stdio.h", P.mingw_lib + L"\\dllcrt2.o", P.src + L"\\cpu.h",
                                  P.src + L"\\gamelib_entry.c", P.src + L"\\game_overrides.c", P.implib})
        if (!exists(f) || !file_bytes(f)) return fail(why, why_cap, TR("missing: ") + u8(f) + TR(": run the installer again"));
    if (g_tainted)
        return fail(why, why_cap, TR("an earlier build failed inside clang and left this process in a bad state: close "
                                  "the installer and open it again (what was already compiled is kept)"));
    if (g_lld_spent)
        return fail(why, why_cap, TR("the linker failed earlier and cannot be used again: close the installer and open "
                                  "it again"));
    std::string cpu = cpu_level();
    // (whatever is still running from a canceled or failed build: it finishes on its own, and it is waited for here; a
    // large chunk takes ~3 min: if it is not done in 20, something got stuck)
    for (DWORD tw = GetTickCount(); (g_last_gen && g_last_gen->running) || (g_last_build && g_last_build->alive > 0);) {
        if (cancelled()) return fail(why, why_cap, TR("canceled"));
        if (g_tainted || GetTickCount() - tw > 20 * 60 * 1000)
            return fail(why, why_cap, TR("what was left running from the previous attempt does not finish: close the "
                                      "installer and open it again (what was already compiled is kept)"));
        step(0, trw(L"waiting for what was left running from the previous attempt"));
        Sleep(200);
    }
    guard_install();
    mkdirs(P.work);
    MEMORYSTATUSEX ms = {sizeof ms};
    GlobalMemoryStatusEx(&ms);
    g_log("%s: %s, CPU %s, %d compiler threads, %.1f GB of RAM, id %s", BUILDER_VERSION, u8(P.game).c_str(),
          cpu.c_str(), jobs, ms.ullTotalPhys / 1073741824.0, full_id.c_str());

    // the way of compiling (base.txt): if it changes, no earlier object is valid. Without paths (moving the folder does
    // not change the output) and with the contents of the clang and MinGW-w64 headers; the port's (cpu.h, simd.h...)
    // and the generator are in GAMELIB_ID
    std::vector<std::string> base = cc1_args(P, cpu);
    Paths Pn = P;
    Pn.tools = SIG_TOOLS, Pn.res_inc = SIG_TOOLS L"\\clang\\include", Pn.mingw_inc = SIG_TOOLS L"\\mingw\\include";
    Pn.src = L"<gamelib-src>";
    std::string sig = std::string(BUILDER_SIG) + "\n" + GAMELIB_ID + "\n" + cpu + "\n";
    for (auto &s : cc1_args(Pn, cpu)) sig += s + " ";
    sig += "\n" + std::to_string(file_bytes(P.tools + L"\\libclang-cpp.dll")) + " " +
           std::to_string(file_bytes(P.tools + L"\\libLLVM-23.dll")) + " " + dir_sha(P.res_inc) + " " + dir_sha(P.mingw_inc) + "\n";
    std::string old;
    bool same_base = read_file(P.work + L"\\base.txt", old) && old == sig;
    if (!same_base) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(lp(P.work + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring n = fd.cFileName;
                if (n.size() > 2 && (n.substr(n.size() - 2) == L".o" || (n.size() > 4 && n.substr(n.size() - 4) == L".src")))
                    DeleteFileW(lp(P.work + L"\\" + n).c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        write_file(P.work + L"\\base.txt", sig);
    }

    // the generator: the generated code is reused if the game and the generator are the same (gen.key)
    std::string key = std::string(BUILDER_SIG) + GAMELIB_ID + sha256_hex(nso) + file_sha(P.game + L"\\exefs\\sdk") +
                      file_sha(P.src + L"\\hle_symbols.txt");
    for (const wchar_t *f : {L"gen.py", L"lifter.py", L"analyze.py", L"nso.py", L"passes.py", L"patches.py", L"simd_c.py",
                             L"game_addresses.py"})
    {
        std::string s = file_sha(P.py + L"\\gen\\tools\\" + f);
        for (auto &g : SAME_GEN)
            if (s == g.now) s = g.then;
        key += s;
    }
    key = sha256_hex(key);
    std::string done;
    int files[2] = {0, 0};  // chunks of main and sdk
    bool reuse_gen = read_file(P.work + L"\\gen.key", done) && done.compare(0, 64, key) == 0 &&
                     sscanf(done.c_str() + 64, " %d %d", &files[0], &files[1]) == 2 && files[0] > 0 && files[1] > 0;
    const char *const *mods = g_mods;
    auto gs = std::make_shared<GenShared>();
    gs->P = P;
    gs->key = key;
    if (reuse_gen) {
        gs->state[0] = 1, gs->state[1] = 1;
        gs->files_final[0] = files[0], gs->files_final[1] = files[1];
        g_log("the generated code is still good (main %d chunks, sdk %d)", files[0], files[1]);
    } else {
        // (LAS Recomp #337: with Control Flow Guard active, unicorn takes the process down outright; the installer
        // reopens itself without it, so this is only reached if it could not)
        PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfg = {};
        if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessControlFlowGuardPolicy, &cfg, sizeof cfg) &&
            cfg.EnableControlFlowGuard)
            return fail(why, why_cap, TR("Windows forces Control Flow Guard on this process and the generator (unicorn) "
                                      "cannot work with it"));
        if (cancelled()) return fail(why, why_cap, TR("canceled"));
        files[0] = files[1] = 0;
        DeleteFileW(lp(P.work + L"\\gen.key").c_str());
        for (int i = 0; i < 2; i++) {
            gs->log[i] = P.work + L"\\gen_" + wide(mods[i]) + L".log";
            DeleteFileW(lp(gs->log[i]).c_str());
            if (exists(gs->log[i]))  // (open in another program: the previous run's is not read by mistake)
                gs->log[i] = P.work + L"\\gen_" + wide(mods[i]) + L"." + std::to_wstring(GetTickCount()) + L".log";
        }
        std::string err;
        if (!py_load(P.py, err)) return fail(why, why_cap, TR("the generator does not start: ") + err);
        gs->running = true;
        g_last_gen = gs;
        HANDLE h = big_thread([gs] {
            // the sdk first (8 s), then main (45 s): their chunks are compiled meanwhile. Each one, on finishing
            // successfully: first its final chunk count (from its whole log), then state = 1
            for (int i : {1, 0}) {
                std::string e;
                gs->started[i] = true;
                int r = py_gen(gs->P, g_mods[i], i == 0 ? "0x7000000000" : "0x7005000000", gs->log[i], e);
                uint64_t ci = 0, all = 0;
                int n = 0;
                if (r == 0) {
                    gen_progress(gs->log[i], ci, all, n);
                    if (n <= 0) r = 1, e = std::string("the generator's log (") + g_mods[i] + ") does not say how many "
                                                                                                    "chunks it wrote";
                }
                if (r != 0) {
                    {
                        std::lock_guard<std::mutex> g(gs->m);
                        gs->err = e;
                    }
                    gs->state[i] = 2;
                    if (gs->state[1 - i] == 0) gs->state[1 - i] = 2;
                    break;
                }
                gs->files_final[i] = n;
                gs->state[i] = 1;
            }
            // (the generated code is good for next time even if this one is canceled while compiling)
            if (gs->state[0] == 1 && gs->state[1] == 1)
                write_file(gs->P.work + L"\\gen.key", gs->key + " " + std::to_string(gs->files_final[0]) + " " +
                                                         std::to_string(gs->files_final[1]) + "\n");
            gs->running = false;
        });
        if (!h) {
            gs->running = false;
            return fail(why, why_cap, TR("the generator's thread could not be created (error ") + std::to_string(GetLastError()) + ")");
        }
        CloseHandle(h);  // (detached: it finishes on its own)
    }

    // the chunks, as they come out; a thread pool compiles them
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
    auto bs = std::make_shared<Build>();
    Build &b = *bs;
    b.P = P, b.base = base;
    std::vector<Unit> &units = b.units;
    std::mutex &qm = b.qm;
    std::condition_variable &qcv = b.qcv;
    std::atomic<size_t> &ndone = b.ndone, &nreused = b.nreused;
    std::atomic<uint64_t> &bytes_done = b.bytes_done;
    std::atomic<bool> &failed = b.failed, &stop = b.stop;
    add_unit(b, P.src + L"\\gamelib_entry.c", "gamelib_entry", "GAMELIB_ID=\"" + full_id + "\"");
    add_unit(b, P.src + L"\\game_overrides.c", "game_overrides", "");
    int njobs = jobs < 1 ? 1 : jobs > 64 ? 64 : jobs;
    std::vector<HANDLE> pool;
    g_last_build = bs;
    for (int i = 0; i < njobs; i++) {
        b.alive++;
        HANDLE h = big_thread([bs] { compile_worker(bs); });
        if (h) pool.push_back(h);
        else b.alive--;
    }
    if (pool.empty()) {
        std::lock_guard<std::mutex> g(qm);
        failed = true, stop = true;
        return fail(why, why_cap, TR("no compiler thread could be created (error ") + std::to_string(GetLastError()) + ")");
    }
    int queued[2] = {0, 0};
    bool tables[2] = {false, false};
    uint64_t gci[2] = {0, 0}, gall[2] = {0, 0};
    double shown = 0;
    for (;;) {
        if (cancelled()) {
            std::lock_guard<std::mutex> g(qm);
            if (!failed) failed = true, b.fail_msg = TR("canceled (what was compiled is kept)");
            stop = true;
            b.queue.clear();
            qcv.notify_all();
        }
        bool gens_done = true;
        for (int i = 0; i < 2; i++) {
            // (the state first: with state = 1, files_final is already the full chunk count, and the table, which goes
            // last, never comes out before one of its chunks)
            int st = gs->state[i];
            if (st == 1 && files[i] < gs->files_final[i]) files[i] = gs->files_final[i];
            if (!reuse_gen && st == 0 && gs->started[i]) gen_progress(gs->log[i], gci[i], gall[i], files[i]);
            if (st == 2 && !failed) {
                std::string e;
                {
                    std::lock_guard<std::mutex> g(gs->m);
                    e = gs->err;
                }
                std::lock_guard<std::mutex> g2(qm);
                failed = true, b.fail_msg = e.empty() ? TR("the generator failed") : e;
                stop = true;
                qcv.notify_all();
            }
            if (st != 1) gens_done = false;
            for (; !failed && queued[i] < files[i]; queued[i]++) {
                char stem[32];
                snprintf(stem, sizeof stem, "%s_%03d", mods[i], queued[i]);
                add_unit(b, P.work + L"\\" + wide(mods[i]) + L"\\" + wide(stem) + L".c", stem, "");
            }
            if (st == 1 && !tables[i] && !failed) {
                tables[i] = true;
                add_unit(b, P.work + L"\\" + wide(mods[i]) + L"\\" + wide(mods[i]) + L"_table.c", std::string(mods[i]) + "_table", "");
            }
        }
        size_t total, nd = ndone;
        {
            std::lock_guard<std::mutex> g(qm);
            total = units.size();
        }
        if (failed && stop) break;
        if (gens_done && nd == total) break;
        // the progress: what was generated (15%), what was compiled (half bytes, half files) and the link (the final
        // 5%)
        double gf = reuse_gen ? 1 : 0;
        if (!reuse_gen) {
            for (int i = 0; i < 2; i++)
                gf += (gs->state[i] == 1 ? 1.0 : gall[i] ? (double)gci[i] / (double)gall[i] : 0) * (i == 0 ? 0.85 : 0.15);
        }
        uint64_t all_bytes = 0;
        {
            std::lock_guard<std::mutex> g(qm);
            for (auto &u : units) all_bytes += u.bytes;
        }
        double est_files = gens_done ? (double)total : gf > 0.05 ? (double)total / (gf < 1 ? gf : 1) : 1056;
        double est_bytes = gens_done ? (double)all_bytes : gf > 0.05 ? (double)all_bytes / gf : 7.6e8;
        double cf = 0.5 * (double)nd / est_files + 0.5 * (double)bytes_done / (est_bytes > 1 ? est_bytes : 1);
        if (cf > 1) cf = 1;
        double f = 0.15 * gf + 0.80 * cf;
        if (f > shown) shown = f;
        wchar_t s[200];
        if (!gens_done && gf < 1)
            swprintf(s, 200, trw(L"translating the game code to C and compiling it (%zu files done)"), nd);
        else
            swprintf(s, 200, trw(L"compiling (%zu of %zu files)"), nd, total);
        step(shown, s);
        Sleep(150);
    }
    std::string fail_msg;
    {
        std::lock_guard<std::mutex> g(qm);
        stop = true;
        fail_msg = b.fail_msg;
        qcv.notify_all();
    }
    if (failed) {
        // (the threads finish on their own the file they have in hand; the next build waits for them)
        for (HANDLE h : pool) CloseHandle(h);
        int busy = b.alive;
        if (busy) g_log("%d compiler threads are finishing the file they had in the background", busy);
        return fail(why, why_cap, fail_msg);
    }
    for (HANDLE h : pool) WaitForSingleObject(h, INFINITE), CloseHandle(h);  // (no work: they exit right away)
    while (gs->running) Sleep(10);  // (it already said it is done: exits right away; it wrote gen.key itself)
    g_log("compiled %zu files (%zu reused) in %lu s", units.size(), (size_t)nreused, (GetTickCount() - t0) / 1000);

    // the link: gamelib_entry, game_overrides (ahead of the generated code) and the chunks
    step(0.95, trw(L"linking"));
    std::vector<std::wstring> objs;
    for (auto &u : units) objs.push_back(u.obj);
    std::wstring out = P.game + L"\\lasgame.dll", tmp = P.work + L"\\lasgame.dll.tmp";
    std::string diag;
    DWORD tl = GetTickCount();
    bool linked = link_lib(P, objs, tmp, diag);
    g_log("link: %.1f s", (GetTickCount() - tl) / 1000.0);
    if (!linked) {
        write_file(P.work + L"\\link.log", diag);
        // (something that was kept is bad, a corrupted object or a wrong chunk count: next time, everything from
        // scratch)
        DeleteFileW(lp(P.work + L"\\gen.key").c_str());
        DeleteFileW(lp(P.work + L"\\base.txt").c_str());
        size_t k = diag.find('\n');
        return fail(why, why_cap, TR("the game code does not link: ") + (k == std::string::npos ? diag : diag.substr(0, k)) +
                                      TR(" (what was saved is discarded: installing again generates and compiles "
                                      "everything from scratch)"));
    }
    // (without a .cpu, the program requires the highest, x86-64-v3: while the library is being swapped there is never
    // code marked for a CPU that is not its own)
    DeleteFileW(lp(out + L".cpu").c_str());
    if (!MoveFileExW(lp(tmp).c_str(), lp(out).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        return fail(why, why_cap, TR("extracted\\lasgame.dll could not be written (is the game open?)"));
    write_file(out + L".id", full_id + "\n");
    write_file(out + L".cpu", cpu + "\n");
    remove_tree(P.game + L"\\.gamebuild");  // (0.47.0's cache, with zig: no longer of any use)
    PROCESS_MEMORY_COUNTERS pm = {sizeof pm};
    GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof pm);
    g_log("lasgame.dll ready: %.0f MB, %lu s in total, peak memory of the process %.1f GB", file_bytes(out) / 1e6,
          (GetTickCount() - t0) / 1000, pm.PeakWorkingSetSize / 1073741824.0);
    step(1, trw(L"done"));
    return 0;
}

// (if anyone unloaded builder.dll, its exception handler must not be left pointing at code that is gone)
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_DETACH && !reserved && g_guard_veh) RemoveVectoredExceptionHandler(g_guard_veh);
    return TRUE;
}
