#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <strsafe.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include <MddBootstrap.h>

#include "PrintPreviewWindow.h"
#include "rendering.h"

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

static void DebugLog(const WCHAR *msg) {
    WCHAR path[MAX_PATH];
    DWORD len = GetTempPathW(ARRAYSIZE(path), path);
    if (len == 0 || len >= ARRAYSIZE(path)) return;
    if (FAILED(StringCchCatW(path, ARRAYSIZE(path), L"retropad_preview.log"))) return;
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR line[512];
    StringCchPrintfW(line, ARRAYSIZE(line), L"[%02u:%02u:%02u.%03u] %s\r\n",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     msg ? msg : L"(null)");
    DWORD written = 0;
    WriteFile(h, line, (DWORD)(lstrlenW(line) * sizeof(WCHAR)), &written, NULL);
    CloseHandle(h);
}

static bool g_bootstrapReady = false;

BOOL InitWindowsAppRuntime(void) {
    if (g_bootstrapReady) return TRUE;

    PACKAGE_VERSION minVersion{};
    minVersion.Major = 1;
    minVersion.Minor = 8;

    DebugLog(L"InitWindowsAppRuntime: calling MddBootstrapInitialize2");
    HRESULT hr = MddBootstrapInitialize2(0x00010008, nullptr, minVersion, MddBootstrapInitializeOptions_OnNoMatch_ShowUI);
    if (FAILED(hr)) {
        WCHAR buf[128];
        StringCchPrintfW(buf, ARRAYSIZE(buf), L"InitWindowsAppRuntime failed hr=0x%08lX", hr);
        DebugLog(buf);
        return FALSE;
    }

    g_bootstrapReady = true;
    DebugLog(L"InitWindowsAppRuntime: success");
    return TRUE;
}

void ShutdownWindowsAppRuntime(void) {
    if (!g_bootstrapReady) return;
    MddBootstrapShutdown();
    g_bootstrapReady = false;
    DebugLog(L"ShutdownWindowsAppRuntime completed");
}

BOOL ModernPreviewAvailable(void) {
    if (!g_bootstrapReady) {
        InitWindowsAppRuntime();
    }
    return g_bootstrapReady;
}

#if __has_include(<winrt/Windows.UI.Xaml.Hosting.h>) && __has_include(<winrt/Windows.UI.Xaml.Controls.h>) && __has_include(<winrt/Windows.UI.Xaml.Media.h>) && __has_include(<winrt/Windows.UI.Xaml.Printing.h>) && __has_include(<winrt/Windows.Graphics.Printing.h>) && __has_include(<PrintManagerInterop.h>) && __has_include(<windows.ui.xaml.hosting.desktopwindowxamlsource.h>)
#define RETROPAD_HAS_WINUI 1
#include <unknwn.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Printing.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Printing.h>
#include <PrintManagerInterop.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>

using namespace winrt;
using namespace Windows::UI;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Hosting;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Printing;
using namespace Windows::Graphics::Printing;
#else
#pragma message("WinUI headers missing - modern print preview disabled")
#endif

#if defined(RETROPAD_HAS_WINUI)
struct DrawOp {
    double x;
    double y;
    std::wstring text;
};

struct PageVisual {
    int width;
    int height;
    std::vector<DrawOp> draws;
};

struct PreviewState {
    std::wstring text;
    PrintRenderContext ctx{};
    PrintPageDescription pageDesc{};
    std::vector<PageVisual> pages;
    std::vector<UIElement> pageElements;
    double fontSize{12.0};
    std::wstring fontFamily{L"Consolas"};
    bool testMode{false};
};

static HWND g_previewWnd = nullptr;
static int ToThousandths(double px, double dpi) {
    return static_cast<int>(std::lround((px / dpi) * 1000.0));
}

class PreviewRenderTarget {
public:
    PreviewRenderTarget(const PrintPageDescription &desc, double fontSize, const std::wstring &fontFamily) {
        base.ops = &s_ops;
        base.userData = this;
        Initialize(desc, fontSize, fontFamily);
    }

    ~PreviewRenderTarget() {
        Cleanup();
    }

    bool Ready() const { return ready; }

    PrintRenderTarget base{};
    PrintMetrics metrics{};
    int pageWidth{0};
    int pageHeight{0};
    std::vector<PageVisual> pages;

private:
    HDC hdc{nullptr};
    HFONT hFont{nullptr};
    HFONT oldFont{nullptr};
    PageVisual *current{nullptr};
    bool ready{false};

    void Cleanup() {
        if (oldFont && hdc) {
            SelectObject(hdc, oldFont);
        }
        if (hFont) DeleteObject(hFont);
        if (hdc) DeleteDC(hdc);
        hdc = nullptr;
        hFont = nullptr;
        oldFont = nullptr;
        ready = false;
        current = nullptr;
    }

    void Initialize(const PrintPageDescription &desc, double fontSize, const std::wstring &fontFamily) {
        Cleanup();
        hdc = CreateCompatibleDC(NULL);
        // Use DIPs (96) for preview composition to align with PrintPageDescription PageSize units.
        int dpiX = 96;
        int dpiY = 96;
        LOGFONTW lf{};
        lf.lfHeight = -MulDiv((int)std::lround(fontSize), dpiY, 72);
        lf.lfWeight = FW_NORMAL;
        lf.lfQuality = CLEARTYPE_QUALITY;
        StringCchCopyW(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), fontFamily.c_str());
        hFont = CreateFontIndirectW(&lf);
        if (hFont && hdc) {
            oldFont = (HFONT)SelectObject(hdc, hFont);
        }
        TEXTMETRICW tm{};
        if (!hdc || !hFont || !GetTextMetricsW(hdc, &tm)) {
            Cleanup();
            return;
        }

        metrics.lineHeight = tm.tmHeight + tm.tmExternalLeading;
        metrics.averageCharWidth = tm.tmAveCharWidth;
        metrics.dpiX = dpiX;
        metrics.dpiY = dpiY;
        pageWidth = (int)std::lround(desc.PageSize.Width > 0 ? desc.PageSize.Width : 816.0);   // 8.5" at 96 DPI
        pageHeight = (int)std::lround(desc.PageSize.Height > 0 ? desc.PageSize.Height : 1056.0); // 11" at 96 DPI
        ready = (pageWidth > 0 && pageHeight > 0);
    }

    static BOOL GetMetrics(void *userData, PrintMetrics *outMetrics) {
        if (!userData || !outMetrics) return FALSE;
        auto *self = static_cast<PreviewRenderTarget *>(userData);
        *outMetrics = self->metrics;
        return TRUE;
    }

    static BOOL GetPageSize(void *userData, int *w, int *h) {
        if (!userData || !w || !h) return FALSE;
        auto *self = static_cast<PreviewRenderTarget *>(userData);
        *w = self->pageWidth;
        *h = self->pageHeight;
        return TRUE;
    }

    static BOOL BeginDocument(void *userData, const WCHAR *, int) {
        if (!userData) return FALSE;
        auto *self = static_cast<PreviewRenderTarget *>(userData);
        self->pages.clear();
        self->current = nullptr;
        return TRUE;
    }

    static BOOL EndDocument(void *userData) {
        (void)userData;
        return TRUE;
    }

    static BOOL AbortDocument(void *userData) {
        if (!userData) return FALSE;
        auto *self = static_cast<PreviewRenderTarget *>(userData);
        self->pages.clear();
        self->current = nullptr;
        return TRUE;
    }

    static BOOL BeginPage(void *userData, int) {
        if (!userData) return FALSE;
        auto *self = static_cast<PreviewRenderTarget *>(userData);
        PageVisual page{self->pageWidth, self->pageHeight, {}};
        self->pages.push_back(std::move(page));
        self->current = &self->pages.back();
        return TRUE;
    }

    static BOOL EndPage(void *userData) {
        if (!userData) return FALSE;
        auto *self = static_cast<PreviewRenderTarget *>(userData);
        self->current = nullptr;
        return TRUE;
    }

    static BOOL DrawText(void *userData, int x, int y, const WCHAR *text, int length) {
        if (!userData || !text) return FALSE;
        auto *self = static_cast<PreviewRenderTarget *>(userData);
        if (!self->current) return FALSE;
        DrawOp op;
        op.x = static_cast<double>(x);
        op.y = static_cast<double>(y);
        op.text.assign(text, length);
        self->current->draws.push_back(std::move(op));
        return TRUE;
    }

    static BOOL MeasureText(void *userData, const WCHAR *text, int length, SIZE *size) {
        if (!userData || !text || !size) return FALSE;
        auto *self = static_cast<PreviewRenderTarget *>(userData);
        if (!self->hdc) return FALSE;
        if (GetTextExtentPoint32W(self->hdc, text, length, size)) {
            return TRUE;
        }
        size->cx = self->metrics.averageCharWidth * length;
        size->cy = self->metrics.lineHeight;
        return TRUE;
    }

    static const PrintRenderTargetOps s_ops;
};

const PrintRenderTargetOps PreviewRenderTarget::s_ops = {
    PreviewRenderTarget::GetMetrics,
    PreviewRenderTarget::GetPageSize,
    PreviewRenderTarget::BeginDocument,
    PreviewRenderTarget::EndDocument,
    PreviewRenderTarget::AbortDocument,
    PreviewRenderTarget::BeginPage,
    PreviewRenderTarget::EndPage,
    PreviewRenderTarget::DrawText,
    PreviewRenderTarget::MeasureText,
};

static void LogHr(const WCHAR *prefix, HRESULT hr) {
    WCHAR buf[128];
    StringCchPrintfW(buf, ARRAYSIZE(buf), L"%s hr=0x%08lX", prefix ? prefix : L"hr", hr);
    DebugLog(buf);
}

static LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)wParam;
    (void)lParam;
    if (msg == WM_DESTROY) {
        g_previewWnd = nullptr;
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
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"Print Preview",
        WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 720,
        parent, NULL, wc.hInstance, NULL);
    return g_previewWnd != NULL;
}

static void RunMessageLoop(void) {
    MSG msg;
    while (g_previewWnd && GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static PrintRenderContext BuildAdjustedContext(const PreviewState &state) {
    PrintRenderContext ctx = state.ctx;
    // PrintPageDescription PageSize/ImageableRect are in DIPs (96 DPI), not device DPI.
    const double dpiDip = 96.0;
    double hardwareLeft = state.pageDesc.ImageableRect.X;
    double hardwareTop = state.pageDesc.ImageableRect.Y;
    double hardwareRight = state.pageDesc.PageSize.Width - state.pageDesc.ImageableRect.X - state.pageDesc.ImageableRect.Width;
    double hardwareBottom = state.pageDesc.PageSize.Height - state.pageDesc.ImageableRect.Y - state.pageDesc.ImageableRect.Height;

    const auto safeMax = [dpiDip](int current, double px) -> int {
        return std::max<int>(current, ToThousandths(px, dpiDip));
    };

    int userLeft = ctx.marginsThousandths.left;
    int userRight = ctx.marginsThousandths.right;
    int userTop = ctx.marginsThousandths.top;
    int userBottom = ctx.marginsThousandths.bottom;

    int adjustedLeft = safeMax(userLeft, hardwareLeft);
    int adjustedRight = safeMax(userRight, hardwareRight);
    // If the user asked for symmetric margins, keep them symmetric even when hardware adds a bias.
    if (userLeft == userRight) {
        int unified = std::max(adjustedLeft, adjustedRight);
        adjustedLeft = adjustedRight = unified;
    }

    ctx.marginsThousandths.left = adjustedLeft;
    ctx.marginsThousandths.right = adjustedRight;
    ctx.marginsThousandths.top = safeMax(userTop, hardwareTop);
    ctx.marginsThousandths.bottom = safeMax(userBottom, hardwareBottom);

    WCHAR buf[200];
    StringCchPrintfW(buf, ARRAYSIZE(buf),
        L"Adjusted margins (thousandths): L=%d T=%d R=%d B=%d, hardware px: L=%.1f T=%.1f R=%.1f B=%.1f",
        ctx.marginsThousandths.left, ctx.marginsThousandths.top,
        ctx.marginsThousandths.right, ctx.marginsThousandths.bottom,
        hardwareLeft, hardwareTop, hardwareRight, hardwareBottom);
    DebugLog(buf);
    return ctx;
}

static std::vector<PageVisual> GeneratePages(PreviewState &state) {
    PreviewRenderTarget target(state.pageDesc, state.fontSize, state.fontFamily);
    if (!target.Ready()) {
        WCHAR buf[160];
        StringCchPrintfW(buf, ARRAYSIZE(buf),
                         L"GeneratePages: not ready (PageSize %.2f x %.2f, Dpi %u x %u)",
                         state.pageDesc.PageSize.Width, state.pageDesc.PageSize.Height,
                         state.pageDesc.DpiX, state.pageDesc.DpiY);
        DebugLog(buf);
        return {};
    }

    PrintRenderContext ctx = BuildAdjustedContext(state);
    WCHAR buf[160];
    StringCchPrintfW(buf, ARRAYSIZE(buf),
                     L"GeneratePages: rendering with PageSize %.2f x %.2f, Dpi %u x %u",
                     state.pageDesc.PageSize.Width, state.pageDesc.PageSize.Height,
                     state.pageDesc.DpiX, state.pageDesc.DpiY);
    DebugLog(buf);
    RenderDocument(&ctx, &target.base);
    StringCchPrintfW(buf, ARRAYSIZE(buf), L"GeneratePages: produced %u pages", (UINT)target.pages.size());
    DebugLog(buf);
    return std::move(target.pages);
}

static UIElement BuildPageElement(const PageVisual &page, const PreviewState &state) {
    Border border;
    double width = state.pageDesc.PageSize.Width;
    double height = state.pageDesc.PageSize.Height;
    border.Width(width);
    border.Height(height);
    auto white = SolidColorBrush(Colors::White());
    border.Background(white);
    border.BorderBrush(SolidColorBrush(Color{0xFF, 0xD8, 0xD8, 0xD8}));
    border.BorderThickness(Thickness{1, 1, 1, 1});

    Canvas canvas;
    canvas.Width(width);
    canvas.Height(height);
    canvas.Background(white);

    for (const auto &op : page.draws) {
        TextBlock tb;
        tb.Text(op.text);
        tb.FontFamily(FontFamily(state.fontFamily));
        tb.FontSize(state.fontSize);
        tb.TextWrapping(TextWrapping::NoWrap);
        Canvas::SetLeft(tb, op.x);
        Canvas::SetTop(tb, op.y);
        canvas.Children().Append(tb);
    }

    border.Child(canvas);
    return border;
}

BOOL ShowModernPrintPreview(HWND parent, PrintRenderContext *ctx) {
    if (!ModernPreviewAvailable() || !ctx) {
        DebugLog(L"ShowModernPrintPreview: bootstrap unavailable or ctx NULL");
        return FALSE;
    }

    WindowsXamlManager xamlManager{nullptr};
    DesktopWindowXamlSource xamlSource{nullptr};
    PrintManager pm{nullptr};
    event_token token{};
    bool tokenActive = false;

    auto Cleanup = [&]() {
        if (tokenActive) {
            try { pm.PrintTaskRequested(token); } catch (...) {}
            tokenActive = false;
        }
        try { xamlSource.Close(); } catch (...) {}
        try { xamlManager.Close(); } catch (...) {}
        if (g_previewWnd) {
            DestroyWindow(g_previewWnd);
            g_previewWnd = nullptr;
        }
    };

    try {
        init_apartment(apartment_type::single_threaded);
        DebugLog(L"ShowModernPrintPreview: apartment initialized");

        if (!CreatePreviewHostWindow(parent)) {
            DebugLog(L"ShowModernPrintPreview: CreatePreviewHostWindow failed");
            return FALSE;
        }

        xamlManager = WindowsXamlManager::InitializeForCurrentThread();
        DebugLog(L"ShowModernPrintPreview: Xaml manager initialized");

        xamlSource = DesktopWindowXamlSource();
        auto interop = xamlSource ? xamlSource.as<IDesktopWindowXamlSourceNative>() : nullptr;
        HRESULT hrAttach = interop ? interop->AttachToWindow(g_previewWnd) : E_NOINTERFACE;
        if (FAILED(hrAttach)) {
            LogHr(L"AttachToWindow failed", hrAttach);
            Cleanup();
            return FALSE;
        }
        DebugLog(L"ShowModernPrintPreview: XAML island attached");
        HWND xamlHost = nullptr;
        if (interop && SUCCEEDED(interop->get_WindowHandle(&xamlHost)) && xamlHost) {
            SetWindowPos(xamlHost, NULL, 0, 0, 1, 1, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }

        Grid hostRoot;
        hostRoot.Background(SolidColorBrush(Color{0xFF, 0xF3, 0xF3, 0xF3}));
        xamlSource.Content(hostRoot);

        auto previewState = std::make_shared<PreviewState>();
        previewState->text = ctx->text ? ctx->text : L"";
        previewState->ctx = *ctx;
        previewState->ctx.text = previewState->text.c_str();
        previewState->testMode = ctx->testMode ? true : false;

        PrintDocument printDoc;
        auto docSource = printDoc.DocumentSource();
        DebugLog(L"ShowModernPrintPreview: PrintDocument created");
        printDoc.Paginate([previewState, printDoc](auto const &, PaginateEventArgs const &args) {
            previewState->pageDesc = args.PrintTaskOptions().GetPageDescription(0);
            previewState->pages = GeneratePages(*previewState);
            previewState->pageElements.clear();
            for (const auto &page : previewState->pages) {
                previewState->pageElements.push_back(BuildPageElement(page, *previewState));
            }
            int count = static_cast<int>(previewState->pageElements.size());
            if (count == 0) {
                count = 1;
            }
            printDoc.SetPreviewPageCount(count, PreviewPageCountType::Final);
            WCHAR buf[64];
            StringCchPrintfW(buf, ARRAYSIZE(buf), L"Paginate: page count %d", count);
            DebugLog(buf);
        });

        printDoc.GetPreviewPage([previewState, printDoc](auto const &, GetPreviewPageEventArgs const &args) {
            int index = args.PageNumber() - 1;
            if (index >= 0 && index < (int)previewState->pageElements.size()) {
                printDoc.SetPreviewPage(args.PageNumber(), previewState->pageElements[index]);
            }
        });

        printDoc.AddPages([previewState, printDoc](auto const &, AddPagesEventArgs const &) {
            if (previewState->pageElements.empty()) {
                // Create a blank page so the print UI stays responsive even on empty documents.
                PageVisual blank{(int)std::lround(previewState->pageDesc.PageSize.Width),
                                 (int)std::lround(previewState->pageDesc.PageSize.Height),
                                 {}};
                previewState->pageElements.push_back(BuildPageElement(blank, *previewState));
            }
            for (auto const &element : previewState->pageElements) {
                printDoc.AddPage(element);
            }
            printDoc.AddPagesComplete();
            WCHAR buf[64];
            StringCchPrintfW(buf, ARRAYSIZE(buf), L"AddPages: submitted %u pages", (UINT)previewState->pageElements.size());
            DebugLog(buf);
        });

        auto pmInterop = winrt::get_activation_factory<PrintManager, IPrintManagerInterop>();
        HRESULT hr = pmInterop ? pmInterop->GetForWindow(parent, winrt::guid_of<PrintManager>(), winrt::put_abi(pm)) : E_NOINTERFACE;
        if (FAILED(hr)) {
            LogHr(L"GetForWindow failed", hr);
            Cleanup();
            return FALSE;
        }
        DebugLog(L"ShowModernPrintPreview: PrintManager acquired");

        token = pm.PrintTaskRequested([docSource](PrintManager const &, PrintTaskRequestedEventArgs const &args) {
            PrintTask task = args.Request().CreatePrintTask(L"retropad", [docSource](PrintTaskSourceRequestedArgs const &requestArgs) {
                requestArgs.SetSource(docSource);
            });
            task.Completed([](PrintTask const &, PrintTaskCompletedEventArgs const &) {
                if (g_previewWnd) {
                    PostMessageW(g_previewWnd, WM_CLOSE, 0, 0);
                }
            });
        });
        tokenActive = true;

        Windows::Foundation::IAsyncOperation<bool> showOp{nullptr};
        hr = pmInterop->ShowPrintUIForWindowAsync(parent, winrt::guid_of<Windows::Foundation::IAsyncOperation<bool>>(), winrt::put_abi(showOp));
        if (FAILED(hr)) {
            LogHr(L"ShowPrintUIForWindowAsync failed", hr);
            Cleanup();
            return FALSE;
        }
        DebugLog(L"ShowModernPrintPreview: awaiting Print UI");

        if (previewState->testMode) {
            std::thread([op = showOp]() mutable {
                Sleep(2000);
                op.Cancel();
            }).detach();
        }

        try {
            showOp.get();
            DebugLog(L"ShowModernPrintPreview: Print UI shown");
        } catch (const winrt::hresult_canceled &) {
            DebugLog(previewState->testMode ? L"Print UI auto-canceled (test mode)" : L"Print UI canceled");
            Cleanup();
            return FALSE;
        }

        ShowWindow(g_previewWnd, SW_HIDE);
        UpdateWindow(g_previewWnd);
        RunMessageLoop();

        Cleanup();
        return !previewState->pageElements.empty();
    } catch (const winrt::hresult_error &e) {
        LogHr(L"ShowModernPrintPreview: hresult_error", e.code());
        DebugLog(e.message().c_str());
        Cleanup();
        return FALSE;
    } catch (...) {
        DebugLog(L"ShowModernPrintPreview: unknown exception");
        Cleanup();
        return FALSE;
    }
}
#else
BOOL ShowModernPrintPreview(HWND parent, PrintRenderContext *ctx) {
    (void)parent;
    (void)ctx;
    DebugLog(L"ShowModernPrintPreview: RETROPAD_HAS_WINUI not defined");
    return FALSE;
}
#endif
