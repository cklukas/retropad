#pragma once

#include <windows.h>

typedef struct PrintRenderContext {
    const WCHAR *text;
    const WCHAR *fullPath;
    RECT marginsThousandths;
    const WCHAR *headerText;
    const WCHAR *footerText;
} PrintRenderContext;

typedef struct PrintMetrics {
    int lineHeight;
    int averageCharWidth;
    int dpiX;
    int dpiY;
} PrintMetrics;

typedef struct PrintRenderTarget PrintRenderTarget;
typedef struct PrintRenderTargetOps PrintRenderTargetOps;

struct PrintRenderTarget {
    const PrintRenderTargetOps *ops;
    void *userData;
};

struct PrintRenderTargetOps {
    BOOL (*GetMetrics)(void *userData, PrintMetrics *metrics);
    BOOL (*GetPageSize)(void *userData, int *width, int *height);
    BOOL (*BeginDocument)(void *userData, const WCHAR *docName, int totalPages);
    BOOL (*EndDocument)(void *userData);
    BOOL (*AbortDocument)(void *userData);
    BOOL (*BeginPage)(void *userData, int pageNumber);
    BOOL (*EndPage)(void *userData);
    BOOL (*DrawText)(void *userData, int x, int y, const WCHAR *text, int length);
    BOOL (*MeasureText)(void *userData, const WCHAR *text, int length, SIZE *size);
};

#ifdef __cplusplus
extern "C" {
#endif

BOOL RenderDocument(const PrintRenderContext *ctx, const PrintRenderTarget *target);

typedef struct HdcRenderTarget {
    PrintRenderTarget base;
    HDC hdc;
    BOOL documentStarted;
} HdcRenderTarget;

void InitHdcRenderTarget(HdcRenderTarget *target, HDC hdc);

#ifdef __cplusplus
}
#endif
