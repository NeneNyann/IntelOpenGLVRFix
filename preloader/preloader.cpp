#include <windows.h>
#include <GL/gl.h>

#pragma comment(linker, "/include:PreloadOpenGL")

extern "C" __declspec(noinline) PROC WINAPI PreloadOpenGL(LPCSTR name) {
    return wglGetProcAddress(name);
}
