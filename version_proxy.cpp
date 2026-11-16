#include "pch.h"

#include <Windows.h>
#include <cwchar>

namespace
{
    HMODULE g_realVersion = nullptr;
    INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;

    using FnGetFileVersionInfoA =
        BOOL(WINAPI*)(LPCSTR, DWORD, DWORD, LPVOID);

    using FnGetFileVersionInfoW =
        BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);

    using FnGetFileVersionInfoExA =
        BOOL(WINAPI*)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);

    using FnGetFileVersionInfoExW =
        BOOL(WINAPI*)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);

    using FnGetFileVersionInfoSizeA =
        DWORD(WINAPI*)(LPCSTR, LPDWORD);

    using FnGetFileVersionInfoSizeW =
        DWORD(WINAPI*)(LPCWSTR, LPDWORD);

    using FnGetFileVersionInfoSizeExA =
        DWORD(WINAPI*)(DWORD, LPCSTR, LPDWORD);

    using FnGetFileVersionInfoSizeExW =
        DWORD(WINAPI*)(DWORD, LPCWSTR, LPDWORD);

    using FnVerFindFileA =
        DWORD(WINAPI*)(DWORD, LPSTR, LPSTR, LPSTR, LPSTR,
            PUINT, LPSTR, PUINT);

    using FnVerFindFileW =
        DWORD(WINAPI*)(DWORD, LPWSTR, LPWSTR, LPWSTR, LPWSTR,
            PUINT, LPWSTR, PUINT);

    using FnVerInstallFileA =
        DWORD(WINAPI*)(DWORD, LPSTR, LPSTR, LPSTR, LPSTR,
            LPSTR, LPSTR, PUINT);

    using FnVerInstallFileW =
        DWORD(WINAPI*)(DWORD, LPWSTR, LPWSTR, LPWSTR, LPWSTR,
            LPWSTR, LPWSTR, PUINT);

    using FnVerLanguageNameA =
        DWORD(WINAPI*)(DWORD, LPSTR, DWORD);

    using FnVerLanguageNameW =
        DWORD(WINAPI*)(DWORD, LPWSTR, DWORD);

    using FnVerQueryValueA =
        BOOL(WINAPI*)(LPCVOID, LPCSTR, LPVOID*, PUINT);

    using FnVerQueryValueW =
        BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);

    using FnGetFileVersionInfoByHandle =
        BOOL(WINAPI*)(DWORD, HANDLE, LPVOID*, PDWORD);

    FnGetFileVersionInfoA pGetFileVersionInfoA = nullptr;
    FnGetFileVersionInfoW pGetFileVersionInfoW = nullptr;
    FnGetFileVersionInfoExA pGetFileVersionInfoExA = nullptr;
    FnGetFileVersionInfoExW pGetFileVersionInfoExW = nullptr;
    FnGetFileVersionInfoSizeA pGetFileVersionInfoSizeA = nullptr;
    FnGetFileVersionInfoSizeW pGetFileVersionInfoSizeW = nullptr;
    FnGetFileVersionInfoSizeExA pGetFileVersionInfoSizeExA = nullptr;
    FnGetFileVersionInfoSizeExW pGetFileVersionInfoSizeExW = nullptr;
    FnVerFindFileA pVerFindFileA = nullptr;
    FnVerFindFileW pVerFindFileW = nullptr;
    FnVerInstallFileA pVerInstallFileA = nullptr;
    FnVerInstallFileW pVerInstallFileW = nullptr;
    FnVerLanguageNameA pVerLanguageNameA = nullptr;
    FnVerLanguageNameW pVerLanguageNameW = nullptr;
    FnVerQueryValueA pVerQueryValueA = nullptr;
    FnVerQueryValueW pVerQueryValueW = nullptr;
    FnGetFileVersionInfoByHandle pGetFileVersionInfoByHandle = nullptr;

    FARPROC GetRealProc(const char* name)
    {
        if (!g_realVersion)
            return nullptr;

        return GetProcAddress(g_realVersion, name);
    }

    BOOL CALLBACK InitializeVersionProxy(
        PINIT_ONCE,
        PVOID,
        PVOID*)
    {
        wchar_t path[MAX_PATH]{};

        UINT length = GetSystemDirectoryW(path, MAX_PATH);

        if (length == 0 || length >= MAX_PATH)
            return TRUE;

        if (length + 13 >= MAX_PATH)
            return TRUE;

        wcscat_s(path, MAX_PATH, L"\\version.dll");

        g_realVersion = LoadLibraryW(path);

        if (!g_realVersion)
            return TRUE;

        pGetFileVersionInfoA =
            reinterpret_cast<FnGetFileVersionInfoA>(
                GetRealProc("GetFileVersionInfoA"));

        pGetFileVersionInfoW =
            reinterpret_cast<FnGetFileVersionInfoW>(
                GetRealProc("GetFileVersionInfoW"));

        pGetFileVersionInfoExA =
            reinterpret_cast<FnGetFileVersionInfoExA>(
                GetRealProc("GetFileVersionInfoExA"));

        pGetFileVersionInfoExW =
            reinterpret_cast<FnGetFileVersionInfoExW>(
                GetRealProc("GetFileVersionInfoExW"));

        pGetFileVersionInfoSizeA =
            reinterpret_cast<FnGetFileVersionInfoSizeA>(
                GetRealProc("GetFileVersionInfoSizeA"));

        pGetFileVersionInfoSizeW =
            reinterpret_cast<FnGetFileVersionInfoSizeW>(
                GetRealProc("GetFileVersionInfoSizeW"));

        pGetFileVersionInfoSizeExA =
            reinterpret_cast<FnGetFileVersionInfoSizeExA>(
                GetRealProc("GetFileVersionInfoSizeExA"));

        pGetFileVersionInfoSizeExW =
            reinterpret_cast<FnGetFileVersionInfoSizeExW>(
                GetRealProc("GetFileVersionInfoSizeExW"));

        pVerFindFileA =
            reinterpret_cast<FnVerFindFileA>(
                GetRealProc("VerFindFileA"));

        pVerFindFileW =
            reinterpret_cast<FnVerFindFileW>(
                GetRealProc("VerFindFileW"));

        pVerInstallFileA =
            reinterpret_cast<FnVerInstallFileA>(
                GetRealProc("VerInstallFileA"));

        pVerInstallFileW =
            reinterpret_cast<FnVerInstallFileW>(
                GetRealProc("VerInstallFileW"));

        pVerLanguageNameA =
            reinterpret_cast<FnVerLanguageNameA>(
                GetRealProc("VerLanguageNameA"));

        pVerLanguageNameW =
            reinterpret_cast<FnVerLanguageNameW>(
                GetRealProc("VerLanguageNameW"));

        pVerQueryValueA =
            reinterpret_cast<FnVerQueryValueA>(
                GetRealProc("VerQueryValueA"));

        pVerQueryValueW =
            reinterpret_cast<FnVerQueryValueW>(
                GetRealProc("VerQueryValueW"));

        pGetFileVersionInfoByHandle =
            reinterpret_cast<FnGetFileVersionInfoByHandle>(
                GetRealProc("GetFileVersionInfoByHandle"));

        return TRUE;
    }

    void EnsureVersionProxy()
    {
        InitOnceExecuteOnce(
            &g_initOnce,
            InitializeVersionProxy,
            nullptr,
            nullptr
        );
        
    }
}

// Экспортируемые прокси-функции.
// Их реальные имена задаются в version.def.

extern "C" BOOL WINAPI Proxy_GetFileVersionInfoA(
    LPCSTR file, DWORD handle, DWORD length, LPVOID data)
{
    EnsureVersionProxy();
    return pGetFileVersionInfoA
        ? pGetFileVersionInfoA(file, handle, length, data)
        : FALSE;
}

extern "C" BOOL WINAPI Proxy_GetFileVersionInfoW(
    LPCWSTR file, DWORD handle, DWORD length, LPVOID data)
{
    EnsureVersionProxy();
    return pGetFileVersionInfoW
        ? pGetFileVersionInfoW(file, handle, length, data)
        : FALSE;
}

extern "C" BOOL WINAPI Proxy_GetFileVersionInfoExA(
    DWORD flags, LPCSTR file, DWORD handle, DWORD length, LPVOID data)
{
    EnsureVersionProxy();
    return pGetFileVersionInfoExA
        ? pGetFileVersionInfoExA(flags, file, handle, length, data)
        : FALSE;
}

extern "C" BOOL WINAPI Proxy_GetFileVersionInfoExW(
    DWORD flags, LPCWSTR file, DWORD handle, DWORD length, LPVOID data)
{
    EnsureVersionProxy();
    return pGetFileVersionInfoExW
        ? pGetFileVersionInfoExW(flags, file, handle, length, data)
        : FALSE;
}

extern "C" DWORD WINAPI Proxy_GetFileVersionInfoSizeA(
    LPCSTR file, LPDWORD handle)
{
    EnsureVersionProxy();
    return pGetFileVersionInfoSizeA
        ? pGetFileVersionInfoSizeA(file, handle)
        : 0;
}

extern "C" DWORD WINAPI Proxy_GetFileVersionInfoSizeW(
    LPCWSTR file, LPDWORD handle)
{
    EnsureVersionProxy();
    return pGetFileVersionInfoSizeW
        ? pGetFileVersionInfoSizeW(file, handle)
        : 0;
}

extern "C" DWORD WINAPI Proxy_GetFileVersionInfoSizeExA(
    DWORD flags, LPCSTR file, LPDWORD handle)
{
    EnsureVersionProxy();
    return pGetFileVersionInfoSizeExA
        ? pGetFileVersionInfoSizeExA(flags, file, handle)
        : 0;
}

extern "C" DWORD WINAPI Proxy_GetFileVersionInfoSizeExW(
    DWORD flags, LPCWSTR file, LPDWORD handle)
{
    EnsureVersionProxy();
    return pGetFileVersionInfoSizeExW
        ? pGetFileVersionInfoSizeExW(flags, file, handle)
        : 0;
}

extern "C" DWORD WINAPI Proxy_VerFindFileA(
    DWORD flags, LPSTR file, LPSTR winDir, LPSTR appDir,
    LPSTR curDir, PUINT curLen, LPSTR destDir, PUINT destLen)
{
    EnsureVersionProxy();
    return pVerFindFileA
        ? pVerFindFileA(flags, file, winDir, appDir,
            curDir, curLen, destDir, destLen)
        : 0;
}

extern "C" DWORD WINAPI Proxy_VerFindFileW(
    DWORD flags, LPWSTR file, LPWSTR winDir, LPWSTR appDir,
    LPWSTR curDir, PUINT curLen, LPWSTR destDir, PUINT destLen)
{
    EnsureVersionProxy();
    return pVerFindFileW
        ? pVerFindFileW(flags, file, winDir, appDir,
            curDir, curLen, destDir, destLen)
        : 0;
}

extern "C" DWORD WINAPI Proxy_VerInstallFileA(
    DWORD flags, LPSTR srcFile, LPSTR destFile, LPSTR srcDir,
    LPSTR destDir, LPSTR curDir, LPSTR tempFile, PUINT tempLen)
{
    EnsureVersionProxy();
    return pVerInstallFileA
        ? pVerInstallFileA(flags, srcFile, destFile, srcDir,
            destDir, curDir, tempFile, tempLen)
        : 0;
}

extern "C" DWORD WINAPI Proxy_VerInstallFileW(
    DWORD flags, LPWSTR srcFile, LPWSTR destFile, LPWSTR srcDir,
    LPWSTR destDir, LPWSTR curDir, LPWSTR tempFile, PUINT tempLen)
{
    EnsureVersionProxy();
    return pVerInstallFileW
        ? pVerInstallFileW(flags, srcFile, destFile, srcDir,
            destDir, curDir, tempFile, tempLen)
        : 0;
}

extern "C" DWORD WINAPI Proxy_VerLanguageNameA(
    DWORD language, LPSTR buffer, DWORD size)
{
    EnsureVersionProxy();
    return pVerLanguageNameA
        ? pVerLanguageNameA(language, buffer, size)
        : 0;
}

extern "C" DWORD WINAPI Proxy_VerLanguageNameW(
    DWORD language, LPWSTR buffer, DWORD size)
{
    EnsureVersionProxy();
    return pVerLanguageNameW
        ? pVerLanguageNameW(language, buffer, size)
        : 0;
}

extern "C" BOOL WINAPI Proxy_VerQueryValueA(
    LPCVOID block, LPCSTR subBlock, LPVOID* buffer, PUINT length)
{
    EnsureVersionProxy();
    return pVerQueryValueA
        ? pVerQueryValueA(block, subBlock, buffer, length)
        : FALSE;
}

extern "C" BOOL WINAPI Proxy_VerQueryValueW(
    LPCVOID block, LPCWSTR subBlock, LPVOID* buffer, PUINT length)
{
    EnsureVersionProxy();
    return pVerQueryValueW
        ? pVerQueryValueW(block, subBlock, buffer, length)
        : FALSE;
}

extern "C" BOOL WINAPI Proxy_GetFileVersionInfoByHandle(
    DWORD dwFlags,
    HANDLE hFile,
    LPVOID* lplpData,
    PDWORD pdwLen)
{
    EnsureVersionProxy();

    return pGetFileVersionInfoByHandle
        ? pGetFileVersionInfoByHandle(
            dwFlags,
            hFile,
            lplpData,
            pdwLen)
        : FALSE;
}