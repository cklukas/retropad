#include <stddef.h>
#include <ocidl.h>
#include <PrintPreview.h>
#include <strsafe.h>

#include "print_dialog_callback.h"
#include "preview_target.h"
#include "rendering.h"

// Provided by print.c; holds the live context for the current print operation.
extern PrintRenderContext g_printContext;

#ifndef __IPrintDialogCallback2_INTERFACE_DEFINED__
#define __IPrintDialogCallback2_INTERFACE_DEFINED__

#ifndef IID_IPrintDialogCallback2
// Fallback IID for IPrintDialogCallback2; not present in older SDKs.
static const IID IID_IPrintDialogCallback2 = {0xf79cf4cf, 0x0ee0, 0x4a1e, {0x9f, 0x3d, 0x89, 0xe1, 0xd8, 0x68, 0xe9, 0xc5}};
#endif

typedef interface IPrintDialogCallback2 IPrintDialogCallback2;

typedef struct IPrintDialogCallback2Vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IPrintDialogCallback2 *This, REFIID riid, void **ppvObject);
    ULONG (STDMETHODCALLTYPE *AddRef)(IPrintDialogCallback2 *This);
    ULONG (STDMETHODCALLTYPE *Release)(IPrintDialogCallback2 *This);
    HRESULT (STDMETHODCALLTYPE *InitDone)(IPrintDialogCallback2 *This);
    HRESULT (STDMETHODCALLTYPE *SelectionChange)(IPrintDialogCallback2 *This);
    HRESULT (STDMETHODCALLTYPE *HandleMessage)(IPrintDialogCallback2 *This, HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT *pResult);
    HRESULT (STDMETHODCALLTYPE *PrinterChanged)(IPrintDialogCallback2 *This, LPCWSTR pszPrinterName);
} IPrintDialogCallback2Vtbl;

interface IPrintDialogCallback2 {
    CONST_VTBL IPrintDialogCallback2Vtbl *lpVtbl;
};

#endif // __IPrintDialogCallback2_INTERFACE_DEFINED__

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

static void DebugLogGuid(const WCHAR *prefix, REFIID riid) {
    WCHAR g[64];
    if (!StringFromGUID2(riid, g, ARRAYSIZE(g))) return;
    WCHAR buf[256];
    if (SUCCEEDED(StringCchPrintfW(buf, ARRAYSIZE(buf), L"%s %s", prefix ? prefix : L"GUID", g))) {
        DebugLog(buf);
    }
}

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

typedef struct PrintDialogCallbackImpl {
    IPrintDialogCallback callback;
    IPrintDialogCallback2 callback2;
    IObjectWithSite siteIface;
    LONG refCount;
    IUnknown *siteUnk;
    IPrintDialogServices *services;
    PreviewTarget *previewTarget;
    struct PrintRenderContext *ctx;
} PrintDialogCallbackImpl;

// Forward declaration for use before definition.
static HRESULT RefreshPreviewTarget(PrintDialogCallbackImpl *impl);

static PrintDialogCallbackImpl *FromCallback(IPrintDialogCallback *iface) {
    return (PrintDialogCallbackImpl *)iface;
}

static PrintDialogCallbackImpl *FromCallback2(IPrintDialogCallback2 *iface) {
    return (PrintDialogCallbackImpl *)((BYTE *)iface - offsetof(PrintDialogCallbackImpl, callback2));
}

static PrintDialogCallbackImpl *FromSite(IObjectWithSite *iface) {
    return (PrintDialogCallbackImpl *)((BYTE *)iface - offsetof(PrintDialogCallbackImpl, siteIface));
}

static HRESULT STDMETHODCALLTYPE Callback_QueryInterface(IPrintDialogCallback *self, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    DebugLogGuid(L"Callback QI for", riid);
    PrintDialogCallbackImpl *impl = FromCallback(self);
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IPrintDialogCallback)) {
        *ppv = self;
    } else if (IsEqualIID(riid, &IID_IPrintDialogCallback2)) {
        *ppv = &impl->callback2;
        impl->callback2.lpVtbl->AddRef(&impl->callback2);
        return S_OK;
    } else if (IsEqualIID(riid, &IID_IObjectWithSite)) {
        *ppv = &impl->siteIface;
        impl->siteIface.lpVtbl->AddRef(&impl->siteIface);
        return S_OK;
    } else if (IsEqualIID(riid, &IID_IPrintPreviewDxgiPackageTarget)) {
        if (SUCCEEDED(PrintDialogCallbackGetPreviewTarget(self, NULL)) && impl->previewTarget) {
            *ppv = PreviewTargetAsUnknown(impl->previewTarget);
            ((IUnknown *)(*ppv))->lpVtbl->AddRef((IUnknown *)*ppv);
            return S_OK;
        }
        return E_NOINTERFACE;
    } else {
        // Be permissive: return the extended callback for unknown IIDs so newer dialogs can still use us.
        *ppv = &impl->callback2;
        impl->callback2.lpVtbl->AddRef(&impl->callback2);
        return S_OK;
    }
    self->lpVtbl->AddRef(self);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE Callback_AddRef(IPrintDialogCallback *self) {
    PrintDialogCallbackImpl *impl = FromCallback(self);
    LONG r = InterlockedIncrement(&impl->refCount);
    DebugLog(L"Callback AddRef");
    return (ULONG)r;
}

static ULONG STDMETHODCALLTYPE Callback_Release(IPrintDialogCallback *self) {
    PrintDialogCallbackImpl *impl = FromCallback(self);
    LONG ref = InterlockedDecrement(&impl->refCount);
    DebugLog(L"Callback Release");
    if (ref == 0) {
        if (impl->services) {
            impl->services->lpVtbl->Release(impl->services);
        }
        if (impl->siteUnk) {
            impl->siteUnk->lpVtbl->Release(impl->siteUnk);
        }
        if (impl->previewTarget) {
            ReleasePreviewTarget(impl->previewTarget);
            impl->previewTarget = NULL;
        }
        HeapFree(GetProcessHeap(), 0, impl);
    }
    return (ULONG)ref;
}

static HRESULT STDMETHODCALLTYPE Callback_InitDone(IPrintDialogCallback *self) {
    PrintDialogCallbackImpl *impl = FromCallback(self);
    if (impl->ctx) {
        PrintDialogCallbackGetPreviewTarget(self, NULL);
    }
    DebugLog(L"Callback InitDone");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Callback_SelectionChange(IPrintDialogCallback *self) {
    PrintDialogCallbackImpl *impl = FromCallback(self);
    if (impl->ctx) {
        PrintDialogCallbackGetPreviewTarget(self, NULL);
    }
    DebugLog(L"Callback SelectionChange");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Callback_HandleMessage(IPrintDialogCallback *self, HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT *pResult) {
    (void)self;
    (void)hDlg;
    (void)uMsg;
    (void)wParam;
    (void)lParam;
    if (pResult) *pResult = 0;
    return S_FALSE; // let default handling continue
}

static HRESULT STDMETHODCALLTYPE Callback2_QueryInterface(IPrintDialogCallback2 *self, REFIID riid, void **ppv) {
    PrintDialogCallbackImpl *impl = FromCallback2(self);
    return Callback_QueryInterface(&impl->callback, riid, ppv);
}

static ULONG STDMETHODCALLTYPE Callback2_AddRef(IPrintDialogCallback2 *self) {
    PrintDialogCallbackImpl *impl = FromCallback2(self);
    return Callback_AddRef(&impl->callback);
}

static ULONG STDMETHODCALLTYPE Callback2_Release(IPrintDialogCallback2 *self) {
    PrintDialogCallbackImpl *impl = FromCallback2(self);
    return Callback_Release(&impl->callback);
}

static HRESULT STDMETHODCALLTYPE Callback2_InitDone(IPrintDialogCallback2 *self) {
    PrintDialogCallbackImpl *impl = FromCallback2(self);
    return Callback_InitDone(&impl->callback);
}

static HRESULT STDMETHODCALLTYPE Callback2_SelectionChange(IPrintDialogCallback2 *self) {
    PrintDialogCallbackImpl *impl = FromCallback2(self);
    return Callback_SelectionChange(&impl->callback);
}

static HRESULT STDMETHODCALLTYPE Callback2_HandleMessage(IPrintDialogCallback2 *self, HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT *pResult) {
    PrintDialogCallbackImpl *impl = FromCallback2(self);
    return Callback_HandleMessage(&impl->callback, hDlg, uMsg, wParam, lParam, pResult);
}

static HRESULT STDMETHODCALLTYPE Callback2_PrinterChanged(IPrintDialogCallback2 *self, LPCWSTR pszPrinterName) {
    (void)pszPrinterName;
    PrintDialogCallbackImpl *impl = FromCallback2(self);
    DebugLog(L"PrinterChanged called");
    if (impl->ctx) {
        RefreshPreviewTarget(impl);
    }
    return S_OK;
}

static const IPrintDialogCallbackVtbl g_callbackVtbl = {
    Callback_QueryInterface,
    Callback_AddRef,
    Callback_Release,
    Callback_InitDone,
    Callback_SelectionChange,
    Callback_HandleMessage
};

static const IPrintDialogCallback2Vtbl g_callback2Vtbl = {
    Callback2_QueryInterface,
    Callback2_AddRef,
    Callback2_Release,
    Callback2_InitDone,
    Callback2_SelectionChange,
    Callback2_HandleMessage,
    Callback2_PrinterChanged
};

static HRESULT STDMETHODCALLTYPE Site_QueryInterface(IObjectWithSite *self, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    DebugLogGuid(L"Site QI for", riid);
    PrintDialogCallbackImpl *impl = FromSite(self);
    return Callback_QueryInterface(&impl->callback, riid, ppv);
}

static ULONG STDMETHODCALLTYPE Site_AddRef(IObjectWithSite *self) {
    PrintDialogCallbackImpl *impl = FromSite(self);
    DebugLog(L"Site AddRef");
    return Callback_AddRef(&impl->callback);
}

static ULONG STDMETHODCALLTYPE Site_Release(IObjectWithSite *self) {
    PrintDialogCallbackImpl *impl = FromSite(self);
    DebugLog(L"Site Release");
    return Callback_Release(&impl->callback);
}

static HRESULT STDMETHODCALLTYPE Site_SetSite(IObjectWithSite *self, IUnknown *punk) {
    PrintDialogCallbackImpl *impl = FromSite(self);
    if (impl->services) {
        impl->services->lpVtbl->Release(impl->services);
        impl->services = NULL;
    }
    if (impl->siteUnk) {
        impl->siteUnk->lpVtbl->Release(impl->siteUnk);
        impl->siteUnk = NULL;
    }
    if (punk) {
        punk->lpVtbl->AddRef(punk);
        impl->siteUnk = punk;
        HRESULT hr = punk->lpVtbl->QueryInterface(punk, &IID_IPrintDialogServices, (void **)&impl->services);
        DebugLog(L"SetSite: site provided, services queried");
        if (SUCCEEDED(hr) && impl->services) {
            DebugLog(L"SetSite: services available");
            impl->ctx = &g_printContext;
            RefreshPreviewTarget(impl);
        } else {
            DebugLog(L"SetSite: no IPrintDialogServices");
        }
    } else {
        DebugLog(L"SetSite: cleared");
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Site_GetSite(IObjectWithSite *self, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    PrintDialogCallbackImpl *impl = FromSite(self);
    if (IsEqualIID(riid, &IID_IPrintDialogServices)) {
        if (impl->services) {
            impl->services->lpVtbl->AddRef(impl->services);
            *ppv = impl->services;
            DebugLog(L"GetSite: returning IPrintDialogServices");
            return S_OK;
        }
        DebugLog(L"GetSite: no IPrintDialogServices available");
        return E_NOINTERFACE;
    }
    if (IsEqualIID(riid, &IID_IPrintPreviewDxgiPackageTarget)) {
        if (SUCCEEDED(PrintDialogCallbackGetPreviewTarget(&impl->callback, NULL)) && impl->previewTarget) {
            DebugLog(L"GetSite: returning preview target");
            *ppv = PreviewTargetAsUnknown(impl->previewTarget);
            if (*ppv) {
                ((IUnknown *)(*ppv))->lpVtbl->AddRef((IUnknown *)*ppv);
                return S_OK;
            }
        }
        DebugLog(L"GetSite: preview target unavailable");
        return E_NOINTERFACE;
    }
    if (impl->siteUnk) {
        return impl->siteUnk->lpVtbl->QueryInterface(impl->siteUnk, riid, ppv);
    }
    return E_NOINTERFACE;
}

static const IObjectWithSiteVtbl g_siteVtbl = {
    Site_QueryInterface,
    Site_AddRef,
    Site_Release,
    Site_SetSite,
    Site_GetSite
};
HRESULT CreatePrintDialogCallback(IPrintDialogCallback **out) {
    if (!out) return E_POINTER;
    *out = NULL;
    PrintDialogCallbackImpl *impl = (PrintDialogCallbackImpl *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*impl));
    if (!impl) return E_OUTOFMEMORY;
    impl->callback.lpVtbl = (IPrintDialogCallbackVtbl *)&g_callbackVtbl;
    impl->callback2.lpVtbl = (IPrintDialogCallback2Vtbl *)&g_callback2Vtbl;
    impl->siteIface.lpVtbl = (IObjectWithSiteVtbl *)&g_siteVtbl;
    impl->refCount = 1;
    impl->services = NULL;
    impl->siteUnk = NULL;
    impl->previewTarget = NULL;
    impl->ctx = NULL;
    *out = &impl->callback;
    return S_OK;
}

HRESULT PrintDialogCallbackGetServices(IPrintDialogCallback *cb, IPrintDialogServices **outServices) {
    if (!outServices) return E_POINTER;
    *outServices = NULL;
    if (!cb) return E_POINTER;
    PrintDialogCallbackImpl *impl = FromCallback(cb);
    if (impl->services) {
        impl->services->lpVtbl->AddRef(impl->services);
        *outServices = impl->services;
        return S_OK;
    }
    return E_FAIL;
}

HRESULT PrintDialogCallbackCopyDevMode(IPrintDialogCallback *cb, HGLOBAL *outDevMode) {
    if (!outDevMode) return E_POINTER;
    *outDevMode = NULL;
    if (!cb) return E_POINTER;
    PrintDialogCallbackImpl *impl = FromCallback(cb);
    if (!impl->services) return E_FAIL;

    UINT cbSize = 0;
    HRESULT hr = impl->services->lpVtbl->GetCurrentDevMode(impl->services, NULL, &cbSize);
    if (FAILED(hr) || cbSize == 0) return hr;

    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, cbSize);
    if (!h) return E_OUTOFMEMORY;
    LPDEVMODEW dm = (LPDEVMODEW)GlobalLock(h);
    if (!dm) {
        GlobalFree(h);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    hr = impl->services->lpVtbl->GetCurrentDevMode(impl->services, dm, &cbSize);
    GlobalUnlock(h);
    if (FAILED(hr)) {
        GlobalFree(h);
        return hr;
    }
    *outDevMode = h;
    return S_OK;
}

static void ComputePageMetrics(DEVMODEW *dm, const WCHAR *printerName, FLOAT *outDpiX, FLOAT *outDpiY, FLOAT *outWidth, FLOAT *outHeight) {
    FLOAT dpiX = 96.0f, dpiY = 96.0f;
    FLOAT pageWidth = 816.0f;   // 8.5" * 96
    FLOAT pageHeight = 1056.0f; // 11" * 96

    HDC hdc = CreateDCW(NULL, printerName, NULL, dm);
    if (hdc) {
        int capDpiX = GetDeviceCaps(hdc, LOGPIXELSX);
        int capDpiY = GetDeviceCaps(hdc, LOGPIXELSY);
        if (capDpiX > 0) dpiX = (FLOAT)capDpiX;
        if (capDpiY > 0) dpiY = (FLOAT)capDpiY;

        int physWidth = GetDeviceCaps(hdc, PHYSICALWIDTH);
        int physHeight = GetDeviceCaps(hdc, PHYSICALHEIGHT);
        if (physWidth > 0 && physHeight > 0 && capDpiX > 0 && capDpiY > 0) {
            pageWidth = (FLOAT)physWidth * (96.0f / (FLOAT)capDpiX);
            pageHeight = (FLOAT)physHeight * (96.0f / (FLOAT)capDpiY);
        }
        DeleteDC(hdc);
    }

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
    }

    if (outDpiX) *outDpiX = dpiX;
    if (outDpiY) *outDpiY = dpiY;
    if (outWidth) *outWidth = pageWidth;
    if (outHeight) *outHeight = pageHeight;
}

static HRESULT RefreshPreviewTarget(PrintDialogCallbackImpl *impl) {
    if (!impl || !impl->ctx) return E_FAIL;
    if (!impl->previewTarget) {
        HRESULT hrCreate = CreatePreviewTarget(&impl->previewTarget);
        if (FAILED(hrCreate)) return hrCreate;
    }

    FLOAT dpiX = 96.0f, dpiY = 96.0f, pageWidth = 816.0f, pageHeight = 1056.0f;
    DEVMODEW *dm = NULL;
    WCHAR *printerName = NULL;

    if (impl->services) {
        DebugLog(L"RefreshPreviewTarget: services available");
        UINT cb = 0;
        if (impl->services->lpVtbl->GetCurrentDevMode(impl->services, NULL, &cb) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && cb > 0) {
            dm = (DEVMODEW *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cb);
            if (dm) {
                if (FAILED(impl->services->lpVtbl->GetCurrentDevMode(impl->services, dm, &cb))) {
                    HeapFree(GetProcessHeap(), 0, dm);
                    dm = NULL;
                }
            }
        }

        UINT cchPrinter = 0;
        if (impl->services->lpVtbl->GetCurrentPrinterName(impl->services, NULL, &cchPrinter) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && cchPrinter > 0) {
            printerName = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WCHAR) * cchPrinter);
            if (printerName) {
                if (FAILED(impl->services->lpVtbl->GetCurrentPrinterName(impl->services, printerName, &cchPrinter))) {
                    HeapFree(GetProcessHeap(), 0, printerName);
                    printerName = NULL;
                }
            }
        }
    }

    if (printerName) {
        ComputePageMetrics(dm, printerName, &dpiX, &dpiY, &pageWidth, &pageHeight);
        DebugLog(L"RefreshPreviewTarget: computed metrics from printer");
    } else {
        ComputePageMetrics(dm, L"", &dpiX, &dpiY, &pageWidth, &pageHeight);
        DebugLog(L"RefreshPreviewTarget: computed metrics fallback");
    }

    if (printerName) HeapFree(GetProcessHeap(), 0, printerName);
    if (dm) HeapFree(GetProcessHeap(), 0, dm);

    HRESULT hrSet = PreviewTargetSetRenderer(impl->previewTarget, impl->ctx, dpiX, dpiY, pageWidth, pageHeight);
    if (SUCCEEDED(hrSet)) {
        DebugLog(L"Preview target refreshed with context");
    } else {
        DebugLog(L"Preview target refresh failed");
    }
    return hrSet;
}

HRESULT PrintDialogCallbackGetPreviewTarget(IPrintDialogCallback *cb, PreviewTarget **outTarget) {
    if (outTarget) *outTarget = NULL;
    if (!cb) return E_POINTER;
    PrintDialogCallbackImpl *impl = FromCallback(cb);
    HRESULT hr = RefreshPreviewTarget(impl);
    if (FAILED(hr)) {
        DebugLog(L"GetPreviewTarget failed");
    }
    if (FAILED(hr)) return hr;
    if (outTarget) {
        *outTarget = impl->previewTarget;
        IUnknown *unk = PreviewTargetAsUnknown(impl->previewTarget);
        if (unk) {
            unk->lpVtbl->AddRef(unk);
        }
    }
    DebugLog(L"GetPreviewTarget succeeded");
    return S_OK;
}

HRESULT PrintDialogCallbackSetContext(IPrintDialogCallback *cb, struct PrintRenderContext *ctx) {
    if (!cb) return E_POINTER;
    PrintDialogCallbackImpl *impl = FromCallback(cb);
    impl->ctx = ctx;
    // If the site was already provided, refresh the preview target with the new context immediately.
    RefreshPreviewTarget(impl);
    return S_OK;
}
