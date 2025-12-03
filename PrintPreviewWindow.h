#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

struct PrintRenderContext;

BOOL InitWindowsAppRuntime(void);
void ShutdownWindowsAppRuntime(void);
BOOL ModernPreviewAvailable(void);
BOOL ShowModernPrintPreview(HWND parent, struct PrintRenderContext *ctx);

#ifdef __cplusplus
}
#endif
