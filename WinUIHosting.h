#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

HWND CreateWinUIHostWindow(HWND parent, int width, int height);
void DestroyWinUIHostWindow(HWND host);

#ifdef __cplusplus
}
#endif
