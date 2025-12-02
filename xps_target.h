#pragma once

#include <windows.h>
#include <xpsobjectmodel.h>

#include "rendering.h"

typedef struct XpsRenderTarget {
    PrintRenderTarget base;
    IXpsOMObjectFactory *factory;
    IXpsOMPackageWriter *writer;
    IXpsOMPage *currentPage;
    IXpsOMFontResource *font;
    IOpcPartUri *fontUri;
    XPS_SIZE pageSize;
    FLOAT dpiX;
    FLOAT dpiY;
    UINT32 pageNumber;
} XpsRenderTarget;

#ifdef __cplusplus
extern "C" {
#endif

HRESULT InitXpsRenderTarget(XpsRenderTarget *target, IXpsOMObjectFactory *factory, IXpsOMPackageWriter *writer, FLOAT width, FLOAT height, FLOAT dpiX, FLOAT dpiY);
void ReleaseXpsRenderTarget(XpsRenderTarget *target);

#ifdef __cplusplus
}
#endif
