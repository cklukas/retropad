#include <windows.h>

#include "WinUIHosting.h"

HWND CreateWinUIHostWindow(HWND parent, int width, int height) {
    (void)parent;
    (void)width;
    (void)height;
    return NULL;
}

void DestroyWinUIHostWindow(HWND host) {
    (void)host;
}
