#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>

#include <cstdlib>

namespace IntelOpenGLVRFix {

HMODULE SystemOpenGL();
FARPROC SystemExport(const char *name);
PROC SystemWGLProc(const char *name);
bool IsValidWGLProc(PROC proc);
void LogMessage(const char *format, ...);

PROC InteropProcAddress(const char *name);

} // namespace IntelOpenGLVRFix
