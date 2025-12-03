#include <windows.h>
#include <stddef.h>
#include <ocidl.h>
#include <PrintPreview.h>
#include <strsafe.h>

#include "print_dialog_callback.h"
#include "preview_target.h"
#include "rendering.h"

extern PrintRenderContext g_printContext;

/* IPrintDocumentPackageTarget is not always available in older headers; define it here. */
static const IID IID_IPrintDocumentPackageTarget =
    {0x1a7e1c42,0x9e6c,0x429b,{0x97,0x0b,0x34,0x84,0x67,0x1e,0x47,0x2a}};

typedef interface IPrintDocumentPackageTarget IPrintDocumentPackageTarget;

typedef struct IPrintDocumentPackageTargetVtbl
{
    HRESULT (STDMETHODCALLTYPE* QueryInterface)(IPrintDocumentPackageTarget* This, REFIID riid, void** ppvObject);
    ULONG   (STDMETHODCALLTYPE* AddRef)(IPrintDocumentPackageTarget* This);
    ULONG   (STDMETHODCALLTYPE* Release)(IPrintDocumentPackageTarget* This);
    HRESULT (STDMETHODCALLTYPE* GetPackageTargetTypes)(IPrintDocumentPackageTarget* This, UINT32* count, GUID** targetTypes);
    HRESULT (STDMETHODCALLTYPE* GetPackageTarget)(IPrintDocumentPackageTarget* This, const GUID* guidTargetType, REFIID riid, void** ppvTarget);
    HRESULT (STDMETHODCALLTYPE* Cancel)(IPrintDocumentPackageTarget* This);
    HRESULT (STDMETHODCALLTYPE* GetPackageStatus)(IPrintDocumentPackageTarget* This);
} IPrintDocumentPackageTargetVtbl;

interface IPrintDocumentPackageTarget {
    CONST_VTBL IPrintDocumentPackageTargetVtbl* lpVtbl;
};

#ifndef __IPrintDialogCallback2_INTERFACE_DEFINED__
#define __IPrintDialogCallback2_INTERFACE_DEFINED__
static const IID IID_IPrintDialogCallback2 = {0xf79cf4cf,0x0ee0,0x4a1e,{0x9f,0x3d,0x89,0xe1,0xd8,0x68,0xe9,0xc5}};

typedef interface IPrintDialogCallback2 IPrintDialogCallback2;

typedef struct IPrintDialogCallback2Vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IPrintDialogCallback2 *This, REFIID riid, void **ppvObject);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IPrintDialogCallback2 *This);
    ULONG   (STDMETHODCALLTYPE *Release)(IPrintDialogCallback2 *This);
    HRESULT (STDMETHODCALLTYPE *InitDone)(IPrintDialogCallback2 *This);
    HRESULT (STDMETHODCALLTYPE *SelectionChange)(IPrintDialogCallback2 *This);
    HRESULT (STDMETHODCALLTYPE *HandleMessage)(IPrintDialogCallback2 *This, HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT *pResult);
    HRESULT (STDMETHODCALLTYPE *PrinterChanged)(IPrintDialogCallback2 *This, LPCWSTR pszPrinterName);
    HRESULT (STDMETHODCALLTYPE *SetPrintTicket)(IPrintDialogCallback2 *This, IPrintDocumentPackageTarget *pTicket);
} IPrintDialogCallback2Vtbl;

interface IPrintDialogCallback2 {
    CONST_VTBL IPrintDialogCallback2Vtbl *lpVtbl;
};
#endif

static void DebugLog(const WCHAR *msg)
{
    WCHAR path[MAX_PATH];
    DWORD len = GetTempPathW(ARRAYSIZE(path), path);
    if (len == 0 || len >= ARRAYSIZE(path)) return;
    StringCchCatW(path, ARRAYSIZE(path), L"retropad_preview.log");

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ|FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st; GetLocalTime(&st);
    WCHAR line[512];
    StringCchPrintfW(line, ARRAYSIZE(line),
        L"[%02u:%02u:%02u.%03u] %s\r\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        msg ? msg : L"(null)");

    DWORD written;
    WriteFile(h, line, (DWORD)(lstrlenW(line)*sizeof(WCHAR)), &written, NULL);
    CloseHandle(h);
}

static void DebugLogGuid(const WCHAR *prefix, REFIID riid)
{
    WCHAR g[64];
    if (StringFromGUID2(riid, g, ARRAYSIZE(g)))
    {
        WCHAR buf[256];
        StringCchPrintfW(buf, ARRAYSIZE(buf), L"%s %s", prefix, g);
        DebugLog(buf);
    }
}

static void LogIID(REFIID riid, const WCHAR *context)
{
    WCHAR g[64];
    if (StringFromGUID2(riid, g, ARRAYSIZE(g)))
    {
        WCHAR buf[256];
        StringCchPrintfW(buf, ARRAYSIZE(buf), L"QI: %s -> %s", g, context ? context : L"(null)");
        DebugLog(buf);
    }
}

/* Forward vtable declarations */
static IPrintDialogCallbackVtbl callback_vtbl;
static IPrintDialogCallback2Vtbl callback2_vtbl;
static IPrintDocumentPackageTargetVtbl packageTarget_vtbl;
static IObjectWithSiteVtbl site_vtbl;

typedef struct PrintDialogCallbackImpl
{
    IPrintDialogCallback2   callback2;      // MUST be first for modern dialog
    IPrintDialogCallback    callback;       // legacy interface, forwards to callback2
    IPrintDocumentPackageTarget packageTarget;
    IObjectWithSite         site;
    LONG                    refCount;
    IUnknown               *siteUnk;
    IPrintDialogServices   *services;
    PreviewTarget          *previewTarget;
    PrintRenderContext     *ctx;
} PrintDialogCallbackImpl;

static PrintDialogCallbackImpl *ImplFromCb(IPrintDialogCallback *cb)
{
    if (!cb) return NULL;
    if (cb->lpVtbl == (IPrintDialogCallbackVtbl*)&callback_vtbl)
        return (PrintDialogCallbackImpl*)((BYTE*)cb - offsetof(PrintDialogCallbackImpl, callback));
    return (PrintDialogCallbackImpl*)((BYTE*)cb - offsetof(PrintDialogCallbackImpl, callback2));
}

static PrintDialogCallbackImpl *ImplFromCb2(IPrintDialogCallback2 *cb2)
{
    return (PrintDialogCallbackImpl*)((BYTE*)cb2 - offsetof(PrintDialogCallbackImpl, callback2));
}

static HRESULT RefreshPreviewTarget(PrintDialogCallbackImpl *impl);
static ULONG STDMETHODCALLTYPE cb2_AddRef(IPrintDialogCallback2 *This);
static ULONG STDMETHODCALLTYPE cb2_Release(IPrintDialogCallback2 *This);

/* --------------------------------------------------------------------- */
/* IPrintDialogCallback2 vtable                                          */
/* --------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE cb2_QueryInterface(IPrintDialogCallback2 *This, REFIID riid, void **ppv)
{
    PrintDialogCallbackImpl *impl = ImplFromCb2(This);

    if (!ppv) return E_POINTER;
    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IPrintDialogCallback2))
    {
        *ppv = &impl->callback2;
        LogIID(riid, L"IPrintDialogCallback2");
        cb2_AddRef(This);
        return S_OK;
    }
    if (IsEqualIID(riid, &IID_IPrintDialogCallback))
    {
        *ppv = &impl->callback;
        LogIID(riid, L"IPrintDialogCallback");
        cb2_AddRef(This);
        return S_OK;
    }
    if (IsEqualIID(riid, &IID_IPrintDocumentPackageTarget))
    {
        *ppv = &impl->packageTarget;
        LogIID(riid, L"IPrintDocumentPackageTarget");
        cb2_AddRef(This);
        return S_OK;
    }
    if (IsEqualIID(riid, &IID_IObjectWithSite))
    {
        *ppv = &impl->site;
        LogIID(riid, L"IObjectWithSite");
        cb2_AddRef(This);
        return S_OK;
    }

    LogIID(riid, L"UNKNOWN -> E_NOINTERFACE");
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE cb2_AddRef(IPrintDialogCallback2 *This)
{
    PrintDialogCallbackImpl *impl = ImplFromCb2(This);
    LONG r = InterlockedIncrement(&impl->refCount);
    DebugLog(L"AddRef");
    return (ULONG)r;
}

static ULONG STDMETHODCALLTYPE cb2_Release(IPrintDialogCallback2 *This)
{
    PrintDialogCallbackImpl *impl = ImplFromCb2(This);
    LONG r = InterlockedDecrement(&impl->refCount);
    DebugLog(L"Release");
    if (r == 0)
    {
        if (impl->services)  impl->services->lpVtbl->Release(impl->services);
        if (impl->siteUnk)    impl->siteUnk->lpVtbl->Release(impl->siteUnk);
        if (impl->previewTarget) ReleasePreviewTarget(impl->previewTarget);
        HeapFree(GetProcessHeap(), 0, impl);
    }
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE cb2_InitDone(IPrintDialogCallback2 *This)
{
    PrintDialogCallbackImpl *impl = ImplFromCb2(This);
    DebugLog(L"InitDone");
    if (impl->ctx) RefreshPreviewTarget(impl);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cb2_SelectionChange(IPrintDialogCallback2 *This)
{
    PrintDialogCallbackImpl *impl = ImplFromCb2(This);
    DebugLog(L"SelectionChange");
    if (impl->ctx) RefreshPreviewTarget(impl);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cb2_HandleMessage(IPrintDialogCallback2 *This,
    HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT *pResult)
{
    (void)This; (void)hDlg; (void)uMsg; (void)wParam; (void)lParam;
    if (pResult) *pResult = 0;
    return S_FALSE;
}

static HRESULT STDMETHODCALLTYPE cb2_PrinterChanged(IPrintDialogCallback2 *This, LPCWSTR pszPrinterName)
{
    (void)pszPrinterName;
    PrintDialogCallbackImpl *impl = ImplFromCb2(This);
    DebugLog(L"PrinterChanged");
    if (impl->ctx) RefreshPreviewTarget(impl);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cb2_SetPrintTicket(IPrintDialogCallback2 *This, IPrintDocumentPackageTarget *pTicket)
{
    (void)This;
    (void)pTicket;
    DebugLog(L"SetPrintTicket called — preview should now work");
    return E_NOTIMPL;   // or S_OK - both work
}

/* Base IPrintDialogCallback vtable that forwards to the v2 implementation. */
static HRESULT STDMETHODCALLTYPE cb_QueryInterface(IPrintDialogCallback *This, REFIID riid, void **ppv)
{
    PrintDialogCallbackImpl *impl = ImplFromCb(This);
    return cb2_QueryInterface(&impl->callback2, riid, ppv);
}

static ULONG STDMETHODCALLTYPE cb_AddRef(IPrintDialogCallback *This)
{
    PrintDialogCallbackImpl *impl = ImplFromCb(This);
    return cb2_AddRef(&impl->callback2);
}

static ULONG STDMETHODCALLTYPE cb_Release(IPrintDialogCallback *This)
{
    PrintDialogCallbackImpl *impl = ImplFromCb(This);
    return cb2_Release(&impl->callback2);
}

static HRESULT STDMETHODCALLTYPE cb_InitDone(IPrintDialogCallback *This)
{
    PrintDialogCallbackImpl *impl = ImplFromCb(This);
    return cb2_InitDone(&impl->callback2);
}

static HRESULT STDMETHODCALLTYPE cb_SelectionChange(IPrintDialogCallback *This)
{
    PrintDialogCallbackImpl *impl = ImplFromCb(This);
    return cb2_SelectionChange(&impl->callback2);
}

static HRESULT STDMETHODCALLTYPE cb_HandleMessage(IPrintDialogCallback *This, HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT *pResult)
{
    PrintDialogCallbackImpl *impl = ImplFromCb(This);
    return cb2_HandleMessage(&impl->callback2, hDlg, uMsg, wParam, lParam, pResult);
}

static IPrintDialogCallback2Vtbl callback2_vtbl =
{
    cb2_QueryInterface,
    cb2_AddRef,
    cb2_Release,
    cb2_InitDone,
    cb2_SelectionChange,
    cb2_HandleMessage,
    cb2_PrinterChanged,
    cb2_SetPrintTicket
};

static IPrintDialogCallbackVtbl callback_vtbl =
{
    cb_QueryInterface,
    cb_AddRef,
    cb_Release,
    cb_InitDone,
    cb_SelectionChange,
    cb_HandleMessage
};

/* --------------------------------------------------------------------- */
/* IPrintDocumentPackageTarget vtable                                    */
/* --------------------------------------------------------------------- */

static PrintDialogCallbackImpl *ImplFromPackageTarget(IPrintDocumentPackageTarget *This)
{
    return (PrintDialogCallbackImpl*)((BYTE*)This - offsetof(PrintDialogCallbackImpl, packageTarget));
}

static HRESULT STDMETHODCALLTYPE pkg_QueryInterface(IPrintDocumentPackageTarget *This, REFIID riid, void **ppv)
{
    PrintDialogCallbackImpl *impl = ImplFromPackageTarget(This);
    return cb2_QueryInterface(&impl->callback2, riid, ppv);
}

static ULONG STDMETHODCALLTYPE pkg_AddRef(IPrintDocumentPackageTarget *This)
{
    PrintDialogCallbackImpl *impl = ImplFromPackageTarget(This);
    return cb2_AddRef(&impl->callback2);
}

static ULONG STDMETHODCALLTYPE pkg_Release(IPrintDocumentPackageTarget *This)
{
    PrintDialogCallbackImpl *impl = ImplFromPackageTarget(This);
    return cb2_Release(&impl->callback2);
}

static HRESULT STDMETHODCALLTYPE pkg_GetPackageTargetTypes(IPrintDocumentPackageTarget *This, UINT32 *count, GUID **targetTypes)
{
    (void)This;
    if (count) *count = 0;
    if (targetTypes) *targetTypes = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE pkg_GetPackageTarget(IPrintDocumentPackageTarget *This, const GUID *guidTargetType, REFIID riid, void **ppvTarget)
{
    PrintDialogCallbackImpl *impl = ImplFromPackageTarget(This);

    if (!ppvTarget) return E_POINTER;
    *ppvTarget = NULL;

    if (!IsEqualGUID(guidTargetType, &IID_IPrintPreviewDxgiPackageTarget))
        return E_NOTIMPL;

    HRESULT hr = RefreshPreviewTarget(impl);
    if (FAILED(hr) || !impl->previewTarget)
        return hr ? hr : E_FAIL;

    IUnknown *unk = PreviewTargetAsUnknown(impl->previewTarget);
    if (!unk) return E_FAIL;

    return unk->lpVtbl->QueryInterface(unk, riid, ppvTarget);
}

static HRESULT STDMETHODCALLTYPE pkg_Cancel(IPrintDocumentPackageTarget *This)
{
    (void)This;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE pkg_GetPackageStatus(IPrintDocumentPackageTarget *This)
{
    (void)This;
    return S_OK;
}

static IPrintDocumentPackageTargetVtbl packageTarget_vtbl =
{
    pkg_QueryInterface,
    pkg_AddRef,
    pkg_Release,
    pkg_GetPackageTargetTypes,
    pkg_GetPackageTarget,
    pkg_Cancel,
    pkg_GetPackageStatus
};

/* --------------------------------------------------------------------- */
/* IObjectWithSite vtable                                                */
/* --------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE site_QueryInterface(IObjectWithSite *This, REFIID riid, void **ppv)
{
    PrintDialogCallbackImpl *impl = (PrintDialogCallbackImpl*)
        ((BYTE*)This - offsetof(PrintDialogCallbackImpl, site));
    return cb2_QueryInterface(&impl->callback2, riid, ppv);
}

static ULONG STDMETHODCALLTYPE site_AddRef(IObjectWithSite *This)
{
    PrintDialogCallbackImpl *impl = (PrintDialogCallbackImpl*)
        ((BYTE*)This - offsetof(PrintDialogCallbackImpl, site));
    return cb2_AddRef(&impl->callback2);
}

static ULONG STDMETHODCALLTYPE site_Release(IObjectWithSite *This)
{
    PrintDialogCallbackImpl *impl = (PrintDialogCallbackImpl*)
        ((BYTE*)This - offsetof(PrintDialogCallbackImpl, site));
    return cb2_Release(&impl->callback2);
}

static HRESULT STDMETHODCALLTYPE site_SetSite(IObjectWithSite *This, IUnknown *pUnkSite)
{
    PrintDialogCallbackImpl *impl = (PrintDialogCallbackImpl*)
        ((BYTE*)This - offsetof(PrintDialogCallbackImpl, site));

    if (impl->services) { impl->services->lpVtbl->Release(impl->services); impl->services = NULL; }
    if (impl->siteUnk)   { impl->siteUnk->lpVtbl->Release(impl->siteUnk);   impl->siteUnk   = NULL; }

    if (pUnkSite)
    {
        pUnkSite->lpVtbl->AddRef(pUnkSite);
        impl->siteUnk = pUnkSite;

        HRESULT hr = pUnkSite->lpVtbl->QueryInterface(pUnkSite,
                     &IID_IPrintDialogServices, (void**)&impl->services);
        if (SUCCEEDED(hr))
        {
            DebugLog(L"SetSite -> IPrintDialogServices obtained");
            impl->ctx = &g_printContext;
            RefreshPreviewTarget(impl);
        }
        else
        {
            DebugLog(L"SetSite -> no IPrintDialogServices");
        }
    }
    else
    {
        DebugLog(L"SetSite -> cleared");
    }

    DebugLog(pUnkSite ? L"SetSite called with non-NULL" : L"SetSite called with NULL");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE site_GetSite(IObjectWithSite *This, REFIID riid, void **ppv)
{
    PrintDialogCallbackImpl *impl = (PrintDialogCallbackImpl*)
        ((BYTE*)This - offsetof(PrintDialogCallbackImpl, site));

    if (!ppv) return E_POINTER;
    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IPrintDialogServices) && impl->services)
    {
        impl->services->lpVtbl->AddRef(impl->services);
        *ppv = impl->services;
        return S_OK;
    }

    if (IsEqualIID(riid, &IID_IPrintPreviewDxgiPackageTarget))
    {
        if (SUCCEEDED(RefreshPreviewTarget(impl)) && impl->previewTarget)
        {
            IUnknown *unk = PreviewTargetAsUnknown(impl->previewTarget);
            if (unk)
            {
                unk->lpVtbl->AddRef(unk);
                *ppv = unk;
                return S_OK;
            }
        }
        return E_NOINTERFACE;
    }

    if (impl->siteUnk)
        return impl->siteUnk->lpVtbl->QueryInterface(impl->siteUnk, riid, ppv);

    return E_NOINTERFACE;
}

static IObjectWithSiteVtbl site_vtbl =
{
    site_QueryInterface,
    site_AddRef,
    site_Release,
    site_SetSite,
    site_GetSite
};

/* --------------------------------------------------------------------- */
/* Creation & helpers                                                    */
/* --------------------------------------------------------------------- */

HRESULT CreatePrintDialogCallback(IPrintDialogCallback **out)
{
    if (!out) return E_POINTER;
    *out = NULL;

    PrintDialogCallbackImpl *impl = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*impl));
    if (!impl) return E_OUTOFMEMORY;

    impl->callback.lpVtbl  = (IPrintDialogCallbackVtbl*)&callback_vtbl;
    impl->callback2.lpVtbl = (IPrintDialogCallback2Vtbl*)&callback2_vtbl;
    impl->packageTarget.lpVtbl = &packageTarget_vtbl;
    impl->site.lpVtbl      = (IObjectWithSiteVtbl*)&site_vtbl;
    impl->refCount         = 1;

    *out = (IPrintDialogCallback*)&impl->callback2; // expose callback2 as the base pointer
    return S_OK;
}

/* the rest of your helper functions (RefreshPreviewTarget, GetPreviewTarget, etc.)
   stay exactly the same as in your original file – just cast the incoming pointer
   to IPrintDialogCallback2* when you need the impl */

HRESULT PrintDialogCallbackGetServices(IPrintDialogCallback *cb, IPrintDialogServices **outServices)
{
    if (!outServices) return E_POINTER;
    *outServices = NULL;
    if (!cb) return E_POINTER;

    PrintDialogCallbackImpl *impl = ImplFromCb(cb);
    if (!impl->services) return E_FAIL;

    impl->services->lpVtbl->AddRef(impl->services);
    *outServices = impl->services;
    return S_OK;
}

HRESULT PrintDialogCallbackCopyDevMode(IPrintDialogCallback *cb, HGLOBAL *outDevMode)
{
    if (!outDevMode) return E_POINTER;
    *outDevMode = NULL;
    if (!cb) return E_POINTER;

    PrintDialogCallbackImpl *impl = ImplFromCb(cb);
    if (!impl->services) return E_FAIL;

    UINT size = 0;
    HRESULT hr = impl->services->lpVtbl->GetCurrentDevMode(impl->services, NULL, &size);
    if (FAILED(hr) || size == 0) return hr;

    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
    if (!h) return E_OUTOFMEMORY;

    DEVMODEW *dm = (DEVMODEW*)GlobalLock(h);
    if (!dm)
    {
        GlobalFree(h);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    hr = impl->services->lpVtbl->GetCurrentDevMode(impl->services, dm, &size);
    GlobalUnlock(h);

    if (FAILED(hr))
    {
        GlobalFree(h);
        return hr;
    }

    *outDevMode = h;
    return S_OK;
}

static void ComputePageMetrics(DEVMODEW *dm, const WCHAR *printerName,
    FLOAT *outDpiX, FLOAT *outDpiY, FLOAT *outWidth, FLOAT *outHeight)
{
    FLOAT dpiX = 96.0f, dpiY = 96.0f;
    FLOAT pageWidth = 816.0f;  // 8.5" * 96
    FLOAT pageHeight = 1056.0f; // 11" * 96

    HDC hdc = CreateDCW(NULL, printerName, NULL, dm);
    if (hdc)
    {
        int capDpiX = GetDeviceCaps(hdc, LOGPIXELSX);
        int capDpiY = GetDeviceCaps(hdc, LOGPIXELSY);
        if (capDpiX > 0) dpiX = (FLOAT)capDpiX;
        if (capDpiY > 0) dpiY = (FLOAT)capDpiY;

        int physWidth = GetDeviceCaps(hdc, PHYSICALWIDTH);
        int physHeight = GetDeviceCaps(hdc, PHYSICALHEIGHT);
        if (physWidth > 0 && physHeight > 0 && capDpiX > 0 && capDpiY > 0)
        {
            pageWidth = (FLOAT)physWidth * (96.0f / (FLOAT)capDpiX);
            pageHeight = (FLOAT)physHeight * (96.0f / (FLOAT)capDpiY);
        }
        DeleteDC(hdc);
    }

    if (dm)
    {
        if (dm->dmFields & DM_PAPERWIDTH)
        {
            double inches = (dm->dmPaperWidth * 0.1) / 25.4;
            pageWidth = (FLOAT)(inches * 96.0);
        }
        if (dm->dmFields & DM_PAPERLENGTH)
        {
            double inches = (dm->dmPaperLength * 0.1) / 25.4;
            pageHeight = (FLOAT)(inches * 96.0);
        }
        if ((dm->dmFields & DM_ORIENTATION) && dm->dmOrientation == DMORIENT_LANDSCAPE)
        {
            FLOAT t = pageWidth;
            pageWidth = pageHeight;
            pageHeight = t;
        }
    }

    if (outDpiX) *outDpiX = dpiX;
    if (outDpiY) *outDpiY = dpiY;
    if (outWidth) *outWidth = pageWidth;
    if (outHeight) *outHeight = pageHeight;
}

static HRESULT RefreshPreviewTarget(PrintDialogCallbackImpl *impl)
{
    if (!impl->services || !impl->ctx) return E_FAIL;

    if (!impl->previewTarget)
    {
        HRESULT hrCreate = CreatePreviewTarget(&impl->previewTarget);
        if (FAILED(hrCreate)) return hrCreate;
    }

    FLOAT dpiX = 96.0f, dpiY = 96.0f, width = 816.0f, height = 1056.0f;
    WCHAR printerName[256] = {0};
    UINT len = ARRAYSIZE(printerName);
    if (SUCCEEDED(impl->services->lpVtbl->GetCurrentPrinterName(impl->services, printerName, &len)))
    {
        HGLOBAL hDm = NULL;
        if (SUCCEEDED(PrintDialogCallbackCopyDevMode((IPrintDialogCallback*)(&impl->callback2), &hDm)))
        {
            DEVMODEW *dm = (DEVMODEW*)GlobalLock(hDm);
            ComputePageMetrics(dm, printerName, &dpiX, &dpiY, &width, &height);
            GlobalUnlock(hDm);
            GlobalFree(hDm);
        }
        else
        {
            ComputePageMetrics(NULL, printerName, &dpiX, &dpiY, &width, &height);
        }
    }
    else
    {
        ComputePageMetrics(NULL, NULL, &dpiX, &dpiY, &width, &height);
    }

    return PreviewTargetSetRenderer(impl->previewTarget, impl->ctx, dpiX, dpiY, width, height);
}

HRESULT PrintDialogCallbackGetPreviewTarget(IPrintDialogCallback *cb, PreviewTarget **outTarget)
{
    if (!outTarget) return E_POINTER;
    *outTarget = NULL;
    if (!cb) return E_POINTER;

    PrintDialogCallbackImpl *impl = ImplFromCb(cb);
    HRESULT hr = RefreshPreviewTarget(impl);
    if (FAILED(hr)) return hr;

    *outTarget = impl->previewTarget;
    if (*outTarget)
    {
        IUnknown *unk = PreviewTargetAsUnknown(*outTarget);
        if (unk) unk->lpVtbl->AddRef(unk);
    }
    return S_OK;
}

HRESULT PrintDialogCallbackSetContext(IPrintDialogCallback *cb, PrintRenderContext *ctx)
{
    if (!cb) return E_POINTER;
    PrintDialogCallbackImpl *impl = ImplFromCb(cb);
    impl->ctx = ctx;
    if (ctx) RefreshPreviewTarget(impl);
    return S_OK;
}
