#include <sdkddkver.h>
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
#include "PrintPreviewWindow.h"

#ifndef PRINTDLGORD_EX
#define PRINTDLGORD_EX 1538
#endif

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

// Print context used by the modern print dialog preview callback.
PrintRenderContext g_printContext = {0};
static BOOL TryParseMargin(HWND dlg, int ctrlId, int *outThousandths);
static void SetMarginText(HWND dlg, int ctrlId, int thousandths);
static INT_PTR CALLBACK PageSetupDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam);

void DoPageSetup(HWND hwnd) {
    DialogBoxW(g_hInst, MAKEINTRESOURCE(IDD_PAGE_SETUP), hwnd, PageSetupDlgProc);
}

void DoPrint(HWND hwnd) {
    DebugLog(L"DoPrint invoked");
    int len = GetWindowTextLengthW(g_app.hwndEdit);
    WCHAR *buffer = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
    if (!buffer) {
        return;
    }
    GetWindowTextW(g_app.hwndEdit, buffer, len + 1);

    PrintRenderContext ctx = {
        .text = buffer,
        .fullPath = g_app.currentPath[0] ? g_app.currentPath : UNTITLED_NAME,
        .marginsThousandths = g_app.marginsThousandths,
        .headerText = g_app.headerText,
        .footerText = g_app.footerText,
        .testMode = g_app.testMode,
    };

    g_printContext = ctx;

    DebugLog(L"DoPrint: attempting modern preview");
    if (ShowModernPrintPreview(hwnd, &g_printContext)) {
        DebugLog(L"DoPrint: modern preview launched");
    } else {
        DebugLog(L"DoPrint: modern preview unavailable");
        if (!g_app.testMode) {
            MessageBoxW(hwnd, L"Modern print preview is unavailable.", APP_TITLE, MB_ICONERROR);
        }
    }

    HeapFree(GetProcessHeap(), 0, buffer);
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

