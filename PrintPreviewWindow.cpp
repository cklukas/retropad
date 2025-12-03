#include <windows.h>
#include <strsafe.h>
#include <thread>

#include "PrintPreviewWindow.h"
#include "rendering.h"

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

typedef HRESULT (WINAPI *PFNMddBootstrapInitialize)(UINT64 majorMinorVersion, PCWSTR versionTag, PCWSTR packageFamilyName);
typedef void (WINAPI *PFNMddBootstrapShutdown)(void);

static HMODULE g_bootstrapLib = NULL;
static PFNMddBootstrapShutdown g_bootstrapShutdown = NULL;
static BOOL g_bootstrapReady = FALSE;
static BOOL g_bootstrapAttempted = FALSE;

static void DebugLog(const WCHAR *msg) {
    WCHAR path[MAX_PATH];
    DWORD len = GetTempPathW(ARRAYSIZE(path), path);
    if (len == 0 || len >= ARRAYSIZE(path)) return;
    if (FAILED(StringCchCatW(path, ARRAYSIZE(path), L"retropad_preview.log"))) return;
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st; GetLocalTime(&st);
    WCHAR line[512];
    StringCchPrintfW(line, ARRAYSIZE(line),
        L"[%02u:%02u:%02u.%03u] %s\r\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        msg ? msg : L"(null)");
    DWORD written = 0;
    WriteFile(h, line, (DWORD)(lstrlenW(line) * sizeof(WCHAR)), &written, NULL);
    CloseHandle(h);
}

static BOOL BuildSiblingPath(WCHAR *buffer, size_t cchBuffer, const WCHAR *leaf) {
    if (!buffer || !leaf) return FALSE;
    DWORD len = GetModuleFileNameW(NULL, buffer, (DWORD)cchBuffer);
    if (len == 0 || len >= cchBuffer) return FALSE;
    WCHAR *slash = wcsrchr(buffer, L'\\');
    if (!slash) return FALSE;
    *(slash + 1) = L'\0';
    return SUCCEEDED(StringCchCatW(buffer, cchBuffer, leaf));
}

static HMODULE LoadBootstrapLibrary(void) {
    HMODULE lib = LoadLibraryW(L"Microsoft.WindowsAppRuntime.Bootstrap.dll");
    if (lib) {
        DebugLog(L"InitWindowsAppRuntime: loaded bootstrap from process PATH");
        return lib;
    }

    WCHAR path[MAX_PATH];
    if (BuildSiblingPath(path, ARRAYSIZE(path), L"Microsoft.WindowsAppRuntime.Bootstrap.dll")) {
        lib = LoadLibraryW(path);
        if (lib) {
            DebugLog(L"InitWindowsAppRuntime: loaded bootstrap from EXE dir");
            return lib;
        }
    }
    if (BuildSiblingPath(path, ARRAYSIZE(path), L"winappsdk\\Microsoft.WindowsAppRuntime.Bootstrap.dll")) {
        lib = LoadLibraryW(path);
        if (lib) {
            DebugLog(L"InitWindowsAppRuntime: loaded bootstrap from winappsdk\\");
            return lib;
        }
    }
    return NULL;
}

BOOL InitWindowsAppRuntime(void) {
    if (g_bootstrapReady) return TRUE;
    if (g_bootstrapAttempted) return FALSE;
    g_bootstrapAttempted = TRUE;

    DebugLog(L"InitWindowsAppRuntime: start");
    g_bootstrapLib = LoadBootstrapLibrary();
    if (!g_bootstrapLib) {
        DebugLog(L"InitWindowsAppRuntime: failed to load bootstrap DLL");
        g_bootstrapAttempted = FALSE;
        return FALSE;
    }

    PFNMddBootstrapInitialize initFn = (PFNMddBootstrapInitialize)GetProcAddress(g_bootstrapLib, "MddBootstrapInitialize");
    g_bootstrapShutdown = (PFNMddBootstrapShutdown)GetProcAddress(g_bootstrapLib, "MddBootstrapShutdown");
    if (!initFn) {
        DebugLog(L"InitWindowsAppRuntime: missing MddBootstrapInitialize export");
        FreeLibrary(g_bootstrapLib);
        g_bootstrapLib = NULL;
        g_bootstrapShutdown = NULL;
        g_bootstrapAttempted = FALSE;
        return FALSE;
    }

    HRESULT hr = initFn(0x00010008, NULL, L"Microsoft.WindowsAppRuntime.1.8_8wekyb3d8bbwe");
    if (FAILED(hr)) {
        WCHAR buf[128];
        StringCchPrintfW(buf, ARRAYSIZE(buf), L"InitWindowsAppRuntime: bootstrap initialize failed hr=0x%08lX", hr);
        DebugLog(buf);
        if (g_bootstrapLib) {
            FreeLibrary(g_bootstrapLib);
            g_bootstrapLib = NULL;
        }
        g_bootstrapShutdown = NULL;
        g_bootstrapAttempted = FALSE;
        return FALSE;
    }

    DebugLog(L"InitWindowsAppRuntime: bootstrap initialized");
    g_bootstrapReady = TRUE;
    return TRUE;
}

void ShutdownWindowsAppRuntime(void) {
    if (!g_bootstrapReady) return;
    if (g_bootstrapShutdown) {
        g_bootstrapShutdown();
    }
    if (g_bootstrapLib) {
        FreeLibrary(g_bootstrapLib);
        g_bootstrapLib = NULL;
    }
    g_bootstrapShutdown = NULL;
    g_bootstrapAttempted = FALSE;
    g_bootstrapReady = FALSE;
}

BOOL ModernPreviewAvailable(void) {
    if (!g_bootstrapReady) {
        DebugLog(L"ModernPreviewAvailable: bootstrap not ready, attempting init");
        InitWindowsAppRuntime();
    }
    DebugLog(g_bootstrapReady ? L"ModernPreviewAvailable: ready" : L"ModernPreviewAvailable: NOT ready");
    return g_bootstrapReady;
}

#if __has_include(<winrt/Windows.UI.Xaml.Hosting.h>) && __has_include(<winrt/Windows.UI.Xaml.Controls.h>) && __has_include(<winrt/Windows.UI.Xaml.Media.h>) && __has_include(<winrt/Windows.UI.Xaml.Printing.h>) && __has_include(<winrt/Windows.Graphics.Printing.h>) && __has_include(<winrt/Windows.Foundation.Collections.h>) && __has_include(<windows.ui.xaml.hosting.desktopwindowxamlsource.h>)
#define RETROPAD_HAS_WINUI 1
#include <unknwn.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Documents.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Printing.h>
#include <winrt/Windows.Graphics.Printing.h>
#include <winrt/Windows.UI.h>
#include <PrintManagerInterop.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>

using namespace winrt;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Documents;
using namespace Windows::UI::Xaml::Hosting;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Printing;
using namespace Windows::Graphics::Printing;
#else
#pragma message("WinUI headers missing - modern print preview disabled")
#endif

#if defined(RETROPAD_HAS_WINUI)
static PrintRenderContext *g_ctx = NULL;
static HWND g_previewWnd = NULL;
static void LogHr(const WCHAR *prefix, HRESULT hr) {
    WCHAR buf[128];
    StringCchPrintfW(buf, ARRAYSIZE(buf), L"%s hr=0x%08lX", prefix ? prefix : L"hr", hr);
    DebugLog(buf);
}

static LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_DESTROY:
        g_previewWnd = NULL;
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static BOOL CreatePreviewHostWindow(HWND parent) {
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = PreviewWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"RetropadPreviewWindow";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    g_previewWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"Print Preview",
        WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, 1000, 720,
        parent, NULL, wc.hInstance, NULL);
    return g_previewWnd != NULL;
}

static void RunMessageLoop(void) {
    MSG msg;
    while (g_previewWnd && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_QUIT) {
            DebugLog(L"ShowModernPrintPreview: WM_QUIT observed in preview loop; exiting loop");
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
#endif /* RETROPAD_HAS_WINUI */

BOOL ShowModernPrintPreview(HWND parent, PrintRenderContext *ctx) {
    if (!ModernPreviewAvailable() || !ctx) {
        DebugLog(L"ShowModernPrintPreview: not available (bootstrap missing or ctx NULL)");
        return FALSE;
    }

#if defined(RETROPAD_HAS_WINUI)
    WindowsXamlManager xamlManager{ nullptr };
    DesktopWindowXamlSource xamlSource{ nullptr };
    PrintManager pm{ nullptr };
    event_token token{};
    bool tokenActive = false;

    auto Cleanup = [&]() {
        if (tokenActive && pm) {
            try { pm.PrintTaskRequested(token); } catch (...) {}
            tokenActive = false;
        }
        try { if (xamlSource) xamlSource.Close(); } catch (...) {}
        try { if (xamlManager) xamlManager.Close(); } catch (...) {}
    };

    try {
        init_apartment(apartment_type::single_threaded);
        g_ctx = ctx;
        const bool testMode = ctx->testMode ? true : false;
        DebugLog(L"ShowModernPrintPreview: WinUI branch compiled");
        DebugLog(L"ShowModernPrintPreview: apartment initialized");

        if (!CreatePreviewHostWindow(parent)) {
            DebugLog(L"ShowModernPrintPreview: CreatePreviewHostWindow failed");
            return FALSE;
        }
        DebugLog(L"ShowModernPrintPreview: preview host window created");

        xamlManager = WindowsXamlManager::InitializeForCurrentThread();
        DebugLog(L"ShowModernPrintPreview: Xaml manager initialized");
        auto interop = xamlSource.as<IDesktopWindowXamlSourceNative>();
        if (!xamlSource) {
            xamlSource = DesktopWindowXamlSource();
            interop = xamlSource.as<IDesktopWindowXamlSourceNative>();
        }
        HRESULT hr = interop ? interop->AttachToWindow(g_previewWnd) : E_NOINTERFACE;
        if (FAILED(hr)) {
            LogHr(L"ShowModernPrintPreview: AttachToWindow failed", hr);
            if (g_previewWnd) {
                DestroyWindow(g_previewWnd);
                g_previewWnd = NULL;
            }
            Cleanup();
            return FALSE;
        }

        HWND xamlHost = NULL;
        hr = interop->get_WindowHandle(&xamlHost);
        if (FAILED(hr) || !xamlHost) {
            LogHr(L"ShowModernPrintPreview: get_WindowHandle failed", hr);
            if (g_previewWnd) {
                DestroyWindow(g_previewWnd);
                g_previewWnd = NULL;
            }
            Cleanup();
            return FALSE;
        }
        UINT dpiX = 96;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            typedef UINT (WINAPI *GETDPIFORWINDOW)(HWND);
            auto pGetDpiForWindow = (GETDPIFORWINDOW)GetProcAddress(user32, "GetDpiForWindow");
            if (pGetDpiForWindow && g_previewWnd) {
                dpiX = pGetDpiForWindow(g_previewWnd);
            }
        }
        UINT dpiY = dpiX;
        HDC hdc = GetDC(g_previewWnd);
        if (hdc) {
            dpiY = (UINT)GetDeviceCaps(hdc, LOGPIXELSY);
            ReleaseDC(g_previewWnd, hdc);
        }

        double marginLeftPx = (ctx->marginsThousandths.left / 1000.0) * dpiX;
        double marginRightPx = (ctx->marginsThousandths.right / 1000.0) * dpiX;
        double marginTopPx = (ctx->marginsThousandths.top / 1000.0) * dpiY;
        double marginBottomPx = (ctx->marginsThousandths.bottom / 1000.0) * dpiY;
        double pageWidthPx = (8.5 * dpiX) - (marginLeftPx + marginRightPx);
        double pageHeightPx = (11.0 * dpiY) - (marginTopPx + marginBottomPx);
        if (pageWidthPx < 200) pageWidthPx = 200;
        if (pageHeightPx < 200) pageHeightPx = 200;

        const int hostWidth = (int)(pageWidthPx + marginLeftPx + marginRightPx + 64);
        const int hostHeight = (int)(pageHeightPx + marginTopPx + marginBottomPx + 64);
        SetWindowPos(xamlHost, NULL, 0, 0, hostWidth, hostHeight, SWP_NOACTIVATE | SWP_HIDEWINDOW);
        DebugLog(L"ShowModernPrintPreview: XAML island attached");

        // Basic preview UI
        Grid root;
        root.Width((double)hostWidth);
        root.Height((double)hostHeight);
        SolidColorBrush whiteBrush(Windows::UI::Color{0xFF, 0xFF, 0xFF, 0xFF});
        root.Background(whiteBrush);
        DebugLog(L"ShowModernPrintPreview: root container created");

        Border pageBorder;
        pageBorder.Background(whiteBrush);
        pageBorder.Width(pageWidthPx + marginLeftPx + marginRightPx);
        pageBorder.Height(pageHeightPx + marginTopPx + marginBottomPx);
        pageBorder.Padding(Thickness{ marginLeftPx, marginTopPx, marginRightPx, marginBottomPx });
        DebugLog(L"ShowModernPrintPreview: page border created");

        TextBlock pageText;
        pageText.TextWrapping(TextWrapping::Wrap);
        pageText.FontFamily(FontFamily(L"Consolas"));
        pageText.FontSize(12.0);
        pageText.Width(pageWidthPx);
        pageText.Text(ctx->text ? ctx->text : L"");
        DebugLog(L"ShowModernPrintPreview: TextBlock assigned");
        pageBorder.Child(pageText);

        ScrollViewer scroller;
        scroller.Content(pageBorder);
        scroller.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroller.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        scroller.HorizontalAlignment(HorizontalAlignment::Stretch);
        scroller.VerticalAlignment(VerticalAlignment::Stretch);
        scroller.Background(whiteBrush);
        scroller.BorderThickness(Thickness{0, 0, 0, 0});
        scroller.Margin(Thickness{0, 0, 0, 0});
        DebugLog(L"ShowModernPrintPreview: scroller created");

        auto children = root.Children();
        children.Append(scroller);
        xamlSource.Content(root);
        DebugLog(L"ShowModernPrintPreview: XAML content assigned");

        // Print pipeline: simple single-page XAML preview driven by PrintDocument.
        PrintDocument printDoc;
        UIElement previewPage = root;
        auto docSource = printDoc.DocumentSource();
        DebugLog(L"ShowModernPrintPreview: PrintDocument created");

        printDoc.Paginate([printDoc](auto const&, PaginateEventArgs const&) {
            printDoc.SetPreviewPageCount(1, PreviewPageCountType::Final);
        });

        printDoc.GetPreviewPage([printDoc, previewPage](auto const&, GetPreviewPageEventArgs const& args) {
            printDoc.SetPreviewPage(args.PageNumber(), previewPage);
        });

        printDoc.AddPages([printDoc, previewPage](auto const&, AddPagesEventArgs const&) {
            printDoc.AddPage(previewPage);
            printDoc.AddPagesComplete();
        });

        auto pmInterop = winrt::get_activation_factory<PrintManager, IPrintManagerInterop>();
        if (!pmInterop) {
            DebugLog(L"ShowModernPrintPreview: failed to get PrintManagerInterop factory");
            Cleanup();
            return FALSE;
        }

        hr = pmInterop->GetForWindow(g_previewWnd, winrt::guid_of<PrintManager>(), winrt::put_abi(pm));
        if (FAILED(hr)) {
            LogHr(L"ShowModernPrintPreview: GetForWindow failed", hr);
            Cleanup();
            return FALSE;
        }
        DebugLog(L"ShowModernPrintPreview: PrintManager acquired");

        token = pm.PrintTaskRequested([docSource](PrintManager const&, PrintTaskRequestedEventArgs const& args) {
            PrintTask task = args.Request().CreatePrintTask(L"retropad", [docSource](PrintTaskSourceRequestedArgs const& requestArgs) {
                requestArgs.SetSource(docSource);
            });
            task.Completed([](PrintTask const&, PrintTaskCompletedEventArgs const&) {
                if (g_previewWnd) {
                    PostMessageW(g_previewWnd, WM_CLOSE, 0, 0);
                }
            });
        });
        tokenActive = true;

        Windows::Foundation::IAsyncOperation<bool> showOp{ nullptr };
        hr = pmInterop->ShowPrintUIForWindowAsync(g_previewWnd, winrt::guid_of<Windows::Foundation::IAsyncOperation<bool>>(), winrt::put_abi(showOp));
        if (FAILED(hr)) {
            LogHr(L"ShowModernPrintPreview: ShowPrintUIForWindowAsync failed", hr);
            Cleanup();
            return FALSE;
        }
        DebugLog(L"ShowModernPrintPreview: awaiting Print UI");
        if (testMode) {
            DebugLog(L"ShowModernPrintPreview: scheduling auto-cancel (test mode)");
            std::thread([op = showOp]() mutable {
                Sleep(3000);
                op.Cancel();
            }).detach();
        }

        try {
            showOp.get();
            DebugLog(L"ShowModernPrintPreview: Print UI shown");
        } catch (const winrt::hresult_canceled&) {
            DebugLog(testMode ? L"ShowModernPrintPreview: Print UI auto-canceled for test" : L"ShowModernPrintPreview: Print UI canceled");
            if (!testMode) {
                Cleanup();
                throw;
            }
        }

        ShowWindow(g_previewWnd, SW_HIDE);
        UpdateWindow(g_previewWnd);
        if (testMode && g_previewWnd) {
            PostMessageW(g_previewWnd, WM_CLOSE, 0, 0);
        }
        RunMessageLoop();

        DebugLog(L"ShowModernPrintPreview: preview loop exited");
        Cleanup();
        if (g_previewWnd) {
            DestroyWindow(g_previewWnd);
            g_previewWnd = NULL;
        }
        g_ctx = NULL;
        return TRUE;
    } catch (const winrt::hresult_error& e) {
        LogHr(L"ShowModernPrintPreview: hresult_error", e.code());
        DebugLog(e.message().c_str());
        DebugLog(L"ShowModernPrintPreview: exception - preview failed");
        Cleanup();
        return FALSE;
    } catch (...) {
        DebugLog(L"ShowModernPrintPreview: unknown exception - preview failed");
        Cleanup();
        return FALSE;
    }
#else
    DebugLog(L"ShowModernPrintPreview: RETROPAD_HAS_WINUI not defined at build time");
    (void)parent;
    (void)ctx;
    return FALSE;
#endif
}
