#define INITGUID
#include <stddef.h>
#pragma warning(push)
#pragma warning(disable : 4201) // silence nameless struct/union from Windows headers
#include <dxgi.h>
#include <d3d11.h>
#include <PrintPreview.h>
#pragma warning(pop)
#include "preview_target.h"
#include "rendering.h"
#include <strsafe.h>

static void DebugLog(const WCHAR *msg) {
    WCHAR path[MAX_PATH];
    DWORD len = GetTempPathW(ARRAYSIZE(path), path);
    if (len == 0 || len >= ARRAYSIZE(path)) return;
    if (FAILED(StringCchCatW(path, ARRAYSIZE(path), L"retropad_preview.log"))) return;
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR line[512];
    StringCchPrintfW(line, ARRAYSIZE(line), L"[%02u:%02u:%02u.%03u] %s\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg ? msg : L"(null)");
    int bytes = (int)(lstrlenW(line) * sizeof(WCHAR));
    WriteFile(h, line, bytes, &written, NULL);
    CloseHandle(h);
}

// {1A6DD0AD-1E2A-4E99-A5BA-91F17818290E}
DEFINE_GUID(IID_IPrintPreviewDxgiPackageTarget, 0x1a6dd0ad, 0x1e2a, 0x4e99, 0xa5, 0xba, 0x91, 0xf1, 0x78, 0x18, 0x29, 0x0e);

struct PreviewTarget {
    IPrintPreviewDxgiPackageTarget vtbl;
    LONG refCount;
    PrintRenderContext *ctx;
    FLOAT dpiX;
    FLOAT dpiY;
    FLOAT pageWidth;
    FLOAT pageHeight;
};

static HRESULT STDMETHODCALLTYPE Prev_QueryInterface(IPrintPreviewDxgiPackageTarget *self, REFIID riid, void **ppv);
static ULONG STDMETHODCALLTYPE Prev_AddRef(IPrintPreviewDxgiPackageTarget *self);
static ULONG STDMETHODCALLTYPE Prev_Release(IPrintPreviewDxgiPackageTarget *self);
static HRESULT STDMETHODCALLTYPE Prev_SetJobPageCount(IPrintPreviewDxgiPackageTarget *self, PageCountType countType, UINT32 count);
static HRESULT STDMETHODCALLTYPE Prev_DrawPage(IPrintPreviewDxgiPackageTarget *self, UINT32 jobPageNumber, IDXGISurface *pageImage, float dpiX, float dpiY);
static HRESULT STDMETHODCALLTYPE Prev_InvalidatePreview(IPrintPreviewDxgiPackageTarget *self);

static PreviewTarget *FromIface(IPrintPreviewDxgiPackageTarget *iface) {
    return (PreviewTarget *)iface;
}

typedef struct PreviewSurfaceTarget {
    PrintRenderTarget base;
    HDC hdc;
    int width;
    int height;
    int dpiX;
    int dpiY;
    HFONT font;
    HFONT oldFont;
    UINT32 requestedPage;
    UINT32 currentPage;
    BOOL renderThisPage;
} PreviewSurfaceTarget;

static BOOL PrevSurfGetMetrics(void *userData, PrintMetrics *metrics) {
    if (!userData || !metrics) return FALSE;
    PreviewSurfaceTarget *t = (PreviewSurfaceTarget *)userData;
    metrics->dpiX = t->dpiX;
    metrics->dpiY = t->dpiY;
    TEXTMETRICW tm;
    if (GetTextMetricsW(t->hdc, &tm)) {
        metrics->lineHeight = tm.tmHeight;
        metrics->averageCharWidth = tm.tmAveCharWidth;
    } else {
        metrics->lineHeight = MulDiv(12, t->dpiY, 72);
        metrics->averageCharWidth = MulDiv(7, t->dpiX, 72);
    }
    return TRUE;
}

static BOOL PrevSurfGetPageSize(void *userData, int *width, int *height) {
    if (!userData || !width || !height) return FALSE;
    PreviewSurfaceTarget *t = (PreviewSurfaceTarget *)userData;
    *width = t->width;
    *height = t->height;
    return TRUE;
}

static BOOL PrevSurfBeginDocument(void *userData, const WCHAR *docName, int totalPages) {
    (void)userData;
    (void)docName;
    (void)totalPages;
    return TRUE;
}

static BOOL PrevSurfEndDocument(void *userData) {
    (void)userData;
    return TRUE;
}

static BOOL PrevSurfAbortDocument(void *userData) {
    (void)userData;
    return TRUE;
}

static BOOL PrevSurfBeginPage(void *userData, int pageNumber) {
    (void)pageNumber;
    PreviewSurfaceTarget *t = (PreviewSurfaceTarget *)userData;
    t->currentPage = (UINT32)pageNumber;
    t->renderThisPage = (t->requestedPage == 0 || t->requestedPage == (UINT32)pageNumber);
    if (!t->renderThisPage) {
        return TRUE;
    }
    RECT rc = {0, 0, t->width, t->height};
    HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRect(t->hdc, &rc, white);
    return TRUE;
}

static BOOL PrevSurfEndPage(void *userData) {
    (void)userData;
    return TRUE;
}

static BOOL PrevSurfDrawText(void *userData, int x, int y, const WCHAR *text, int length) {
    PreviewSurfaceTarget *t = (PreviewSurfaceTarget *)userData;
    if (!t || !text) return FALSE;
    if (!t->renderThisPage) return TRUE;
    return TextOutW(t->hdc, x, y, text, length);
}

static BOOL PrevSurfMeasureText(void *userData, const WCHAR *text, int length, SIZE *size) {
    PreviewSurfaceTarget *t = (PreviewSurfaceTarget *)userData;
    if (!t || !text || !size) return FALSE;
    return GetTextExtentPoint32W(t->hdc, text, length, size);
}

static PrintRenderTargetOps g_previewOps = {
    PrevSurfGetMetrics,
    PrevSurfGetPageSize,
    PrevSurfBeginDocument,
    PrevSurfEndDocument,
    PrevSurfAbortDocument,
    PrevSurfBeginPage,
    PrevSurfEndPage,
    PrevSurfDrawText,
    PrevSurfMeasureText
};

static HRESULT STDMETHODCALLTYPE Prev_QueryInterface(IPrintPreviewDxgiPackageTarget *self, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IPrintPreviewDxgiPackageTarget)) {
        *ppv = self;
        Prev_AddRef(self);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Prev_AddRef(IPrintPreviewDxgiPackageTarget *self) {
    PreviewTarget *t = FromIface(self);
    return (ULONG)InterlockedIncrement(&t->refCount);
}

static ULONG STDMETHODCALLTYPE Prev_Release(IPrintPreviewDxgiPackageTarget *self) {
    PreviewTarget *t = FromIface(self);
    LONG ref = InterlockedDecrement(&t->refCount);
    if (ref == 0) {
        HeapFree(GetProcessHeap(), 0, t);
    }
    return (ULONG)ref;
}

static HRESULT STDMETHODCALLTYPE Prev_SetJobPageCount(IPrintPreviewDxgiPackageTarget *self, PageCountType countType, UINT32 count) {
    (void)self;
    (void)countType;
    (void)count;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Prev_DrawPage(IPrintPreviewDxgiPackageTarget *self, UINT32 jobPageNumber, IDXGISurface *pageImage, float dpiX, float dpiY) {
    PreviewTarget *t = FromIface(self);
    if (!t || !t->ctx || !pageImage) return E_POINTER;
    DebugLog(L"DrawPage start");

    DXGI_SURFACE_DESC desc = {0};
    HRESULT hr = pageImage->lpVtbl->GetDesc(pageImage, &desc);
    if (FAILED(hr)) return hr;

    DXGI_MAPPED_RECT rect;
    hr = pageImage->lpVtbl->Map(pageImage, &rect, DXGI_MAP_WRITE);
    if (FAILED(hr)) return hr;

    IDXGISurface1 *surf1 = NULL;
    HDC hdc = NULL;
    HRESULT hrDc = pageImage->lpVtbl->QueryInterface(pageImage, &IID_IDXGISurface1, (void **)&surf1);
    if (SUCCEEDED(hrDc) && surf1) {
        hrDc = surf1->lpVtbl->GetDC(surf1, FALSE, &hdc);
    }

    // Always start with a white surface so skipped pages stay blank.
    BYTE *row = rect.pBits;
    for (UINT y = 0; y < desc.Height; ++y) {
        ZeroMemory(row, desc.Width * 4);
        row += rect.Pitch;
    }

    if (SUCCEEDED(hrDc) && hdc) {
        PreviewSurfaceTarget pvt = {0};
        pvt.base.ops = &g_previewOps;
        pvt.base.userData = &pvt;
        pvt.hdc = hdc;
        pvt.width = (int)desc.Width;
        pvt.height = (int)desc.Height;
        pvt.dpiX = (int)(dpiX > 0 ? dpiX : t->dpiX);
        pvt.dpiY = (int)(dpiY > 0 ? dpiY : t->dpiY);
        pvt.font = NULL;
        pvt.oldFont = NULL;
        pvt.requestedPage = jobPageNumber;
        pvt.currentPage = 0;
        pvt.renderThisPage = FALSE;

        RenderDocument(t->ctx, &pvt.base);

        if (pvt.oldFont) {
            SelectObject(hdc, pvt.oldFont);
        }
        if (pvt.font) {
            DeleteObject(pvt.font);
        }
        DebugLog(L"DrawPage rendered via DXGI GetDC");
    } else {
        // Fallback: render into a DIB and copy into the mapped surface when GetDC is unavailable.
        DebugLog(L"DrawPage fallback to DIB");
        BITMAPINFO bmi = {0};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = (LONG)desc.Width;
        bmi.bmiHeader.biHeight = -(LONG)desc.Height; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        VOID *dibBits = NULL;
        HDC memDC = CreateCompatibleDC(NULL);
        HBITMAP hbm = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &dibBits, NULL, 0);
        HBITMAP oldBmp = NULL;
        if (memDC && hbm && dibBits) {
            oldBmp = (HBITMAP)SelectObject(memDC, hbm);
            RECT rc = {0, 0, (LONG)desc.Width, (LONG)desc.Height};
            HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
            FillRect(memDC, &rc, white);

            PreviewSurfaceTarget pvt = {0};
            pvt.base.ops = &g_previewOps;
            pvt.base.userData = &pvt;
            pvt.hdc = memDC;
            pvt.width = (int)desc.Width;
            pvt.height = (int)desc.Height;
            pvt.dpiX = (int)(dpiX > 0 ? dpiX : t->dpiX);
            pvt.dpiY = (int)(dpiY > 0 ? dpiY : t->dpiY);
            pvt.font = NULL;
            pvt.oldFont = NULL;
            pvt.requestedPage = jobPageNumber;
            pvt.currentPage = 0;
            pvt.renderThisPage = FALSE;

            RenderDocument(t->ctx, &pvt.base);

            if (pvt.oldFont) SelectObject(memDC, pvt.oldFont);
            if (pvt.font) DeleteObject(pvt.font);

            BYTE *srcRow = (BYTE *)dibBits;
            BYTE *dstRow = rect.pBits;
            UINT rowBytes = desc.Width * 4;
            for (UINT y = 0; y < desc.Height; ++y) {
                CopyMemory(dstRow, srcRow, rowBytes);
                srcRow += rowBytes;
                dstRow += rect.Pitch;
            }
        }
        if (oldBmp) SelectObject(memDC, oldBmp);
        if (hbm) DeleteObject(hbm);
        if (memDC) DeleteDC(memDC);
        hr = S_OK;
    }

    if (surf1 && hdc) {
        surf1->lpVtbl->ReleaseDC(surf1, NULL);
    }
    if (surf1) surf1->lpVtbl->Release(surf1);
    pageImage->lpVtbl->Unmap(pageImage);
    (void)jobPageNumber;
    return hr;
}

static HRESULT STDMETHODCALLTYPE Prev_InvalidatePreview(IPrintPreviewDxgiPackageTarget *self) {
    (void)self;
    return S_OK;
}

static IPrintPreviewDxgiPackageTargetVtbl g_prevVtbl = {
    Prev_QueryInterface,
    Prev_AddRef,
    Prev_Release,
    Prev_SetJobPageCount,
    Prev_DrawPage,
    Prev_InvalidatePreview
};

HRESULT CreatePreviewTarget(PreviewTarget **outTarget) {
    if (!outTarget) return E_POINTER;
    *outTarget = NULL;
    PreviewTarget *t = (PreviewTarget *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*t));
    if (!t) return E_OUTOFMEMORY;
    t->vtbl.lpVtbl = &g_prevVtbl;
    t->refCount = 1;
    t->ctx = NULL;
    t->dpiX = 96.0f;
    t->dpiY = 96.0f;
    t->pageWidth = 816.0f;
    t->pageHeight = 1056.0f;
    *outTarget = t;
    return S_OK;
}

void ReleasePreviewTarget(PreviewTarget *target) {
    if (!target) return;
    target->vtbl.lpVtbl->Release(&target->vtbl);
}

IUnknown *PreviewTargetAsUnknown(PreviewTarget *target) {
    if (!target) return NULL;
    return (IUnknown *)&target->vtbl;
}

HRESULT PreviewTargetSetRenderer(PreviewTarget *target, PrintRenderContext *ctx, FLOAT dpiX, FLOAT dpiY, FLOAT pageWidth, FLOAT pageHeight) {
    if (!target || !ctx) return E_POINTER;
    target->ctx = ctx;
    target->dpiX = dpiX;
    target->dpiY = dpiY;
    target->pageWidth = pageWidth;
    target->pageHeight = pageHeight;
    return S_OK;
}
