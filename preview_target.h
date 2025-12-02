#pragma once

#include <windows.h>
#include <PrintPreview.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PreviewTarget PreviewTarget;

struct PrintRenderContext;

HRESULT CreatePreviewTarget(PreviewTarget **outTarget);
void ReleasePreviewTarget(PreviewTarget *target);
IUnknown *PreviewTargetAsUnknown(PreviewTarget *target);
HRESULT PreviewTargetSetRenderer(PreviewTarget *target, struct PrintRenderContext *ctx, FLOAT dpiX, FLOAT dpiY, FLOAT pageWidth, FLOAT pageHeight);

#ifdef __cplusplus
}
#endif
