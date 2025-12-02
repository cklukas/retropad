// retropad - a Petzold-style Win32 notepad clone implemented in mostly plain C.
// Keeps the classic menus/accelerators, word wrap, status bar, find/replace,
// font picker, and basic file load/save with BOM detection.
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <strsafe.h>
#include <stdlib.h>
#include "resource.h"
#include "file_io.h"
#include "retropad.h"
#include "print.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

typedef HWND (WINAPI *PFNHTMLHELPW)(HWND, LPCWSTR, UINT, ULONG_PTR);

AppState g_app = {0};
HINSTANCE g_hInst = NULL;
static UINT g_findMsg = 0;
static HMODULE g_hhLib = NULL;
static PFNHTMLHELPW g_pHtmlHelp = NULL;

static void UpdateTitle(HWND hwnd);
static void CreateEditControl(HWND hwnd);
static void UpdateLayout(HWND hwnd);
static BOOL PromptSaveChanges(HWND hwnd);
static BOOL DoFileOpen(HWND hwnd);
static BOOL DoFileSave(HWND hwnd, BOOL saveAs);
static void DoFileNew(HWND hwnd);
static void SetWordWrap(HWND hwnd, BOOL enabled);
static void ToggleStatusBar(HWND hwnd, BOOL visible);
static void UpdateStatusBar(HWND hwnd);
static void ShowFindDialog(HWND hwnd);
static void ShowReplaceDialog(HWND hwnd);
static BOOL DoFindNext(BOOL reverse);
static void DoSelectFont(HWND hwnd);
static void InsertTimeDate(HWND hwnd);
static void HandleFindReplace(LPFINDREPLACE lpfr);
static BOOL LoadDocumentFromPath(HWND hwnd, LPCWSTR path);
static INT_PTR CALLBACK GoToDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam);
static INT_PTR CALLBACK AboutDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam);
static HFONT CreateDefaultUIFont(HWND hwnd);
static void ShowHelp(HWND hwnd);
static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data);

static BOOL GetEditText(HWND hwndEdit, WCHAR **bufferOut, int *lengthOut) {
    int length = GetWindowTextLengthW(hwndEdit);
    WCHAR *buffer = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(WCHAR));
    if (!buffer) return FALSE;
    GetWindowTextW(hwndEdit, buffer, length + 1);
    if (lengthOut) *lengthOut = length;
    *bufferOut = buffer;
    return TRUE;
}

static BOOL FindInEdit(HWND hwndEdit, const WCHAR *needle, BOOL matchCase, BOOL searchDown, DWORD startPos, DWORD *outStart, DWORD *outEnd) {
    if (!needle || needle[0] == L'\0') return FALSE;

    WCHAR *text = NULL;
    int len = 0;
    if (!GetEditText(hwndEdit, &text, &len)) return FALSE;

    size_t needleLen = wcslen(needle);
    WCHAR *haystack = text;
    WCHAR *needleBuf = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (needleLen + 1) * sizeof(WCHAR));
    if (!needleBuf) {
        HeapFree(GetProcessHeap(), 0, text);
        return FALSE;
    }
    StringCchCopyW(needleBuf, needleLen + 1, needle);

    if (!matchCase) {
        CharLowerBuffW(haystack, len);
        CharLowerBuffW(needleBuf, (DWORD)needleLen);
    }

    if (startPos > (DWORD)len) startPos = (DWORD)len;

    WCHAR *found = NULL;
    if (searchDown) {
        found = wcsstr(haystack + startPos, needleBuf);
        if (!found && startPos > 0) {
            found = wcsstr(haystack, needleBuf);
        }
    } else {
        WCHAR *p = haystack;
        while ((p = wcsstr(p, needleBuf)) != NULL) {
            DWORD idx = (DWORD)(p - haystack);
            if (idx < startPos) {
                found = p;
                p++;
            } else {
                break;
            }
        }
        if (!found && startPos < (DWORD)len) {
            p = haystack + startPos;
            while ((p = wcsstr(p, needleBuf)) != NULL) {
                found = p;
                p++;
            }
        }
    }

    BOOL result = FALSE;
    if (found) {
        DWORD pos = (DWORD)(found - haystack);
        *outStart = pos;
        *outEnd = pos + (DWORD)needleLen;
        result = TRUE;
    }

    HeapFree(GetProcessHeap(), 0, text);
    HeapFree(GetProcessHeap(), 0, needleBuf);
    return result;
}

static int ReplaceAllOccurrences(HWND hwndEdit, const WCHAR *needle, const WCHAR *replacement, BOOL matchCase) {
    if (!needle || needle[0] == L'\0') return 0;

    WCHAR *text = NULL;
    int len = 0;
    if (!GetEditText(hwndEdit, &text, &len)) return 0;

    size_t needleLen = wcslen(needle);
    size_t replLen = replacement ? wcslen(replacement) : 0;

    WCHAR *searchBuf = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
    WCHAR *needleBuf = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (needleLen + 1) * sizeof(WCHAR));
    if (!searchBuf || !needleBuf) {
        HeapFree(GetProcessHeap(), 0, text);
        if (searchBuf) HeapFree(GetProcessHeap(), 0, searchBuf);
        if (needleBuf) HeapFree(GetProcessHeap(), 0, needleBuf);
        return 0;
    }
    StringCchCopyW(searchBuf, len + 1, text);
    StringCchCopyW(needleBuf, needleLen + 1, needle);

    if (!matchCase) {
        CharLowerBuffW(searchBuf, len);
        CharLowerBuffW(needleBuf, (DWORD)needleLen);
    }

    int count = 0;
    WCHAR *p = searchBuf;
    while ((p = wcsstr(p, needleBuf)) != NULL) {
        count++;
        p += needleLen;
    }
    if (count == 0) {
        HeapFree(GetProcessHeap(), 0, text);
        HeapFree(GetProcessHeap(), 0, searchBuf);
        HeapFree(GetProcessHeap(), 0, needleBuf);
        return 0;
    }

    size_t newLen = (size_t)len - (size_t)count * needleLen + (size_t)count * replLen;
    WCHAR *result = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (newLen + 1) * sizeof(WCHAR));
    if (!result) {
        HeapFree(GetProcessHeap(), 0, text);
        HeapFree(GetProcessHeap(), 0, searchBuf);
        HeapFree(GetProcessHeap(), 0, needleBuf);
        return 0;
    }

    WCHAR *dst = result;
    WCHAR *searchCur = searchBuf;
    WCHAR *origCur = text;
    while ((p = wcsstr(searchCur, needleBuf)) != NULL) {
        size_t delta = (size_t)(p - searchCur);
        CopyMemory(dst, origCur, delta * sizeof(WCHAR));
        dst += delta;
        origCur += delta;
        searchCur += delta;

        if (replLen) {
            CopyMemory(dst, replacement, replLen * sizeof(WCHAR));
            dst += replLen;
        }
        origCur += needleLen;
        searchCur += needleLen;
    }
    size_t tail = wcslen(origCur);
    CopyMemory(dst, origCur, tail * sizeof(WCHAR));
    dst += tail;
    *dst = L'\0';

    SetWindowTextW(hwndEdit, result);
    HeapFree(GetProcessHeap(), 0, text);
    HeapFree(GetProcessHeap(), 0, searchBuf);
    HeapFree(GetProcessHeap(), 0, needleBuf);
    HeapFree(GetProcessHeap(), 0, result);
    SendMessageW(hwndEdit, EM_SETMODIFY, TRUE, 0);
    g_app.modified = TRUE;
    UpdateTitle(g_app.hwndMain);
    return count;
}

static void UpdateTitle(HWND hwnd) {
    WCHAR name[MAX_PATH_BUFFER];
    if (g_app.currentPath[0]) {
        WCHAR *fileName = wcsrchr(g_app.currentPath, L'\\');
        fileName = fileName ? fileName + 1 : g_app.currentPath;
        StringCchCopyW(name, MAX_PATH_BUFFER, fileName);
    } else {
        StringCchCopyW(name, MAX_PATH_BUFFER, UNTITLED_NAME);
    }

    WCHAR title[MAX_PATH_BUFFER + 32];
    StringCchPrintfW(title, ARRAYSIZE(title), L"%s%s - %s", (g_app.modified ? L"*" : L""), name, APP_TITLE);
    SetWindowTextW(hwnd, title);
}

static void ApplyFontToEdit(HWND hwndEdit, HFONT font) {
    SendMessageW(hwndEdit, WM_SETFONT, (WPARAM)font, TRUE);
}

static UINT GetWindowDpi(HWND hwnd) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef UINT (WINAPI *GETDPIFORWINDOW)(HWND);
        GETDPIFORWINDOW pGetDpiForWindow = (GETDPIFORWINDOW)GetProcAddress(user32, "GetDpiForWindow");
        if (pGetDpiForWindow) {
            return pGetDpiForWindow(hwnd);
        }
    }
    HDC hdc = GetDC(hwnd);
    UINT dpi = 96;
    if (hdc) {
        dpi = (UINT)GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(hwnd, hdc);
    }
    return dpi;
}

static HFONT CreateDefaultUIFont(HWND hwnd) {
    LOGFONTW lf = {0};
    UINT dpi = GetWindowDpi(hwnd);
    lf.lfHeight = -MulDiv(10, (int)dpi, 72); // 10pt UI font at current DPI
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    StringCchCopyW(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), L"Segoe UI");
    g_app.fontDpi = dpi;
    g_app.fontIsDefault = TRUE;
    return CreateFontIndirectW(&lf);
}

static void CreateEditControl(HWND hwnd) {
    if (g_app.hwndEdit) {
        DestroyWindow(g_app.hwndEdit);
    }

    DWORD style = WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL;
    if (!g_app.wordWrap) {
        style |= WS_HSCROLL | ES_AUTOHSCROLL;
    }

    g_app.hwndEdit = CreateWindowExW(0, L"EDIT", NULL, style, 0, 0, 0, 0, hwnd, (HMENU)1, g_hInst, NULL);
    if (!g_app.hFont) {
        g_app.hFont = CreateDefaultUIFont(hwnd);
    }
    if (g_app.hwndEdit && g_app.hFont) {
        ApplyFontToEdit(g_app.hwndEdit, g_app.hFont);
    }
    if (g_app.hwndEdit) {
        SetWindowSubclass(g_app.hwndEdit, EditSubclassProc, 1, 0);
    }
    SendMessageW(g_app.hwndEdit, EM_SETLIMITTEXT, 0, 0); // allow large files
    UpdateLayout(hwnd);
}

static void ToggleStatusBar(HWND hwnd, BOOL visible) {
    g_app.statusVisible = visible;
    if (visible) {
        if (!g_app.hwndStatus) {
            g_app.hwndStatus = CreateStatusWindowW(WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP | CCS_NODIVIDER, L"", hwnd, 2);
        }
        ShowWindow(g_app.hwndStatus, SW_SHOW);
    } else if (g_app.hwndStatus) {
        ShowWindow(g_app.hwndStatus, SW_HIDE);
    }
    UpdateLayout(hwnd);
    UpdateStatusBar(hwnd);
}

static void UpdateLayout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);

    int statusHeight = 0;
    if (g_app.statusVisible && g_app.hwndStatus) {
        SendMessageW(g_app.hwndStatus, WM_SIZE, 0, 0);
        RECT sbrc;
        GetWindowRect(g_app.hwndStatus, &sbrc);
        statusHeight = sbrc.bottom - sbrc.top;
        MoveWindow(g_app.hwndStatus, 0, rc.bottom - statusHeight, rc.right, statusHeight, TRUE);
    }

    if (g_app.hwndEdit) {
        MoveWindow(g_app.hwndEdit, 0, 0, rc.right, rc.bottom - statusHeight, TRUE);
    }
}

static BOOL PromptSaveChanges(HWND hwnd) {
    if (!g_app.modified) return TRUE;

    WCHAR prompt[MAX_PATH_BUFFER + 64];
    const WCHAR *name = g_app.currentPath[0] ? g_app.currentPath : UNTITLED_NAME;
    StringCchPrintfW(prompt, ARRAYSIZE(prompt), L"Do you want to save changes to %s?", name);
    int res = MessageBoxW(hwnd, prompt, APP_TITLE, MB_ICONQUESTION | MB_YESNOCANCEL);
    if (res == IDYES) {
        return DoFileSave(hwnd, FALSE);
    }
    return res == IDNO;
}

static BOOL LoadDocumentFromPath(HWND hwnd, LPCWSTR path) {
    WCHAR *text = NULL;
    TextEncoding enc = ENC_UTF8;
    if (!LoadTextFile(hwnd, path, &text, NULL, &enc)) {
        return FALSE;
    }

    SetWindowTextW(g_app.hwndEdit, text);
    HeapFree(GetProcessHeap(), 0, text);
    StringCchCopyW(g_app.currentPath, ARRAYSIZE(g_app.currentPath), path);
    g_app.encoding = enc;
    SendMessageW(g_app.hwndEdit, EM_SETMODIFY, FALSE, 0);
    g_app.modified = FALSE;
    UpdateTitle(hwnd);
    UpdateStatusBar(hwnd);
    return TRUE;
}

static BOOL DoFileOpen(HWND hwnd) {
    if (!PromptSaveChanges(hwnd)) return FALSE;

    WCHAR path[MAX_PATH_BUFFER] = L"";
    if (!OpenFileDialog(hwnd, path, ARRAYSIZE(path))) {
        return FALSE;
    }
    return LoadDocumentFromPath(hwnd, path);
}

static BOOL DoFileSave(HWND hwnd, BOOL saveAs) {
    WCHAR path[MAX_PATH_BUFFER];
    if (saveAs || g_app.currentPath[0] == L'\0') {
        path[0] = L'\0';
        if (g_app.currentPath[0]) {
            StringCchCopyW(path, ARRAYSIZE(path), g_app.currentPath);
        }
        if (!SaveFileDialog(hwnd, path, ARRAYSIZE(path))) {
            return FALSE;
        }
        StringCchCopyW(g_app.currentPath, ARRAYSIZE(g_app.currentPath), path);
    } else {
        StringCchCopyW(path, ARRAYSIZE(path), g_app.currentPath);
    }

    int len = GetWindowTextLengthW(g_app.hwndEdit);
    WCHAR *buffer = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
    if (!buffer) return FALSE;
    GetWindowTextW(g_app.hwndEdit, buffer, len + 1);

    BOOL ok = SaveTextFile(hwnd, path, buffer, len, g_app.encoding);
    HeapFree(GetProcessHeap(), 0, buffer);
    if (ok) {
        SendMessageW(g_app.hwndEdit, EM_SETMODIFY, FALSE, 0);
        g_app.modified = FALSE;
        UpdateTitle(hwnd);
    }
    return ok;
}

static void DoFileNew(HWND hwnd) {
    if (!PromptSaveChanges(hwnd)) return;
    SetWindowTextW(g_app.hwndEdit, L"");
    g_app.currentPath[0] = L'\0';
    g_app.encoding = ENC_UTF8;
    SendMessageW(g_app.hwndEdit, EM_SETMODIFY, FALSE, 0);
    g_app.modified = FALSE;
    UpdateTitle(hwnd);
    UpdateStatusBar(hwnd);
}

static void SetWordWrap(HWND hwnd, BOOL enabled) {
    if (g_app.wordWrap == enabled) return;
    HWND edit = g_app.hwndEdit;
    WCHAR *text = NULL;
    int len = 0;
    if (!GetEditText(edit, &text, &len)) {
        return;
    }
    DWORD start = 0, end = 0;
    SendMessageW(edit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);

    g_app.wordWrap = enabled;
    CreateEditControl(hwnd);
    SetWindowTextW(g_app.hwndEdit, text);
    SendMessageW(g_app.hwndEdit, EM_SETSEL, start, end);
    HeapFree(GetProcessHeap(), 0, text);

    if (enabled) {
        g_app.statusBeforeWrap = g_app.statusVisible;
        ToggleStatusBar(hwnd, FALSE);
        EnableMenuItem(GetMenu(hwnd), IDM_VIEW_STATUS_BAR, MF_BYCOMMAND | MF_GRAYED);
        EnableMenuItem(GetMenu(hwnd), IDM_EDIT_GOTO, MF_BYCOMMAND | MF_GRAYED);
    } else {
        ToggleStatusBar(hwnd, g_app.statusBeforeWrap);
        EnableMenuItem(GetMenu(hwnd), IDM_VIEW_STATUS_BAR, MF_BYCOMMAND | MF_ENABLED);
        EnableMenuItem(GetMenu(hwnd), IDM_EDIT_GOTO, MF_BYCOMMAND | MF_ENABLED);
    }
    UpdateTitle(hwnd);
    UpdateStatusBar(hwnd);
}

static void UpdateStatusBar(HWND hwnd) {
    (void)hwnd;
    if (!g_app.statusVisible || !g_app.hwndStatus) return;
    DWORD selStart = 0, selEnd = 0;
    SendMessageW(g_app.hwndEdit, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);
    int line = (int)SendMessageW(g_app.hwndEdit, EM_LINEFROMCHAR, selStart, 0) + 1;
    int col = (int)(selStart - SendMessageW(g_app.hwndEdit, EM_LINEINDEX, line - 1, 0)) + 1;
    int lines = (int)SendMessageW(g_app.hwndEdit, EM_GETLINECOUNT, 0, 0);

    WCHAR status[128];
    StringCchPrintfW(status, ARRAYSIZE(status), L"Ln %d, Col %d    Lines: %d", line, col, lines);
    SendMessageW(g_app.hwndStatus, SB_SETTEXT, SBT_NOBORDERS, (LPARAM)status);
}

static void ShowFindDialog(HWND hwnd) {
    if (g_app.hFindDlg) {
        SetForegroundWindow(g_app.hFindDlg);
        return;
    }

    ZeroMemory(&g_app.find, sizeof(g_app.find));
    g_app.find.lStructSize = sizeof(FINDREPLACEW);
    g_app.find.hwndOwner = hwnd;
    g_app.find.lpstrFindWhat = g_app.findText;
    g_app.find.wFindWhatLen = ARRAYSIZE(g_app.findText);
    g_app.find.Flags = g_app.findFlags;

    g_app.hFindDlg = FindTextW(&g_app.find);
}

static void ShowReplaceDialog(HWND hwnd) {
    if (g_app.hReplaceDlg) {
        SetForegroundWindow(g_app.hReplaceDlg);
        return;
    }

    ZeroMemory(&g_app.find, sizeof(g_app.find));
    g_app.find.lStructSize = sizeof(FINDREPLACEW);
    g_app.find.hwndOwner = hwnd;
    g_app.find.lpstrFindWhat = g_app.findText;
    g_app.find.lpstrReplaceWith = g_app.replaceText;
    g_app.find.wFindWhatLen = ARRAYSIZE(g_app.findText);
    g_app.find.wReplaceWithLen = ARRAYSIZE(g_app.replaceText);
    g_app.find.Flags = g_app.findFlags;

    g_app.hReplaceDlg = ReplaceTextW(&g_app.find);
}

static BOOL DoFindNext(BOOL reverse) {
    if (g_app.findText[0] == L'\0') {
        ShowFindDialog(g_app.hwndMain);
        return FALSE;
    }

    DWORD start = 0, end = 0;
    SendMessageW(g_app.hwndEdit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    BOOL matchCase = (g_app.findFlags & FR_MATCHCASE) != 0;
    BOOL down = (g_app.findFlags & FR_DOWN) != 0;
    if (reverse) down = !down;
    DWORD searchStart = down ? end : start;
    DWORD outStart = 0, outEnd = 0;
    if (FindInEdit(g_app.hwndEdit, g_app.findText, matchCase, down, searchStart, &outStart, &outEnd)) {
        SendMessageW(g_app.hwndEdit, EM_SETSEL, outStart, outEnd);
        SendMessageW(g_app.hwndEdit, EM_SCROLLCARET, 0, 0);
        return TRUE;
    }
    MessageBoxW(g_app.hwndMain, L"Cannot find the text.", APP_TITLE, MB_ICONINFORMATION);
    return FALSE;
}

static INT_PTR CALLBACK GoToDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG: {
        SetDlgItemInt(dlg, IDC_GOTO_EDIT, 1, FALSE);
        HWND edit = GetDlgItem(dlg, IDC_GOTO_EDIT);
        SendMessageW(edit, EM_SETLIMITTEXT, 10, 0);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK: {
            BOOL ok = FALSE;
            UINT line = GetDlgItemInt(dlg, IDC_GOTO_EDIT, &ok, FALSE);
            if (!ok || line == 0) {
                MessageBoxW(dlg, L"Enter a valid line number.", APP_TITLE, MB_ICONWARNING);
                return TRUE;
            }
            int maxLine = (int)SendMessageW(g_app.hwndEdit, EM_GETLINECOUNT, 0, 0);
            if ((int)line > maxLine) line = (UINT)maxLine;
            int charIndex = (int)SendMessageW(g_app.hwndEdit, EM_LINEINDEX, line - 1, 0);
            if (charIndex >= 0) {
                SendMessageW(g_app.hwndEdit, EM_SETSEL, charIndex, charIndex);
                SendMessageW(g_app.hwndEdit, EM_SCROLLCARET, 0, 0);
            }
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

static void DoSelectFont(HWND hwnd) {
    LOGFONTW lf = {0};
    if (g_app.hFont) {
        GetObjectW(g_app.hFont, sizeof(LOGFONTW), &lf);
    } else {
        SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(LOGFONTW), &lf, 0);
    }

    CHOOSEFONTW cf = {0};
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = hwnd;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT;

    if (ChooseFontW(&cf)) {
        HFONT newFont = CreateFontIndirectW(&lf);
        if (newFont) {
            if (g_app.hFont) DeleteObject(g_app.hFont);
            g_app.hFont = newFont;
            g_app.fontIsDefault = FALSE;
            g_app.fontDpi = GetWindowDpi(hwnd);
            ApplyFontToEdit(g_app.hwndEdit, g_app.hFont);
            UpdateLayout(hwnd);
        }
    }
}

static void InsertTimeDate(HWND hwnd) {
    (void)hwnd;
    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR date[64], time[64], stamp[128];
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, date, ARRAYSIZE(date));
    GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, time, ARRAYSIZE(time));
    StringCchPrintfW(stamp, ARRAYSIZE(stamp), L"%s %s", time, date);
    SendMessageW(g_app.hwndEdit, EM_REPLACESEL, TRUE, (LPARAM)stamp);
}

static void ShowHelp(HWND hwnd) {
    WCHAR path[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
    if (len == 0 || len >= ARRAYSIZE(path)) {
        MessageBoxW(hwnd, L"Unable to locate help file.", APP_TITLE, MB_ICONERROR);
        return;
    }
    WCHAR *slash = wcsrchr(path, L'\\');
    if (slash) {
        StringCchCopyW(slash + 1, ARRAYSIZE(path) - (slash - path) - 1, L"retropad.chm");
    } else {
        StringCchCopyW(path, ARRAYSIZE(path), L"retropad.chm");
    }

    DWORD attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(hwnd, L"Help file not found.", APP_TITLE, MB_ICONINFORMATION);
        return;
    }

    if (!g_hhLib) {
        g_hhLib = LoadLibraryW(L"hhctrl.ocx");
    }
    if (!g_hhLib) {
        MessageBoxW(hwnd, L"HTML Help components are unavailable.", APP_TITLE, MB_ICONERROR);
        return;
    }
    if (!g_pHtmlHelp) {
        g_pHtmlHelp = (PFNHTMLHELPW)GetProcAddress(g_hhLib, "HtmlHelpW");
    }
    if (!g_pHtmlHelp) {
        FreeLibrary(g_hhLib);
        g_hhLib = NULL;
        MessageBoxW(hwnd, L"HTML Help components are unavailable.", APP_TITLE, MB_ICONERROR);
        return;
    }
    g_pHtmlHelp(hwnd, path, HH_DISPLAY_TOPIC, 0);
}
static void HandleFindReplace(LPFINDREPLACE lpfr) {
    if (lpfr->Flags & FR_DIALOGTERM) {
        g_app.hFindDlg = NULL;
        g_app.hReplaceDlg = NULL;
        return;
    }

    g_app.findFlags = lpfr->Flags;
    if (lpfr->lpstrFindWhat && lpfr->lpstrFindWhat[0]) {
        StringCchCopyW(g_app.findText, ARRAYSIZE(g_app.findText), lpfr->lpstrFindWhat);
    }
    if (lpfr->lpstrReplaceWith) {
        StringCchCopyW(g_app.replaceText, ARRAYSIZE(g_app.replaceText), lpfr->lpstrReplaceWith);
    }

    BOOL matchCase = (lpfr->Flags & FR_MATCHCASE) != 0;
    BOOL down = (lpfr->Flags & FR_DOWN) != 0;

    if (lpfr->Flags & FR_FINDNEXT) {
        DWORD start = 0, end = 0;
        SendMessageW(g_app.hwndEdit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
        DWORD searchStart = down ? end : start;
        DWORD outStart = 0, outEnd = 0;
        if (FindInEdit(g_app.hwndEdit, g_app.findText, matchCase, down, searchStart, &outStart, &outEnd)) {
            SendMessageW(g_app.hwndEdit, EM_SETSEL, outStart, outEnd);
            SendMessageW(g_app.hwndEdit, EM_SCROLLCARET, 0, 0);
        } else {
            MessageBoxW(g_app.hwndMain, L"Cannot find the text.", APP_TITLE, MB_ICONINFORMATION);
        }
    } else if (lpfr->Flags & FR_REPLACE) {
        DWORD start = 0, end = 0;
        SendMessageW(g_app.hwndEdit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
        DWORD outStart = 0, outEnd = 0;
        if (FindInEdit(g_app.hwndEdit, g_app.findText, matchCase, down, start, &outStart, &outEnd)) {
            SendMessageW(g_app.hwndEdit, EM_SETSEL, outStart, outEnd);
            SendMessageW(g_app.hwndEdit, EM_REPLACESEL, TRUE, (LPARAM)g_app.replaceText);
            SendMessageW(g_app.hwndEdit, EM_SCROLLCARET, 0, 0);
            g_app.modified = TRUE;
            UpdateTitle(g_app.hwndMain);
        } else {
            MessageBoxW(g_app.hwndMain, L"Cannot find the text.", APP_TITLE, MB_ICONINFORMATION);
        }
    } else if (lpfr->Flags & FR_REPLACEALL) {
        int replaced = ReplaceAllOccurrences(g_app.hwndEdit, g_app.findText, g_app.replaceText, matchCase);
        WCHAR msg[64];
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"Replaced %d occurrence%s.", replaced, replaced == 1 ? L"" : L"s");
        MessageBoxW(g_app.hwndMain, msg, APP_TITLE, MB_OK | MB_ICONINFORMATION);
    }
}

static void UpdateMenuStates(HWND hwnd) {
    HMENU menu = GetMenu(hwnd);
    if (!menu) return;

    UINT wrapState = g_app.wordWrap ? MF_CHECKED : MF_UNCHECKED;
    UINT statusState = g_app.statusVisible ? MF_CHECKED : MF_UNCHECKED;
    CheckMenuItem(menu, IDM_FORMAT_WORD_WRAP, MF_BYCOMMAND | wrapState);
    CheckMenuItem(menu, IDM_VIEW_STATUS_BAR, MF_BYCOMMAND | statusState);

    BOOL canGoTo = !g_app.wordWrap;
    EnableMenuItem(menu, IDM_EDIT_GOTO, MF_BYCOMMAND | (canGoTo ? MF_ENABLED : MF_GRAYED));
    if (g_app.wordWrap) {
        EnableMenuItem(menu, IDM_VIEW_STATUS_BAR, MF_BYCOMMAND | MF_GRAYED);
    } else {
        EnableMenuItem(menu, IDM_VIEW_STATUS_BAR, MF_BYCOMMAND | MF_ENABLED);
    }

    BOOL modified = (SendMessageW(g_app.hwndEdit, EM_GETMODIFY, 0, 0) != 0);
    EnableMenuItem(menu, IDM_FILE_SAVE, MF_BYCOMMAND | (modified ? MF_ENABLED : MF_GRAYED));
}

static void HandleCommand(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    switch (LOWORD(wParam)) {
    case IDM_FILE_NEW:
        DoFileNew(hwnd);
        break;
    case IDM_FILE_OPEN:
        DoFileOpen(hwnd);
        break;
    case IDM_FILE_SAVE:
        DoFileSave(hwnd, FALSE);
        break;
    case IDM_FILE_SAVE_AS:
        DoFileSave(hwnd, TRUE);
        break;
    case IDM_FILE_PAGE_SETUP:
        DoPageSetup(hwnd);
        break;
    case IDM_FILE_PRINT:
        DoPrint(hwnd);
        break;
    case IDM_FILE_EXIT:
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        break;

    case IDM_EDIT_UNDO:
        SendMessageW(g_app.hwndEdit, EM_UNDO, 0, 0);
        break;
    case IDM_EDIT_CUT:
        SendMessageW(g_app.hwndEdit, WM_CUT, 0, 0);
        break;
    case IDM_EDIT_COPY:
        SendMessageW(g_app.hwndEdit, WM_COPY, 0, 0);
        break;
    case IDM_EDIT_PASTE:
        SendMessageW(g_app.hwndEdit, WM_PASTE, 0, 0);
        break;
    case IDM_EDIT_DELETE:
        SendMessageW(g_app.hwndEdit, WM_CLEAR, 0, 0);
        break;
    case IDM_EDIT_FIND:
        ShowFindDialog(hwnd);
        break;
    case IDM_EDIT_FIND_NEXT:
        DoFindNext(FALSE);
        break;
    case IDM_EDIT_REPLACE:
        ShowReplaceDialog(hwnd);
        break;
    case IDM_EDIT_GOTO:
        if (g_app.wordWrap) {
            MessageBoxW(hwnd, L"Go To is unavailable when Word Wrap is on.", APP_TITLE, MB_ICONINFORMATION);
        } else {
            DialogBoxW(g_hInst, MAKEINTRESOURCE(IDD_GOTO), hwnd, GoToDlgProc);
        }
        break;
    case IDM_EDIT_SELECT_ALL:
        SendMessageW(g_app.hwndEdit, EM_SETSEL, 0, -1);
        break;
    case IDM_EDIT_TIME_DATE:
        InsertTimeDate(hwnd);
        break;

    case IDM_FORMAT_WORD_WRAP:
        SetWordWrap(hwnd, !g_app.wordWrap);
        break;
    case IDM_FORMAT_FONT:
        DoSelectFont(hwnd);
        break;

    case IDM_VIEW_STATUS_BAR:
        ToggleStatusBar(hwnd, !g_app.statusVisible);
        break;

    case IDM_HELP_VIEW_HELP:
        ShowHelp(hwnd);
        break;
    case IDM_HELP_ABOUT:
        DialogBoxW(g_hInst, MAKEINTRESOURCE(IDD_ABOUT), hwnd, AboutDlgProc);
        break;
    }
}

static INT_PTR CALLBACK AboutDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(dlg, LOWORD(wParam));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_findMsg) {
        HandleFindReplace((LPFINDREPLACE)lParam);
        return 0;
    }

    switch (msg) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icc);
        CreateEditControl(hwnd);
        ToggleStatusBar(hwnd, TRUE);
        UpdateTitle(hwnd);
        UpdateStatusBar(hwnd);
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    }
    case WM_SETFOCUS:
        if (g_app.hwndEdit) SetFocus(g_app.hwndEdit);
        return 0;
    case WM_SIZE:
        UpdateLayout(hwnd);
        UpdateStatusBar(hwnd);
        return 0;
    case WM_DPICHANGED: {
        RECT *prcNew = (RECT *)lParam;
        if (prcNew) {
            MoveWindow(hwnd, prcNew->left, prcNew->top,
                       prcNew->right - prcNew->left,
                       prcNew->bottom - prcNew->top, TRUE);
        }
        UINT newDpi = LOWORD(wParam);
        if (!newDpi) newDpi = GetWindowDpi(hwnd);
        BOOL fontUpdated = FALSE;
        if (g_app.hFont) {
            LOGFONTW lf;
            if (GetObjectW(g_app.hFont, sizeof(lf), &lf)) {
                UINT oldDpi = g_app.fontDpi ? g_app.fontDpi : 96;
                lf.lfHeight = MulDiv(lf.lfHeight, (int)newDpi, (int)oldDpi);
                if (lf.lfWidth != 0) {
                    lf.lfWidth = MulDiv(lf.lfWidth, (int)newDpi, (int)oldDpi);
                }
                HFONT newFont = CreateFontIndirectW(&lf);
                if (newFont) {
                    DeleteObject(g_app.hFont);
                    g_app.hFont = newFont;
                    fontUpdated = TRUE;
                }
            }
        } else {
            g_app.hFont = CreateDefaultUIFont(hwnd);
            fontUpdated = (g_app.hFont != NULL);
        }
        if (fontUpdated) {
            g_app.fontDpi = newDpi;
        }
        if (g_app.hwndEdit && g_app.hFont) {
            ApplyFontToEdit(g_app.hwndEdit, g_app.hFont);
        }
        UpdateLayout(hwnd);
        UpdateStatusBar(hwnd);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        WCHAR path[MAX_PATH_BUFFER];
        if (DragQueryFileW(hDrop, 0, path, ARRAYSIZE(path))) {
            if (PromptSaveChanges(hwnd)) {
                LoadDocumentFromPath(hwnd, path);
            }
        }
        DragFinish(hDrop);
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wParam) == EN_CHANGE && (HWND)lParam == g_app.hwndEdit) {
            g_app.modified = (SendMessageW(g_app.hwndEdit, EM_GETMODIFY, 0, 0) != 0);
            UpdateTitle(hwnd);
            UpdateStatusBar(hwnd);
            return 0;
        } else if (HIWORD(wParam) == EN_UPDATE && (HWND)lParam == g_app.hwndEdit) {
            UpdateStatusBar(hwnd);
            return 0;
        }
        HandleCommand(hwnd, wParam, lParam);
        return 0;
    case WM_INITMENUPOPUP:
        UpdateMenuStates(hwnd);
        return 0;
    case WM_CLOSE:
        if (PromptSaveChanges(hwnd)) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        if (g_app.hDevMode) GlobalFree(g_app.hDevMode);
        if (g_app.hDevNames) GlobalFree(g_app.hDevNames);
        if (g_app.hFont) DeleteObject(g_app.hFont);
        if (g_hhLib) FreeLibrary(g_hhLib);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data) {
    (void)id;
    (void)data;
    switch (msg) {
    case WM_KEYUP:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_HSCROLL:
    case WM_VSCROLL:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case EM_SETSEL: {
        HWND parent = GetParent(hwnd);
        if (parent) UpdateStatusBar(parent);
        break;
    }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    g_hInst = hInstance;
    g_findMsg = RegisterWindowMessageW(FINDMSGSTRINGW);
    g_app.wordWrap = FALSE;
    g_app.statusVisible = TRUE;
    g_app.statusBeforeWrap = TRUE;
    g_app.encoding = ENC_UTF8;
    g_app.findFlags = FR_DOWN;
    g_app.marginsThousandths.left = g_app.marginsThousandths.right = 750;   // 0.75"
    g_app.marginsThousandths.top = g_app.marginsThousandths.bottom = 1000;  // 1.0"
    StringCchCopyW(g_app.headerText, ARRAYSIZE(g_app.headerText), L"&f");
    StringCchCopyW(g_app.footerText, ARRAYSIZE(g_app.footerText), L"Page &p of &P");

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(IDI_RETROPAD));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(NULL, IDC_IBEAM);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"RETROPAD_WINDOW";
    wc.lpszMenuName = MAKEINTRESOURCE(IDC_RETROPAD);

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"Failed to register window class.", APP_TITLE, MB_ICONERROR);
        return 0;
    }

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, APP_TITLE, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                NULL, NULL, hInstance, NULL);
    if (!hwnd) {
        MessageBoxW(NULL, L"Failed to create main window.", APP_TITLE, MB_ICONERROR);
        return 0;
    }

    g_app.hwndMain = hwnd;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    HACCEL accel = LoadAcceleratorsW(hInstance, MAKEINTRESOURCE(IDC_RETROPAD));

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!accel || !TranslateAcceleratorW(hwnd, accel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    return (int)msg.wParam;
}
