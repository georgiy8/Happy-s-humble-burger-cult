/* HHBC scout: freestanding (no CRT, no windows.h) x64 Windows DLL.
 * Built as version.dll: forwards the real version.dll exports to the system copy,
 * and starts one worker thread that waits for IL2CPP to be ready, then writes
 *   hhbc_scout.log, hhbc_classes.txt, hhbc_details.txt
 * next to the game exe. Read-only: no hooks, no memory writes, no network. */
#include "dumper.h"
#include "mod.h"

typedef unsigned long DWORD;
typedef int BOOL;
typedef void *HANDLE;

#define W32 __declspec(dllimport)
W32 HANDLE CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
W32 BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
W32 BOOL CloseHandle(HANDLE);
W32 void *GetModuleHandleA(const char *);
W32 void *GetProcAddress(void *, const char *);
W32 DWORD GetModuleFileNameA(void *, char *, DWORD);
W32 HANDLE CreateThread(void *, unsigned long long, DWORD(*)(void *), void *, DWORD, DWORD *);
W32 void Sleep(DWORD);
W32 unsigned long long GetTickCount64(void);
W32 void *LoadLibraryA(const char *);
W32 const char *GetCommandLineA(void);
W32 void *AddVectoredExceptionHandler(unsigned long, long (*)(void *));
W32 unsigned int GetSystemDirectoryA(char *, unsigned int);
W32 DWORD GetCurrentProcessId(void);

/* Real version.dll API: each export is a tiny jump through a table that DllMain
 * fills from the system copy (loaded by full path). No forwarder strings. */
void *g_fn[16];
#define STUB(idx, name)                                                       \
    __declspec(dllexport) __attribute__((naked)) void name(void) {            \
        __asm__ volatile("jmp *g_fn+" #idx "*8(%rip)");                       \
    }
STUB(0, GetFileVersionInfoA)
STUB(1, GetFileVersionInfoW)
STUB(2, GetFileVersionInfoExA)
STUB(3, GetFileVersionInfoExW)
STUB(4, GetFileVersionInfoSizeA)
STUB(5, GetFileVersionInfoSizeW)
STUB(6, GetFileVersionInfoSizeExA)
STUB(7, GetFileVersionInfoSizeExW)
STUB(8, VerFindFileA)
STUB(9, VerFindFileW)
STUB(10, VerInstallFileA)
STUB(11, VerInstallFileW)
STUB(12, VerLanguageNameA)
STUB(13, VerLanguageNameW)
STUB(14, VerQueryValueA)
STUB(15, VerQueryValueW)

static const char *const g_names[16] = {
    "GetFileVersionInfoA", "GetFileVersionInfoW", "GetFileVersionInfoExA",
    "GetFileVersionInfoExW", "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeW",
    "GetFileVersionInfoSizeExA", "GetFileVersionInfoSizeExW", "VerFindFileA",
    "VerFindFileW", "VerInstallFileA", "VerInstallFileW", "VerLanguageNameA",
    "VerLanguageNameW", "VerQueryValueA", "VerQueryValueW"};

static void load_real_version(void) {
    char path[300];
    unsigned int n = GetSystemDirectoryA(path, 260);
    const char *tail = "\\version.dll";
    void *m;
    int i;
    if (!n || n > 260) return;
    for (i = 0; tail[i]; i++) path[n + i] = tail[i];
    path[n + i] = 0;
    m = LoadLibraryA(path);
    if (!m) return;
    for (i = 0; i < 16; i++) g_fn[i] = GetProcAddress(m, g_names[i]);
}

int _fltused = 0; /* required by the MSVC ABI when floats are used without the CRT */
static Mod g_mod;
#include "ui.h"

void plat_write(void *h, const char *data, u32 len) {
    DWORD w = 0;
    if (h && h != (HANDLE)(long long)-1) WriteFile(h, data, len, &w, 0);
}
void *plat_getproc(void *module, const char *name) { return GetProcAddress(module, name); }

/* The compiler may emit these even in freestanding mode. */
void *memset(void *d, int c, unsigned long long n) {
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned long long n) {
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    while (n--) *p++ = *q++;
    return d;
}

static Out g_log, g_classes, g_details;
static char g_dir[300];
static char g_exe[300];
static unsigned long long g_t0;
static volatile int g_exc_logged;

static HANDLE open_path(const char *name) {
    char path[400];
    int i = 0, j = 0;
    while (g_dir[i]) { path[i] = g_dir[i]; i++; }
    while (name[j]) path[i++] = name[j++];
    path[i] = 0;
    /* GENERIC_WRITE, share read, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL */
    return CreateFileA(path, 0x40000000, 1, 0, 2, 0x80, 0);
}

static void stamp(void) {
    out_s(&g_log, "[+");
    out_u(&g_log, (GetTickCount64() - g_t0) / 1000);
    out_s(&g_log, "s] ");
}
static void L(const char *s) {
    stamp();
    out_s(&g_log, s);
    out_ch(&g_log, '\n');
    out_flush(&g_log);
}
static void LX(const char *s, u64 v) {
    stamp();
    out_s(&g_log, s);
    out_x(&g_log, v);
    out_ch(&g_log, '\n');
    out_flush(&g_log);
}
static void LS(const char *s, const char *v) {
    stamp();
    out_s(&g_log, s);
    out_s(&g_log, v);
    out_ch(&g_log, '\n');
    out_flush(&g_log);
}

/* Log serious exceptions (crash diagnostics); never handles them. */
typedef struct { DWORD code, flags; void *next; void *addr; } ER;
typedef struct { ER *rec; void *ctx; } EP;
static long veh(void *ep) {
    ER *r = ((EP *)ep)->rec;
    DWORD c = r->code;
    if ((c == 0xC0000005 || c == 0xC0000409 || c == 0xC00000FD || c == 0xC0000374 ||
         c == 0xC000001D || c == 0xC0000096) && g_exc_logged < 20) {
        g_exc_logged++;
        LX("EXCEPTION code: ", c);
        LX("EXCEPTION address: ", (u64)r->addr);
    }
    return 0; /* EXCEPTION_CONTINUE_SEARCH */
}

static void make_log_name(char *dst, DWORD pid) {
    const char *a = "hhbc_scout_";
    const char *b = ".log";
    char t[12];
    int n = 0, i = 0, k;
    while (*a) dst[i++] = *a++;
    if (!pid) t[n++] = '0';
    while (pid) { t[n++] = (char)('0' + pid % 10); pid /= 10; }
    while (n) dst[i++] = t[--n];
    for (k = 0; b[k]; k++) dst[i++] = b[k];
    dst[i] = 0;
}

static DWORD scout_thread(void *unused) {
    void *ga = 0, *domain = 0, *image = 0;
    int i;
    (void)unused;

    L("thread running");
    if (contains_ci(g_exe, "crash")) {
        L("this is a crash-handler process, scout does nothing here");
        return 9;
    }
    LX("UnityPlayer.dll handle: ", (u64)GetModuleHandleA("UnityPlayer.dll"));
    g_mod.speed_pct = 150;
    g_ui_mod = &g_mod;
    {
        HANDLE ut = CreateThread(0, 0, ui_thread, 0, 0, 0);
        if (ut) CloseHandle(ut); else L("UI thread FAILED");
    }

    for (i = 0; i < 600 && !ga; i++) {
        ga = GetModuleHandleA("GameAssembly.dll");
        if (!ga) {
            if (i < 12 || i % 10 == 0) LX("waiting for GameAssembly.dll, tick ", (u64)i);
            Sleep(500);
        }
    }
    if (!ga) { L("GameAssembly.dll never loaded"); return 1; }
    LX("GameAssembly.dll handle: ", (u64)ga);
    if (resolve_api(ga, &g_log)) {
        L("required il2cpp exports missing, aborting");
        return 2;
    }
    L("api resolved, waiting for il2cpp domain");

    for (i = 0; i < 600 && !domain; i++) {
        domain = il2cpp_domain_get();
        if (!domain) {
            if (i % 10 == 0) LX("waiting for domain, tick ", (u64)i);
            Sleep(500);
        }
    }
    if (!domain) { L("il2cpp domain never appeared"); return 3; }
    LX("domain ok: ", (u64)domain);
    L("settling 10s");
    Sleep(10000);

    L("attaching thread");
    il2cpp_thread_attach(domain);
    L("attached, looking for Assembly-CSharp.dll");
    for (i = 0; i < 240 && !image; i++) {
        image = find_game_image(domain);
        if (!image) {
            if (i % 10 == 0) LX("waiting for Assembly-CSharp, tick ", (u64)i);
            Sleep(500);
        }
    }
    if (!image) { L("Assembly-CSharp.dll not found"); return 4; }
    L("Assembly-CSharp found, dumping");

    g_classes.h = open_path("hhbc_classes.txt");
    g_details.h = open_path("hhbc_details.txt");
    run_dump(image, &g_log, &g_classes, &g_details);
    CloseHandle(g_classes.h);
    CloseHandle(g_details.h);
    L("dump done, starting mod loop");
    if (mod_init(&g_mod, image, &g_log)) {
        L("mod active");
        for (;;) {
            mod_tick(&g_mod);
            Sleep(50);
        }
    }
    L("mod not active (see messages above)");
    return 0;
}

BOOL __stdcall DllMain(void *inst, DWORD reason, void *reserved) {
    (void)inst;
    if (reason == 1) { /* DLL_PROCESS_ATTACH */
        HANDLE t;
        char name[40];
        DWORD n;
        g_t0 = GetTickCount64();
        load_real_version();
        n = GetModuleFileNameA(0, g_exe, sizeof(g_exe) - 1);
        g_exe[n] = 0;
        {
            int k;
            for (k = 0; k < (int)n + 1; k++) g_dir[k] = g_exe[k];
            while (n > 0 && g_dir[n - 1] != '\\') n--;
            g_dir[n] = 0;
        }
        make_log_name(name, GetCurrentProcessId());
        g_log.h = open_path(name);
        L("scout v5: DllMain attach");
        LS("exe: ", g_exe);
        LS("command line: ", GetCommandLineA());
        AddVectoredExceptionHandler(1, veh);
        t = CreateThread(0, 0, scout_thread, 0, 0, 0);
        if (t) CloseHandle(t); else L("CreateThread FAILED");
    } else if (reason == 0) { /* DLL_PROCESS_DETACH */
        L(reserved ? "DllMain detach: process is EXITING" : "DllMain detach: DLL unloaded");
    }
    return 1;
}
