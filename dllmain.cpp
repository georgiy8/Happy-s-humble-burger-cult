#include "pch.h"
#include "common.h"
#include "il2cpp.h"

#include <MinHook.h>

#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <deque>
#include <mutex>

State g;
std::string g_gameDir;

// ---------------------------------------------------------------- logging
namespace {
FILE* s_log = nullptr;
std::mutex s_mtx;
std::deque<std::string> s_tail;
}

void log_open() {
    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    slash[1] = '\0';
    g_gameDir = path;
    std::string file = g_gameDir + "hhbc_mod.log";
    std::lock_guard<std::mutex> lk(s_mtx);
    if (!s_log) s_log = _fsopen(file.c_str(), "w", _SH_DENYWR);
}

void logf(const char* fmt, ...) {
    char msg[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    char line[700];
    snprintf(line, sizeof(line), "[%llu s] %s", GetTickCount64() / 1000ULL, msg);
    OutputDebugStringA("[HHBC] ");
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_log) {
        fputs(line, s_log);
        fputc('\n', s_log);
        fflush(s_log);
    }
    s_tail.emplace_back(line);
    while (s_tail.size() > 60) s_tail.pop_front();
}

void log_tail(std::vector<std::string>& out, size_t maxLines) {
    std::lock_guard<std::mutex> lk(s_mtx);
    size_t start = s_tail.size() > maxLines ? s_tail.size() - maxLines : 0;
    out.assign(s_tail.begin() + static_cast<std::ptrdiff_t>(start), s_tail.end());
}

// ---------------------------------------------------------------- worker
namespace {

DWORD WINAPI Worker(LPVOID) {
    log_open();
    logf("HHBC mod starting (pid %lu)", GetCurrentProcessId());

    HMODULE ga = nullptr;
    for (int i = 0; i < 240 && !ga; i++) {
        ga = GetModuleHandleA("GameAssembly.dll");
        if (!ga) Sleep(500);
    }
    if (!ga) { logf("GameAssembly.dll never loaded"); return 1; }
    if (!il2::init(ga)) { logf("required il2cpp exports missing"); return 2; }

    void* domain = nullptr;
    for (int i = 0; i < 240 && !domain; i++) {
        domain = il2::domain_get();
        if (!domain) Sleep(500);
    }
    if (!domain) { logf("il2cpp domain never appeared"); return 3; }
    logf("il2cpp domain ready, settling 8s");
    Sleep(8000);
    il2::thread_attach(domain);

    void* image = nullptr;
    for (int i = 0; i < 240 && !image; i++) {
        image = il2::find_image("Assembly-CSharp.dll");
        if (!image) Sleep(500);
    }
    if (!image) { logf("Assembly-CSharp.dll not found"); return 4; }
    logf("Assembly-CSharp found");

    bool playerOk = features_init(image);
    logf("features_init: %s", playerOk ? "ok" : "player fields missing");

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        logf("MH_Initialize failed: %s", MH_StatusToString(st));
        return 5;
    }
    features_hook();
    if (!overlay_install()) logf("overlay NOT installed (menu unavailable, hotkeys unavailable)");
    g.ready = 1;
    logf("HHBC mod ready");

    for (;;) {
        Sleep(250);
    }
}

bool is_crash_handler() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* p = path;
    for (; *p; ++p) *p = static_cast<char>(tolower(static_cast<unsigned char>(*p)));
    return strstr(path, "crashhandler") != nullptr;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (is_crash_handler()) return TRUE;   // UnityCrashHandler64.exe also loads version.dll: do nothing there
        HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
