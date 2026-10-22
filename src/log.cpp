#include "log.h"

#include <windows.h>
#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;

FILE* OpenLog() {
    char base[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return nullptr;
    std::string dir = std::string(base) + "\\openxr-ca-fix";
    CreateDirectoryA(dir.c_str(), nullptr);
    // One log per executable, overwritten each run, e.g. layer-hl2.exe.log.
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    const char* exe = strrchr(exePath, '\\');
    exe = exe ? exe + 1 : exePath;
    return _fsopen((dir + "\\layer-" + exe + ".log").c_str(), "w", _SH_DENYWR);
}

}  // namespace

void Log(const char* fmt, ...) {
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1100];
    snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%lu] %s\n", t.wHour, t.wMinute, t.wSecond,
             t.wMilliseconds, GetCurrentProcessId(), msg);

    OutputDebugStringA(line);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) g_file = OpenLog();
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
    }
}
