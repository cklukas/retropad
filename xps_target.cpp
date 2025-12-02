#include <windows.h>
#include <strsafe.h>
#include <string>

#include "xps_target.h"

static BOOL XpsGetMetrics(void *userData, PrintMetrics *metrics);
static BOOL XpsGetPageSize(void *userData, int *width, int *height);
static BOOL XpsBeginDocument(void *userData, const WCHAR *docName, int totalPages);
static BOOL XpsEndDocument(void *userData);
static BOOL XpsAbortDocument(void *userData);
static BOOL XpsBeginPage(void *userData, int pageNumber);
static BOOL XpsEndPage(void *userData);
static BOOL XpsDrawText(void *userData, int x, int y, const WCHAR *text, int length);
static BOOL XpsMeasureText(void *userData, const WCHAR *text, int length, SIZE *size);

static const PrintRenderTargetOps g_xpsOps = {
    XpsGetMetrics,
    XpsGetPageSize,
    XpsBeginDocument,
    XpsEndDocument,
    XpsAbortDocument,
    XpsBeginPage,
    XpsEndPage,
    XpsDrawText,
    XpsMeasureText,
};

static HRESULT EnsureFontResource(XpsRenderTarget *target);
static HRESULT AppendGlyphs(XpsRenderTarget *target, FLOAT x, FLOAT y, const WCHAR *text, UINT32 length);

HRESULT InitXpsRenderTarget(XpsRenderTarget *target, IXpsOMObjectFactory *factory, IXpsOMPackageWriter *writer, FLOAT width, FLOAT height, FLOAT dpiX, FLOAT dpiY) {
    if (!target) return E_POINTER;
    ZeroMemory(target, sizeof(*target));
    target->base.ops = &g_xpsOps;
    target->base.userData = target;
    target->factory = factory;
    target->writer = writer;
    target->pageSize.width = width;
    target->pageSize.height = height;
    target->dpiX = dpiX;
    target->dpiY = dpiY;
    target->pageNumber = 0;
    target->font = NULL;
    target->fontUri = NULL;
    target->currentPage = NULL;
    return S_OK;
}

void ReleaseXpsRenderTarget(XpsRenderTarget *target) {
    if (!target) return;
    if (target->currentPage) {
        target->currentPage->Release();
        target->currentPage = NULL;
    }
    if (target->font) {
        target->font->Release();
        target->font = NULL;
    }
    if (target->fontUri) {
        target->fontUri->Release();
        target->fontUri = NULL;
    }
    target->factory = NULL;
    target->writer = NULL;
}

static BOOL XpsGetMetrics(void *userData, PrintMetrics *metrics) {
    if (!userData || !metrics) return FALSE;
    XpsRenderTarget *target = (XpsRenderTarget *)userData;
    metrics->dpiX = (int)target->dpiX;
    metrics->dpiY = (int)target->dpiY;
    metrics->lineHeight = MulDiv(12, metrics->dpiY, 72);
    metrics->averageCharWidth = MulDiv(7, metrics->dpiX, 72);
    return TRUE;
}

static BOOL XpsGetPageSize(void *userData, int *width, int *height) {
    if (!userData || !width || !height) return FALSE;
    XpsRenderTarget *target = (XpsRenderTarget *)userData;
    *width = (int)target->pageSize.width;
    *height = (int)target->pageSize.height;
    return TRUE;
}

static BOOL XpsBeginDocument(void *userData, const WCHAR *docName, int totalPages) {
    (void)docName;
    (void)totalPages;
    XpsRenderTarget *target = (XpsRenderTarget *)userData;
    IOpcPartUri *docUri = NULL;
    HRESULT hr = target->factory->CreatePartUri(L"/Documents/1/FixedDocument.fdoc", &docUri);
    if (FAILED(hr)) goto cleanup;
    hr = target->writer->StartNewDocument(docUri, NULL, NULL, NULL, NULL);

cleanup:
    if (docUri) docUri->Release();
    return SUCCEEDED(hr);
}

static BOOL XpsEndDocument(void *userData) {
    XpsRenderTarget *target = (XpsRenderTarget *)userData;
    if (target->currentPage) {
        target->currentPage->Release();
        target->currentPage = NULL;
    }
    return TRUE;
}

static BOOL XpsAbortDocument(void *userData) {
    XpsRenderTarget *target = (XpsRenderTarget *)userData;
    if (target->currentPage) {
        target->currentPage->Release();
        target->currentPage = NULL;
    }
    return TRUE;
}

static BOOL XpsBeginPage(void *userData, int pageNumber) {
    XpsRenderTarget *target = (XpsRenderTarget *)userData;
    if (target->currentPage) {
        target->currentPage->Release();
        target->currentPage = NULL;
    }
    target->pageNumber = (UINT32)pageNumber;

    WCHAR uri[64];
    StringCchPrintfW(uri, ARRAYSIZE(uri), L"/Documents/1/Pages/%u.fpage", target->pageNumber);
    IOpcPartUri *pageUri = NULL;
    IXpsOMPage *page = NULL;
    XPS_SIZE size = target->pageSize;
    HRESULT hr = target->factory->CreatePartUri(uri, &pageUri);
    if (FAILED(hr)) goto cleanup;

    hr = target->factory->CreatePage(&size, L"en-US", pageUri, &page);
    if (FAILED(hr)) goto cleanup;

    target->currentPage = page;
    page = NULL;

cleanup:
    if (pageUri) pageUri->Release();
    if (page) page->Release();
    return SUCCEEDED(hr);
}

static BOOL XpsEndPage(void *userData) {
    XpsRenderTarget *target = (XpsRenderTarget *)userData;
    if (!target->currentPage) return FALSE;

    XPS_SIZE adv = target->pageSize;
    HRESULT hr = target->writer->AddPage(target->currentPage, &adv, NULL, NULL, NULL, NULL);
    target->currentPage->Release();
    target->currentPage = NULL;
    return SUCCEEDED(hr);
}

static BOOL XpsDrawText(void *userData, int x, int y, const WCHAR *text, int length) {
    XpsRenderTarget *target = (XpsRenderTarget *)userData;
    if (!target || !text || length <= 0 || !target->currentPage) return FALSE;
    if (FAILED(EnsureFontResource(target))) return FALSE;
    HRESULT hr = AppendGlyphs(target, (FLOAT)x, (FLOAT)y, text, (UINT32)length);
    return SUCCEEDED(hr);
}

static BOOL XpsMeasureText(void *userData, const WCHAR *text, int length, SIZE *size) {
    if (!userData || !size) return FALSE;
    (void)text;
    XpsRenderTarget *target = (XpsRenderTarget *)userData;
    int avgWidth = MulDiv(7, (int)target->dpiX, 72);
    size->cx = avgWidth * length;
    size->cy = MulDiv(12, (int)target->dpiY, 72);
    return TRUE;
}

static HRESULT EnsureFontResource(XpsRenderTarget *target) {
    if (target->font) return S_OK;
    WCHAR windowsDir[MAX_PATH];
    UINT len = GetWindowsDirectoryW(windowsDir, ARRAYSIZE(windowsDir));
    if (len == 0) return HRESULT_FROM_WIN32(GetLastError());
    WCHAR fontPath[MAX_PATH];
    HRESULT hr = StringCchPrintfW(fontPath, ARRAYSIZE(fontPath), L"%s\\Fonts\\segoeui.ttf", windowsDir);
    if (FAILED(hr)) return hr;

    IStream *fontStream = NULL;
    IOpcPartUri *fontUri = NULL;
    IXpsOMFontResource *font = NULL;

    hr = target->factory->CreateReadOnlyStreamOnFile(fontPath, &fontStream);
    if (FAILED(hr)) goto cleanup;

    hr = target->factory->CreatePartUri(L"/Resources/Fonts/SegoeUI.ttf", &fontUri);
    if (FAILED(hr)) goto cleanup;

    hr = target->factory->CreateFontResource(fontStream, XPS_FONT_EMBEDDING_RESTRICTED, fontUri, FALSE, &font);
    if (FAILED(hr)) goto cleanup;

    hr = target->writer->AddResource(font);
    if (FAILED(hr)) goto cleanup;

    target->font = font;
    target->fontUri = fontUri;
    font = NULL;
    fontUri = NULL;

cleanup:
    if (fontStream) fontStream->Release();
    if (fontUri) fontUri->Release();
    if (font) font->Release();
    return hr;
}

static HRESULT AppendGlyphs(XpsRenderTarget *target, FLOAT x, FLOAT y, const WCHAR *text, UINT32 length) {
    IXpsOMGlyphs *glyphs = NULL;
    IXpsOMGlyphsEditor *editor = NULL;
    IXpsOMSolidColorBrush *brush = NULL;
    IXpsOMVisualCollection *visuals = NULL;
    std::wstring fragment;
    XPS_POINT origin = { x, y };
    XPS_COLOR color = {0};
    color.colorType = XPS_COLOR_TYPE_SRGB;
    color.value.sRGB.alpha = 0xFF;
    color.value.sRGB.red = 0x00;
    color.value.sRGB.green = 0x00;
    color.value.sRGB.blue = 0x00;

    HRESULT hr = target->factory->CreateGlyphs(target->font, &glyphs);
    if (FAILED(hr)) goto cleanup;

    hr = glyphs->SetOrigin(&origin);
    if (FAILED(hr)) goto cleanup;
    hr = glyphs->SetFontRenderingEmSize((FLOAT)MulDiv(12, (INT)target->dpiY, 72));
    if (FAILED(hr)) goto cleanup;

    hr = target->factory->CreateSolidColorBrush(&color, NULL, &brush);
    if (FAILED(hr)) goto cleanup;
    hr = glyphs->SetFillBrushLocal(brush);
    if (FAILED(hr)) goto cleanup;

    hr = glyphs->GetGlyphsEditor(&editor);
    if (FAILED(hr)) goto cleanup;
    fragment.assign(text, text + length);
    hr = editor->SetUnicodeString(fragment.c_str());
    if (SUCCEEDED(hr)) {
        hr = editor->ApplyEdits();
    }
    editor->Release();
    editor = NULL;
    if (FAILED(hr)) goto cleanup;

    hr = target->currentPage->GetVisuals(&visuals);
    if (FAILED(hr)) goto cleanup;
    hr = visuals->Append(glyphs);

cleanup:
    if (visuals) visuals->Release();
    if (brush) brush->Release();
    if (editor) editor->Release();
    if (glyphs) glyphs->Release();
    return hr;
}
