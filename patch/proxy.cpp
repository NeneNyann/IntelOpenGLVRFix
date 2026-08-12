#include "runtime.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

HMODULE G_SELF_MODULE = nullptr;
INIT_ONCE G_SYSTEM_ONCE = INIT_ONCE_STATIC_INIT;
HMODULE G_SYSTEM_OPENGL = nullptr;
INIT_ONCE G_LOG_ONCE = INIT_ONCE_STATIC_INIT;
HANDLE G_LOG = INVALID_HANDLE_VALUE;
SRWLOCK G_LOG_LOCK = SRWLOCK_INIT;

BOOL CALLBACK InitializeSystemOpenGL(PINIT_ONCE, PVOID, PVOID *) {
    wchar_t path[32768] = {};
    const UINT length = GetSystemDirectoryW(path, static_cast<UINT>(_countof(path)));
    if (length == 0 || length >= _countof(path))
        return TRUE;
    if (wcscat_s(path, _countof(path), L"\\opengl32.dll") != 0)
        return TRUE;

    HMODULE module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module && module != G_SELF_MODULE)
        G_SYSTEM_OPENGL = module;
    return TRUE;
}

BOOL CALLBACK InitializeLog(PINIT_ONCE, PVOID, PVOID *) {
    wchar_t path[32768] = {};
    const DWORD length = GetEnvironmentVariableW(L"INTEL_OPENGL_VR_FIX_LOG", path, static_cast<DWORD>(_countof(path)));
    if (length == 0 || length >= _countof(path))
        return TRUE;

    G_LOG = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    return TRUE;
}

} // namespace

namespace IntelOpenGLVRFix {

HMODULE SystemOpenGL() {
    InitOnceExecuteOnce(&G_SYSTEM_ONCE, InitializeSystemOpenGL, nullptr, nullptr);
    return G_SYSTEM_OPENGL;
}

FARPROC SystemExport(const char *name) {
    HMODULE module = SystemOpenGL();
    return module && name ? GetProcAddress(module, name) : nullptr;
}

bool IsValidWGLProc(PROC proc) {
    const auto value = reinterpret_cast<UINT_PTR>(proc);
    return proc && value != 1 && value != 2 && value != 3 && value != static_cast<UINT_PTR>(-1);
}

PROC SystemWGLProc(const char *name) {
    using GetProcAddressFn = PROC(WINAPI *)(LPCSTR);
    const auto getProc = reinterpret_cast<GetProcAddressFn>(SystemExport("wglGetProcAddress"));
    if (!getProc || !name)
        return nullptr;
    PROC proc = getProc(name);
    return IsValidWGLProc(proc) ? proc : nullptr;
}

void LogMessage(const char *format, ...) {
    const DWORD savedError = GetLastError();
    InitOnceExecuteOnce(&G_LOG_ONCE, InitializeLog, nullptr, nullptr);
    if (G_LOG == INVALID_HANDLE_VALUE) {
        SetLastError(savedError);
        return;
    }

    char message[2048] = {};
    va_list arguments;
    va_start(arguments, format);
    const int messageLength = _vsnprintf_s(message, _countof(message), _TRUNCATE, format, arguments);
    va_end(arguments);

    SYSTEMTIME time = {};
    GetLocalTime(&time);
    char record[2304] = {};
    const int recordLength = _snprintf_s(record, _countof(record), _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu tid=%lu %s\r\n",
                                         time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
                                         GetCurrentProcessId(), GetCurrentThreadId(), messageLength >= 0 ? message : "log truncated");

    if (recordLength > 0) {
        AcquireSRWLockExclusive(&G_LOG_LOCK);
        DWORD written = 0;
        WriteFile(G_LOG, record, static_cast<DWORD>(recordLength), &written, nullptr);
        ReleaseSRWLockExclusive(&G_LOG_LOCK);
    }
    SetLastError(savedError);
}

} // namespace IntelOpenGLVRFix

extern "C" PROC WINAPI ProxyWGLGetProcAddress(LPCSTR name) {
    if (!name)
        return nullptr;
    if (PROC local = IntelOpenGLVRFix::InteropProcAddress(name))
        return local;
    return IntelOpenGLVRFix::SystemWGLProc(name);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        G_SELF_MODULE = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
