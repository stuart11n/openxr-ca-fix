#pragma once

// Appends to %LOCALAPPDATA%\openxr-ca-fix\layer-<exe>.log and mirrors to OutputDebugString.
void Log(const char* fmt, ...);
