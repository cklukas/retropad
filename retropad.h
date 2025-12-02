#pragma once

#include <windows.h>
#include "file_io.h"

#define APP_TITLE      L"retropad"
#define UNTITLED_NAME  L"Untitled"
#define MAX_PATH_BUFFER 1024
#define DEFAULT_WIDTH  640
#define DEFAULT_HEIGHT 480
#define HH_DISPLAY_TOPIC 0x0000

typedef struct AppState {
    HWND hwndMain;
    HWND hwndEdit;
    HWND hwndStatus;
    HFONT hFont;
    UINT fontDpi;
    BOOL fontIsDefault;
    HGLOBAL hDevMode;
    HGLOBAL hDevNames;
    RECT marginsThousandths; // left/top/right/bottom in thousandths of an inch
    WCHAR headerText[128];
    WCHAR footerText[128];
    WCHAR currentPath[MAX_PATH_BUFFER];
    BOOL wordWrap;
    BOOL statusVisible;
    BOOL statusBeforeWrap;
    BOOL modified;
    TextEncoding encoding;
    FINDREPLACEW find;
    HWND hFindDlg;
    HWND hReplaceDlg;
    UINT findFlags;
    WCHAR findText[128];
    WCHAR replaceText[128];
} AppState;

extern AppState g_app;
extern HINSTANCE g_hInst;
