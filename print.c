#define WINVER          0x0601
#define _WIN32_WINNT    0x0601
#define _WIN32_WINDOWS  0x0601
#define _WIN32_IE       0x0700
#define NTDDI_VERSION   0x06010000

#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <strsafe.h>

#include "retropad.h"
#include "print.h"
#include "rendering.h"
#include "xps_backend.h"
#include "xps_target.h"
#include "resource.h"
#include "print_dialog_callback.h"
#include <PrintPreview.h>
#include <strsafe.h>

#ifndef __IPrintDialogCallback2_INTERFACE_DEFINED__
typedef struct IPrintDialogCallback2 { const void *lpVtbl; } IPrintDialogCallback2;
#endif
#ifndef LPPAGERANGE
typedef struct tagPAGERANGE { DWORD nFromPage; DWORD nToPage; } PAGERANGE, *LPPAGERANGE;
#endif
#ifndef IID_IPrintPreviewDxgiPackageTarget
EXTERN_C const IID IID_IPrintPreviewDxgiPackageTarget;
#endif
#ifndef __IPrintPreviewDxgiPackageTarget_INTERFACE_DEFINED__
typedef struct IPrintPreviewDxgiPackageTarget { const void *lpVtbl; } IPrintPreviewDxgiPackageTarget;
#endif

/* Compatibility: extended PRINTDLGEXW including dwCallbackSize when headers omit it. */
typedef struct PRINTDLGEXW_EX {
    DWORD        lStructSize;
    HWND         hwndOwner;
    HGLOBAL      hDevMode;
    HGLOBAL      hDevNames;
    HDC          hDC;
    DWORD        Flags;
    DWORD        Flags2;
    DWORD        ExclusionFlags;
    DWORD        nPageRanges;
    DWORD        nMaxPageRanges;
    LPPAGERANGE  lpPageRanges;
    DWORD        nMinPage;
    DWORD        nMaxPage;
    DWORD        nCopies;
    HINSTANCE    hInstance;
    LPCWSTR      lpPrintTemplateName;
    LPUNKNOWN    lpCallback;
    DWORD        nPropertyPages;
    HPROPSHEETPAGE *lphPropertyPages;
    DWORD        nStartPage;
    DWORD        dwResultAction;
    DWORD        dwCallbackSize;
} PRINTDLGEXW_EX;

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

#ifndef PD_ENABLEPRINTPREVIEW
#define PD_ENABLEPRINTPREVIEW 0x00200000
#endif

// Print context used by the modern print dialog preview callback.
PrintRenderContext g_printContext = {0};
static IPrintDialogCallback *g_printCallback = NULL;
static BOOL TryParseMargin(HWND dlg, int ctrlId, int *outThousandths);
static void SetMarginText(HWND dlg, int ctrlId, int thousandths);
static INT_PTR CALLBACK PageSetupDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam);
static HFONT CreatePrintFont(HDC hdc);
static void SplitHeaderFooterSegments(const WCHAR *format, const WCHAR *fileName, int pageNumber, int totalPages, const WCHAR *dateStr, const WCHAR *timeStr, WCHAR *left, size_t cchLeft, WCHAR *center, size_t cchCenter, WCHAR *right, size_t cchRight);
static int ComputeTotalPages(const WCHAR *text, int charsPerLine, int linesPerPage);
static BOOL PrintBuffer(HDC hdc, const WCHAR *text);
static BOOL GetPrinterNameFromDevNames(HGLOBAL hDevNames, WCHAR *out, size_t cchOut);
static BOOL IsValidGlobalHandle(HGLOBAL h);
static void GetDefaultPrintHandles(HGLOBAL *outDevMode, HGLOBAL *outDevNames);
static HGLOBAL DuplicateGlobalHandle(HGLOBAL h);
static void EnsureComctl();

void DoPageSetup(HWND hwnd) {
    DialogBoxW(g_hInst, MAKEINTRESOURCE(IDD_PAGE_SETUP), hwnd, PageSetupDlgProc);
}

void DoPrint(HWND hwnd) {
    DebugLog(L"DoPrint invoked");
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    BOOL coInitialized = SUCCEEDED(hrCo);
    if (hrCo == RPC_E_CHANGED_MODE) {
        coInitialized = FALSE;
    }

    XpsBackend xpsBackend = {0};
    BOOL xpsReady = SUCCEEDED(XpsBackendInitialize(&xpsBackend));
    EnsureComctl();

    int len = GetWindowTextLengthW(g_app.hwndEdit);
    WCHAR *buffer = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
    if (!buffer) {
        if (xpsReady) XpsBackendShutdown(&xpsBackend);
        if (coInitialized) CoUninitialize();
        return;
    }
    GetWindowTextW(g_app.hwndEdit, buffer, len + 1);

    PrintRenderContext ctx = {
        .text = buffer,
        .fullPath = g_app.currentPath[0] ? g_app.currentPath : UNTITLED_NAME,
        .marginsThousandths = g_app.marginsThousandths,
        .headerText = g_app.headerText,
        .footerText = g_app.footerText,
    };

    // Make the context available to the preview callback once the dialog provides a site.
    g_printContext = ctx;

    if (!g_printCallback) {
        if (SUCCEEDED(CreatePrintDialogCallback(&g_printCallback))) {
            DebugLog(L"Callback created and stored globally");
        } else {
            DebugLog(L"CreatePrintDialogCallback failed; aborting print");
            if (xpsReady) XpsBackendShutdown(&xpsBackend);
            if (coInitialized) CoUninitialize();
            HeapFree(GetProcessHeap(), 0, buffer);
            return;
        }
    }

    PRINTDLGEXW_EX pdx = {0};
    pdx.lStructSize    = sizeof(PRINTDLGEXW_EX);
    pdx.dwCallbackSize = sizeof(PRINTDLGEXW_EX); // non-zero enables callback usage
    pdx.hwndOwner      = hwnd;
    pdx.hDevMode       = g_app.hDevMode ? DuplicateGlobalHandle(g_app.hDevMode) : NULL;
    pdx.hDevNames      = g_app.hDevNames ? DuplicateGlobalHandle(g_app.hDevNames) : NULL;
    // Only the valid modern-preview flag combo (0x00A80000)
    pdx.Flags          = PD_ENABLEPRINTPREVIEW |
                         PD_USEDEVMODECOPIESANDCOLLATE |
                         PD_NOCURRENTPAGE;
    pdx.nCopies        = 1;
    pdx.nMinPage       = 1;
    pdx.nMaxPage       = 0xFFFF;
    #pragma warning(push)
    #pragma warning(disable:4133) // LPUNKNOWN expects exact pointer; keep identity, silence type warning
    pdx.lpCallback     = (LPUNKNOWN)g_printCallback;
    #pragma warning(pop)

    WCHAR dbg[256];
    StringCchPrintfW(dbg, ARRAYSIZE(dbg),
        L"Calling PrintDlgExW: lStructSize=%lu dwCallbackSize=%lu Flags=0x%08lx lpCallback=%p",
        pdx.lStructSize, pdx.dwCallbackSize, pdx.Flags, pdx.lpCallback);
    DebugLog(dbg);

    DebugLog(L"DoPrint: calling PrintDlgExW with PD_ENABLEPRINTPREVIEW");
    HRESULT hr = PrintDlgExW((PRINTDLGEXW*)&pdx);

    if (FAILED(hr) || pdx.dwResultAction != PD_RESULT_PRINT) {
        StringCchPrintfW(dbg, ARRAYSIZE(dbg),
            L"PrintDlgExW cancelled or failed hr=0x%08lX result=%u", hr, pdx.dwResultAction);
        DebugLog(dbg);
        if (pdx.hDC) DeleteDC(pdx.hDC);
        if (pdx.hDevMode && pdx.hDevMode != g_app.hDevMode) GlobalFree(pdx.hDevMode);
        if (pdx.hDevNames && pdx.hDevNames != g_app.hDevNames) GlobalFree(pdx.hDevNames);
        HeapFree(GetProcessHeap(), 0, buffer);
        if (xpsReady) XpsBackendShutdown(&xpsBackend);
        if (coInitialized) CoUninitialize();
        return;
    }

    if (pdx.dwResultAction == PD_RESULT_CANCEL) {
        if (pdx.hDC) DeleteDC(pdx.hDC);
        if (pdx.hDevMode && pdx.hDevMode != g_app.hDevMode) GlobalFree(pdx.hDevMode);
        if (pdx.hDevNames && pdx.hDevNames != g_app.hDevNames) GlobalFree(pdx.hDevNames);
        HeapFree(GetProcessHeap(), 0, buffer);
        if (xpsReady) XpsBackendShutdown(&xpsBackend);
        if (coInitialized) CoUninitialize();
        return;
    }

    if (pdx.dwResultAction == PD_RESULT_APPLY) {
        if (g_app.hDevMode && g_app.hDevMode != pdx.hDevMode) GlobalFree(g_app.hDevMode);
        if (g_app.hDevNames && g_app.hDevNames != pdx.hDevNames) GlobalFree(g_app.hDevNames);
        g_app.hDevMode = pdx.hDevMode;
        g_app.hDevNames = pdx.hDevNames;
        if (pdx.hDC) DeleteDC(pdx.hDC);
        HeapFree(GetProcessHeap(), 0, buffer);
        if (xpsReady) XpsBackendShutdown(&xpsBackend);
        if (coInitialized) CoUninitialize();
        return;
    }

    if (pdx.dwResultAction != PD_RESULT_PRINT) {
        if (pdx.hDC) DeleteDC(pdx.hDC);
        HeapFree(GetProcessHeap(), 0, buffer);
        if (xpsReady) XpsBackendShutdown(&xpsBackend);
        if (coInitialized) CoUninitialize();
        return;
    }

    if (g_app.hDevMode && g_app.hDevMode != pdx.hDevMode) GlobalFree(g_app.hDevMode);
    if (g_app.hDevNames && g_app.hDevNames != pdx.hDevNames) GlobalFree(g_app.hDevNames);
    g_app.hDevMode = pdx.hDevMode;
    g_app.hDevNames = pdx.hDevNames;
    
    BOOL printed = FALSE;
    FLOAT pageWidth = 816.0f;   // defaults: 8.5" * 96
    FLOAT pageHeight = 1056.0f; // 11" * 96
    FLOAT dpiX = 96.0f;
    FLOAT dpiY = 96.0f;

    if (pdx.hDC) {
        int capDpiX = GetDeviceCaps(pdx.hDC, LOGPIXELSX);
        int capDpiY = GetDeviceCaps(pdx.hDC, LOGPIXELSY);
        if (capDpiX > 0) dpiX = (FLOAT)capDpiX;
        if (capDpiY > 0) dpiY = (FLOAT)capDpiY;

        int physWidth = GetDeviceCaps(pdx.hDC, PHYSICALWIDTH);
        int physHeight = GetDeviceCaps(pdx.hDC, PHYSICALHEIGHT);
        if (physWidth > 0 && physHeight > 0 && capDpiX > 0 && capDpiY > 0) {
            pageWidth = (FLOAT)physWidth * (96.0f / (FLOAT)capDpiX);
            pageHeight = (FLOAT)physHeight * (96.0f / (FLOAT)capDpiY);
        }
    }

    if (xpsReady) {
        WCHAR printerName[MAX_PATH] = {0};
        BOOL havePrinterName = GetPrinterNameFromDevNames(g_app.hDevNames, printerName, ARRAYSIZE(printerName));
        if (havePrinterName) {
            IPrintDocumentPackageTarget *pkgTarget = NULL;
            if (SUCCEEDED(XpsBackendCreateTarget(&xpsBackend, printerName, &pkgTarget))) {
                DebugLog(L"Created IPrintDocumentPackageTarget");
                // Try to hand the renderer to the preview channel if available.
                // Probe for OS-provided preview target and log result (no-op otherwise).
                IUnknown *previewPkg = NULL;
                GUID previewGuid;
                if (CLSIDFromString(L"{1A6DD0AD-1E2A-4E99-A5BA-91F17818290E}", &previewGuid) == NOERROR) {
                    HRESULT hrPrev = pkgTarget->lpVtbl->GetPackageTarget(pkgTarget, &previewGuid, &IID_IPrintPreviewDxgiPackageTarget, (void **)&previewPkg);
                    if (SUCCEEDED(hrPrev) && previewPkg) {
                        DebugLog(L"Got preview package target from package target factory");
                        previewPkg->lpVtbl->Release(previewPkg);
                    } else {
                        DebugLog(L"GetPackageTarget for preview returned no target");
                    }
                }

                IXpsOMPackageWriter *writer = NULL;
                if (SUCCEEDED(XpsBackendCreateWriterForTarget(&xpsBackend, pkgTarget, &writer))) {
                    if (g_app.hDevMode) {
                        DEVMODEW *dm = (DEVMODEW *)GlobalLock(g_app.hDevMode);
                        if (dm) {
                            if (dm->dmFields & DM_PAPERWIDTH) {
                                double inches = (dm->dmPaperWidth * 0.1) / 25.4;
                                pageWidth = (FLOAT)(inches * 96.0);
                            }
                            if (dm->dmFields & DM_PAPERLENGTH) {
                                double inches = (dm->dmPaperLength * 0.1) / 25.4;
                                pageHeight = (FLOAT)(inches * 96.0);
                            }
                            if ((dm->dmFields & DM_ORIENTATION) && dm->dmOrientation == DMORIENT_LANDSCAPE) {
                                FLOAT tmp = pageWidth;
                                pageWidth = pageHeight;
                                pageHeight = tmp;
                            }
                            GlobalUnlock(g_app.hDevMode);
                        }
                    }

                    XpsRenderTarget xpsTarget;
                    if (SUCCEEDED(InitXpsRenderTarget(&xpsTarget, xpsBackend.factory, writer, pageWidth, pageHeight, dpiX, dpiY))) {
                        if (RenderDocument(&ctx, &xpsTarget.base)) {
                            printed = TRUE;
                        }
                        ReleaseXpsRenderTarget(&xpsTarget);
                    }
                    XpsPackageWriterClose(writer);
                    XpsPackageWriterRelease(writer);
                }
                pkgTarget->lpVtbl->Release(pkgTarget);
            }
        }
    }

    if (!printed) {
        MessageBoxW(hwnd, L"Printing failed.", APP_TITLE, MB_ICONERROR);
    }

    if (pdx.hDC) DeleteDC(pdx.hDC);

    HeapFree(GetProcessHeap(), 0, buffer);
    if (xpsReady) XpsBackendShutdown(&xpsBackend);
    if (coInitialized) CoUninitialize();
    // Optional: release the global callback here if you don't want to reuse it.
    // if (g_printCallback) { g_printCallback->lpVtbl->Release(g_printCallback); g_printCallback = NULL; }
}

static BOOL TryParseMargin(HWND dlg, int ctrlId, int *outThousandths) {
    WCHAR buf[32];
    GetDlgItemTextW(dlg, ctrlId, buf, ARRAYSIZE(buf));
    if (buf[0] == L'\0') return FALSE;
    WCHAR *end = NULL;
    double inches = wcstod(buf, &end);
    if (end == buf || inches < 0.0) return FALSE;
    int thousandths = (int)(inches * 1000.0 + 0.5);
    if (thousandths < 0) thousandths = 0;
    *outThousandths = thousandths;
    return TRUE;
}

static void SetMarginText(HWND dlg, int ctrlId, int thousandths) {
    double inches = thousandths / 1000.0;
    WCHAR buf[32];
    StringCchPrintfW(buf, ARRAYSIZE(buf), L"%.2f", inches);
    SetDlgItemTextW(dlg, ctrlId, buf);
}

static INT_PTR CALLBACK PageSetupDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG: {
        SetDlgItemTextW(dlg, IDC_PAGE_HEADER, g_app.headerText);
        SetDlgItemTextW(dlg, IDC_PAGE_FOOTER, g_app.footerText);
        SetMarginText(dlg, IDC_MARGIN_LEFT, g_app.marginsThousandths.left ? g_app.marginsThousandths.left : 750);
        SetMarginText(dlg, IDC_MARGIN_RIGHT, g_app.marginsThousandths.right ? g_app.marginsThousandths.right : 750);
        SetMarginText(dlg, IDC_MARGIN_TOP, g_app.marginsThousandths.top ? g_app.marginsThousandths.top : 1000);
        SetMarginText(dlg, IDC_MARGIN_BOTTOM, g_app.marginsThousandths.bottom ? g_app.marginsThousandths.bottom : 1000);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK: {
            int left = 0, right = 0, top = 0, bottom = 0;
            if (!TryParseMargin(dlg, IDC_MARGIN_LEFT, &left) ||
                !TryParseMargin(dlg, IDC_MARGIN_RIGHT, &right) ||
                !TryParseMargin(dlg, IDC_MARGIN_TOP, &top) ||
                !TryParseMargin(dlg, IDC_MARGIN_BOTTOM, &bottom)) {
                MessageBoxW(dlg, L"Enter valid margins (e.g. 0.75).", APP_TITLE, MB_ICONWARNING);
                return TRUE;
            }

            WCHAR header[ARRAYSIZE(g_app.headerText)];
            WCHAR footer[ARRAYSIZE(g_app.footerText)];
            GetDlgItemTextW(dlg, IDC_PAGE_HEADER, header, ARRAYSIZE(header));
            GetDlgItemTextW(dlg, IDC_PAGE_FOOTER, footer, ARRAYSIZE(footer));
            StringCchCopyW(g_app.headerText, ARRAYSIZE(g_app.headerText), header);
            StringCchCopyW(g_app.footerText, ARRAYSIZE(g_app.footerText), footer);

            g_app.marginsThousandths.left = left;
            g_app.marginsThousandths.right = right;
            g_app.marginsThousandths.top = top;
            g_app.marginsThousandths.bottom = bottom;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static HFONT CreatePrintFont(HDC hdc) {
    LOGFONTW lf = {0};
    if (g_app.hFont) {
        GetObjectW(g_app.hFont, sizeof(lf), &lf);
    } else {
        SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(lf), &lf, 0);
    }

    HDC hScreen = GetDC(NULL);
    int screenDpiX = GetDeviceCaps(hScreen, LOGPIXELSX);
    int screenDpiY = GetDeviceCaps(hScreen, LOGPIXELSY);
    ReleaseDC(NULL, hScreen);

    int printerDpiX = GetDeviceCaps(hdc, LOGPIXELSX);
    int printerDpiY = GetDeviceCaps(hdc, LOGPIXELSY);

    lf.lfHeight = MulDiv(lf.lfHeight, printerDpiY, screenDpiY);
    lf.lfWidth = MulDiv(lf.lfWidth, printerDpiX, screenDpiX);

    return CreateFontIndirectW(&lf);
}

static void SplitHeaderFooterSegments(const WCHAR *format, const WCHAR *fileName, int pageNumber, int totalPages, const WCHAR *dateStr, const WCHAR *timeStr, WCHAR *left, size_t cchLeft, WCHAR *center, size_t cchCenter, WCHAR *right, size_t cchRight) {
    if (left && cchLeft) left[0] = L'\0';
    if (center && cchCenter) center[0] = L'\0';
    if (right && cchRight) right[0] = L'\0';
    if (!format) return;

    enum { SEG_LEFT = 0, SEG_CENTER = 1, SEG_RIGHT = 2 } seg = SEG_LEFT;
    WCHAR *targets[3] = { left, center, right };
    size_t sizes[3] = { cchLeft, cchCenter, cchRight };

    for (const WCHAR *p = format; *p; ++p) {
        if (*p == L'&') {
            WCHAR next = *(p + 1);
            if (next == L'l' || next == L'L') { seg = SEG_LEFT; p++; continue; }
            if (next == L'c' || next == L'C') { seg = SEG_CENTER; p++; continue; }
            if (next == L'r' || next == L'R') { seg = SEG_RIGHT; p++; continue; }

            const WCHAR *insert = NULL;
            WCHAR tmp[2] = {0};
            WCHAR pageBuf[16], totalBuf[16];
            switch (next) {
            case L'&': tmp[0] = L'&'; insert = tmp; p++; break;
            case L'f': case L'F': insert = fileName; p++; break;
            case L'p': StringCchPrintfW(pageBuf, ARRAYSIZE(pageBuf), L"%d", pageNumber); insert = pageBuf; p++; break;
            case L'P': StringCchPrintfW(totalBuf, ARRAYSIZE(totalBuf), L"%d", max(1, totalPages)); insert = totalBuf; p++; break;
            case L'd': case L'D': insert = dateStr; p++; break;
            case L't': case L'T': insert = timeStr; p++; break;
            default:
                tmp[0] = L'&';
                insert = tmp;
                break;
            }

            if (insert && insert[0] && targets[seg] && sizes[seg] > 0) {
                size_t curLen = wcslen(targets[seg]);
                size_t remaining = sizes[seg] - curLen - 1;
                if (remaining > 0) {
                    size_t len = wcslen(insert);
                    size_t toCopy = min(len, remaining);
                    CopyMemory(targets[seg] + curLen, insert, toCopy * sizeof(WCHAR));
                    targets[seg][curLen + toCopy] = L'\0';
                }
            }
        } else {
            if (targets[seg] && sizes[seg] > 0) {
                size_t curLen = wcslen(targets[seg]);
                if (curLen + 1 < sizes[seg]) {
                    targets[seg][curLen] = *p;
                    targets[seg][curLen + 1] = L'\0';
                }
            }
        }
    }
}

static int ComputeTotalPages(const WCHAR *text, int charsPerLine, int linesPerPage) {
    if (charsPerLine < 1) charsPerLine = 1;
    if (linesPerPage < 1) linesPerPage = 1;

    const int tabWidth = 8;
    int lineOnPage = 0;
    int pageNumber = 1;
    const WCHAR *p = text;
    while (*p) {
        int col = 0;
        while (*p && *p != L'\r' && *p != L'\n') {
            if (*p == L'\t') {
                int spaces = tabWidth - (col % tabWidth);
                col += spaces;
            } else {
                col++;
            }
            if (col >= charsPerLine) {
                lineOnPage++;
                col = 0;
                if (lineOnPage >= linesPerPage) {
                    const WCHAR *peek = p;
                    while (*peek == L'\r' || *peek == L'\n') peek++;
                    if (*peek) {
                        pageNumber++;
                        lineOnPage = 0;
                    }
                }
            }
            p++;
        }

        lineOnPage++;
        if (lineOnPage >= linesPerPage) {
            const WCHAR *peek = p;
            while (*peek == L'\r' || *peek == L'\n') peek++;
            if (*peek) {
                pageNumber++;
                lineOnPage = 0;
            }
        }

        if (*p == L'\r') {
            p++;
            if (*p == L'\n') p++;
        } else if (*p == L'\n') {
            p++;
        }
    }

    if (!text || text[0] == L'\0') {
        pageNumber = 1;
    }
    return pageNumber;
}

static BOOL PrintBuffer(HDC hdc, const WCHAR *text) {
    TEXTMETRICW tm = {0};
    GetTextMetricsW(hdc, &tm);
    int lineHeight = tm.tmHeight + tm.tmExternalLeading;
    BOOL headerEnabled = (g_app.headerText[0] != L'\0');
    BOOL footerEnabled = (g_app.footerText[0] != L'\0');
    int headerHeight = headerEnabled ? lineHeight : 0;
    int footerHeight = footerEnabled ? lineHeight : 0;

    RECT m = g_app.marginsThousandths;
    if (m.left == 0 && m.right == 0 && m.top == 0 && m.bottom == 0) {
        m.left = m.right = 750;
        m.top = m.bottom = 1000;
    }

    int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
    int dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
    int marginLeft = MulDiv(m.left, dpiX, 1000);
    int marginRight = MulDiv(m.right, dpiX, 1000);
    int marginTop = MulDiv(m.top, dpiY, 1000);
    int marginBottom = MulDiv(m.bottom, dpiY, 1000);

    int pageWidth = GetDeviceCaps(hdc, HORZRES);
    int pageHeight = GetDeviceCaps(hdc, VERTRES);
    int printableWidth = pageWidth - marginLeft - marginRight;
    int printableHeight = pageHeight - marginTop - marginBottom - headerHeight - footerHeight;
    if (printableWidth < tm.tmAveCharWidth) printableWidth = tm.tmAveCharWidth;
    if (printableHeight < lineHeight) printableHeight = lineHeight;

    int charsPerLine = max(1, printableWidth / max(1, tm.tmAveCharWidth));
    int linesPerPage = max(1, printableHeight / lineHeight);
    int contentTop = marginTop + headerHeight;

    WCHAR dateStr[64] = {0};
    WCHAR timeStr[64] = {0};
    SYSTEMTIME st;
    GetLocalTime(&st);
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, dateStr, ARRAYSIZE(dateStr));
    GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, timeStr, ARRAYSIZE(timeStr));

    int totalPages = ComputeTotalPages(text, charsPerLine, linesPerPage);

    DOCINFOW di = {0};
    di.cbSize = sizeof(di);
    di.lpszDocName = g_app.currentPath[0] ? g_app.currentPath : UNTITLED_NAME;

    if (StartDocW(hdc, &di) <= 0) {
        return FALSE;
    }
    if (StartPage(hdc) <= 0) {
        AbortDoc(hdc);
        return FALSE;
    }

    const int tabWidth = 8;
    int lineOnPage = 0;
    int y = contentTop;
    int pageNumber = 1;
    WCHAR lineBuf[4096];
    WCHAR headerLeft[128], headerCenter[128], headerRight[128];
    WCHAR footerLeft[128], footerCenter[128], footerRight[128];
    const WCHAR *fullPath = g_app.currentPath[0] ? g_app.currentPath : UNTITLED_NAME;
    const WCHAR *fileName = fullPath;
    const WCHAR *slash = wcsrchr(fullPath, L'\\');
    if (slash && *(slash + 1)) {
        fileName = slash + 1;
    }
    SplitHeaderFooterSegments(g_app.headerText, fileName, pageNumber, totalPages, dateStr, timeStr, headerLeft, ARRAYSIZE(headerLeft), headerCenter, ARRAYSIZE(headerCenter), headerRight, ARRAYSIZE(headerRight));
    SplitHeaderFooterSegments(g_app.footerText, fileName, pageNumber, totalPages, dateStr, timeStr, footerLeft, ARRAYSIZE(footerLeft), footerCenter, ARRAYSIZE(footerCenter), footerRight, ARRAYSIZE(footerRight));
    if (headerEnabled) {
        SIZE sz;
        if (headerLeft[0]) {
            TextOutW(hdc, marginLeft, marginTop, headerLeft, lstrlenW(headerLeft));
        }
        if (headerCenter[0]) {
            GetTextExtentPoint32W(hdc, headerCenter, lstrlenW(headerCenter), &sz);
            int cx = marginLeft + (printableWidth - sz.cx) / 2;
            TextOutW(hdc, cx, marginTop, headerCenter, lstrlenW(headerCenter));
        }
        if (headerRight[0]) {
            GetTextExtentPoint32W(hdc, headerRight, lstrlenW(headerRight), &sz);
            int rx = pageWidth - marginRight - sz.cx;
            TextOutW(hdc, rx, marginTop, headerRight, lstrlenW(headerRight));
        }
    }

    const WCHAR *p = text;
    while (*p) {
        int bufLen = 0;
        int col = 0;

        while (*p && *p != L'\r' && *p != L'\n') {
            WCHAR ch = *p++;
            if (ch == L'\t') {
                int spaces = tabWidth - (col % tabWidth);
                for (int i = 0; i < spaces; ++i) {
                    if (bufLen < (int)ARRAYSIZE(lineBuf) - 1) {
                        lineBuf[bufLen++] = L' ';
                    }
                    col++;
                    if (col >= charsPerLine) break;
                }
            } else {
                if (bufLen < (int)ARRAYSIZE(lineBuf) - 1) {
                    lineBuf[bufLen++] = ch;
                }
                col++;
            }

            if (col >= charsPerLine) {
                TextOutW(hdc, marginLeft, y, lineBuf, bufLen);
                y += lineHeight;
                lineOnPage++;
                bufLen = 0;
                col = 0;
                if (lineOnPage >= linesPerPage) {
                    const WCHAR *peek = p;
                    while (*peek == L'\r' || *peek == L'\n') peek++;
                    if (*peek) {
                        if (footerEnabled && (footerLeft[0] || footerCenter[0] || footerRight[0])) {
                            SIZE ft;
                            if (footerLeft[0]) {
                                TextOutW(hdc, marginLeft, pageHeight - marginBottom - footerHeight, footerLeft, lstrlenW(footerLeft));
                            }
                            if (footerCenter[0]) {
                                GetTextExtentPoint32W(hdc, footerCenter, lstrlenW(footerCenter), &ft);
                                int cx = marginLeft + (printableWidth - ft.cx) / 2;
                                TextOutW(hdc, cx, pageHeight - marginBottom - footerHeight, footerCenter, lstrlenW(footerCenter));
                            }
                            if (footerRight[0]) {
                                GetTextExtentPoint32W(hdc, footerRight, lstrlenW(footerRight), &ft);
                                int rx = pageWidth - marginRight - ft.cx;
                                TextOutW(hdc, rx, pageHeight - marginBottom - footerHeight, footerRight, lstrlenW(footerRight));
                            }
                        }

                        if (EndPage(hdc) <= 0) {
                            AbortDoc(hdc);
                            return FALSE;
                        }
                        pageNumber++;
                        if (StartPage(hdc) <= 0) {
                            AbortDoc(hdc);
                            return FALSE;
                        }
                        SplitHeaderFooterSegments(g_app.headerText, fileName, pageNumber, totalPages, dateStr, timeStr, headerLeft, ARRAYSIZE(headerLeft), headerCenter, ARRAYSIZE(headerCenter), headerRight, ARRAYSIZE(headerRight));
                        SplitHeaderFooterSegments(g_app.footerText, fileName, pageNumber, totalPages, dateStr, timeStr, footerLeft, ARRAYSIZE(footerLeft), footerCenter, ARRAYSIZE(footerCenter), footerRight, ARRAYSIZE(footerRight));
                        if (headerEnabled) {
                            SIZE ht;
                            if (headerLeft[0]) {
                                TextOutW(hdc, marginLeft, marginTop, headerLeft, lstrlenW(headerLeft));
                            }
                            if (headerCenter[0]) {
                                GetTextExtentPoint32W(hdc, headerCenter, lstrlenW(headerCenter), &ht);
                                int hx = marginLeft + (printableWidth - ht.cx) / 2;
                                TextOutW(hdc, hx, marginTop, headerCenter, lstrlenW(headerCenter));
                            }
                            if (headerRight[0]) {
                                GetTextExtentPoint32W(hdc, headerRight, lstrlenW(headerRight), &ht);
                                int rx = pageWidth - marginRight - ht.cx;
                                TextOutW(hdc, rx, marginTop, headerRight, lstrlenW(headerRight));
                            }
                        }
                        lineOnPage = 0;
                        y = contentTop;
                    }
                }
            }
        }

        TextOutW(hdc, marginLeft, y, lineBuf, bufLen);
        y += lineHeight;
        lineOnPage++;
        if (lineOnPage >= linesPerPage) {
            const WCHAR *peek = p;
            while (*peek == L'\r' || *peek == L'\n') peek++;
            if (*peek) {
                if (footerEnabled && (footerLeft[0] || footerCenter[0] || footerRight[0])) {
                    SIZE ft;
                    if (footerLeft[0]) {
                        TextOutW(hdc, marginLeft, pageHeight - marginBottom - footerHeight, footerLeft, lstrlenW(footerLeft));
                    }
                    if (footerCenter[0]) {
                        GetTextExtentPoint32W(hdc, footerCenter, lstrlenW(footerCenter), &ft);
                        int cx = marginLeft + (printableWidth - ft.cx) / 2;
                        TextOutW(hdc, cx, pageHeight - marginBottom - footerHeight, footerCenter, lstrlenW(footerCenter));
                    }
                    if (footerRight[0]) {
                        GetTextExtentPoint32W(hdc, footerRight, lstrlenW(footerRight), &ft);
                        int rx = pageWidth - marginRight - ft.cx;
                        TextOutW(hdc, rx, pageHeight - marginBottom - footerHeight, footerRight, lstrlenW(footerRight));
                    }
                }

                if (EndPage(hdc) <= 0) {
                    AbortDoc(hdc);
                    return FALSE;
                }
                pageNumber++;
                if (StartPage(hdc) <= 0) {
                    AbortDoc(hdc);
                    return FALSE;
                }
                SplitHeaderFooterSegments(g_app.headerText, fileName, pageNumber, totalPages, dateStr, timeStr, headerLeft, ARRAYSIZE(headerLeft), headerCenter, ARRAYSIZE(headerCenter), headerRight, ARRAYSIZE(headerRight));
                SplitHeaderFooterSegments(g_app.footerText, fileName, pageNumber, totalPages, dateStr, timeStr, footerLeft, ARRAYSIZE(footerLeft), footerCenter, ARRAYSIZE(footerCenter), footerRight, ARRAYSIZE(footerRight));
                if (headerEnabled) {
                    SIZE ht;
                    if (headerLeft[0]) {
                        TextOutW(hdc, marginLeft, marginTop, headerLeft, lstrlenW(headerLeft));
                    }
                    if (headerCenter[0]) {
                        GetTextExtentPoint32W(hdc, headerCenter, lstrlenW(headerCenter), &ht);
                        int hx = marginLeft + (printableWidth - ht.cx) / 2;
                        TextOutW(hdc, hx, marginTop, headerCenter, lstrlenW(headerCenter));
                    }
                    if (headerRight[0]) {
                        GetTextExtentPoint32W(hdc, headerRight, lstrlenW(headerRight), &ht);
                        int rx = pageWidth - marginRight - ht.cx;
                        TextOutW(hdc, rx, marginTop, headerRight, lstrlenW(headerRight));
                    }
                }
                lineOnPage = 0;
                y = contentTop;
            }
        }

        if (*p == L'\r') {
            p++;
            if (*p == L'\n') p++;
        } else if (*p == L'\n') {
            p++;
        }
    }

    if (footerEnabled && (footerLeft[0] || footerCenter[0] || footerRight[0])) {
        SIZE ft;
        if (footerLeft[0]) {
            TextOutW(hdc, marginLeft, pageHeight - marginBottom - footerHeight, footerLeft, lstrlenW(footerLeft));
        }
        if (footerCenter[0]) {
            GetTextExtentPoint32W(hdc, footerCenter, lstrlenW(footerCenter), &ft);
            int cx = marginLeft + (printableWidth - ft.cx) / 2;
            TextOutW(hdc, cx, pageHeight - marginBottom - footerHeight, footerCenter, lstrlenW(footerCenter));
        }
        if (footerRight[0]) {
            GetTextExtentPoint32W(hdc, footerRight, lstrlenW(footerRight), &ft);
            int rx = pageWidth - marginRight - ft.cx;
            TextOutW(hdc, rx, pageHeight - marginBottom - footerHeight, footerRight, lstrlenW(footerRight));
        }
    }

    if (EndPage(hdc) <= 0) {
        AbortDoc(hdc);
        return FALSE;
    }
    if (EndDoc(hdc) <= 0) {
        AbortDoc(hdc);
        return FALSE;
    }
    return TRUE;
}
static BOOL GetPrinterNameFromDevNames(HGLOBAL hDevNames, WCHAR *out, size_t cchOut) {
    DEVNAMES *dn = (DEVNAMES *)GlobalLock(hDevNames);
    if (!dn) return FALSE;
    const WCHAR *name = (const WCHAR *)((BYTE *)dn + dn->wDeviceOffset);
    HRESULT hr = StringCchCopyW(out, cchOut, name);
    GlobalUnlock(hDevNames);
    return SUCCEEDED(hr);
}

static BOOL IsValidGlobalHandle(HGLOBAL h) {
    if (!h) return FALSE;
    void *p = GlobalLock(h);
    if (!p) return FALSE;
    GlobalUnlock(h);
    return TRUE;
}

static void GetDefaultPrintHandles(HGLOBAL *outDevMode, HGLOBAL *outDevNames) {
    if (outDevMode) *outDevMode = NULL;
    if (outDevNames) *outDevNames = NULL;
    PRINTDLGW pd = {0};
    pd.lStructSize = sizeof(pd);
    pd.Flags = PD_RETURNDEFAULT;
    if (PrintDlgW(&pd)) {
        if (outDevMode) *outDevMode = pd.hDevMode;
        if (outDevNames) *outDevNames = pd.hDevNames;
    } else {
        if (pd.hDevMode) GlobalFree(pd.hDevMode);
        if (pd.hDevNames) GlobalFree(pd.hDevNames);
    }
}

static void EnsureComctl() {
    static BOOL s_inited = FALSE;
    if (s_inited) return;
    INITCOMMONCONTROLSEX icc = {0};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    s_inited = TRUE;
}

static HGLOBAL DuplicateGlobalHandle(HGLOBAL h) {
    if (!IsValidGlobalHandle(h)) return NULL;
    SIZE_T size = GlobalSize(h);
    if (size == 0) return NULL;
    HGLOBAL dup = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!dup) return NULL;
    void *src = GlobalLock(h);
    void *dst = GlobalLock(dup);
    if (!src || !dst) {
        if (dst) GlobalUnlock(dup);
        GlobalFree(dup);
        if (src) GlobalUnlock(h);
        return NULL;
    }
    CopyMemory(dst, src, size);
    GlobalUnlock(h);
    GlobalUnlock(dup);
    return dup;
}
