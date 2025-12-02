#include <stddef.h>
#include <ocidl.h>
#include <PrintPreview.h>

#include "print_dialog_callback.h"
#include "preview_target.h"

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

typedef struct PrintDialogCallbackImpl {
    IPrintDialogCallback callback;
    IObjectWithSite siteIface;
    LONG refCount;
    IUnknown *siteUnk;
    IPrintDialogServices *services;
} PrintDialogCallbackImpl;

static PrintDialogCallbackImpl *FromCallback(IPrintDialogCallback *iface) {
    return (PrintDialogCallbackImpl *)iface;
}

static PrintDialogCallbackImpl *FromSite(IObjectWithSite *iface) {
    return (PrintDialogCallbackImpl *)((BYTE *)iface - offsetof(PrintDialogCallbackImpl, siteIface));
}

static HRESULT STDMETHODCALLTYPE Callback_QueryInterface(IPrintDialogCallback *self, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IPrintDialogCallback)) {
        *ppv = self;
    } else if (IsEqualIID(riid, &IID_IObjectWithSite)) {
        PrintDialogCallbackImpl *impl = FromCallback(self);
        *ppv = &impl->siteIface;
    } else {
        return E_NOINTERFACE;
    }
    ((IPrintDialogCallback *)(*ppv))->lpVtbl->AddRef((IPrintDialogCallback *)*ppv);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE Callback_AddRef(IPrintDialogCallback *self) {
    PrintDialogCallbackImpl *impl = FromCallback(self);
    return (ULONG)InterlockedIncrement(&impl->refCount);
}

static ULONG STDMETHODCALLTYPE Callback_Release(IPrintDialogCallback *self) {
    PrintDialogCallbackImpl *impl = FromCallback(self);
    LONG ref = InterlockedDecrement(&impl->refCount);
    if (ref == 0) {
        if (impl->services) {
            impl->services->lpVtbl->Release(impl->services);
        }
        if (impl->siteUnk) {
            impl->siteUnk->lpVtbl->Release(impl->siteUnk);
        }
        HeapFree(GetProcessHeap(), 0, impl);
    }
    return (ULONG)ref;
}

static HRESULT STDMETHODCALLTYPE Callback_InitDone(IPrintDialogCallback *self) {
    (void)self;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Callback_SelectionChange(IPrintDialogCallback *self) {
    (void)self;
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

static const IPrintDialogCallbackVtbl g_callbackVtbl = {
    Callback_QueryInterface,
    Callback_AddRef,
    Callback_Release,
    Callback_InitDone,
    Callback_SelectionChange,
    Callback_HandleMessage
};

static HRESULT STDMETHODCALLTYPE Site_QueryInterface(IObjectWithSite *self, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    PrintDialogCallbackImpl *impl = FromSite(self);
    return Callback_QueryInterface(&impl->callback, riid, ppv);
}

static ULONG STDMETHODCALLTYPE Site_AddRef(IObjectWithSite *self) {
    PrintDialogCallbackImpl *impl = FromSite(self);
    return Callback_AddRef(&impl->callback);
}

static ULONG STDMETHODCALLTYPE Site_Release(IObjectWithSite *self) {
    PrintDialogCallbackImpl *impl = FromSite(self);
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
        punk->lpVtbl->QueryInterface(punk, &IID_IPrintDialogServices, (void **)&impl->services);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Site_GetSite(IObjectWithSite *self, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    PrintDialogCallbackImpl *impl = FromSite(self);
    if (impl->siteUnk) {
        return impl->siteUnk->lpVtbl->QueryInterface(impl->siteUnk, riid, ppv);
    }
    return E_FAIL;
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
    impl->siteIface.lpVtbl = (IObjectWithSiteVtbl *)&g_siteVtbl;
    impl->refCount = 1;
    impl->services = NULL;
    impl->siteUnk = NULL;
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

HRESULT PrintDialogCallbackGetPreviewTarget(IPrintDialogCallback *cb, PreviewTarget **outTarget) {
    (void)cb;
    if (outTarget) *outTarget = NULL;
    return E_NOTIMPL;
}
