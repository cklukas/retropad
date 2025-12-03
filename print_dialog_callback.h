#pragma once

#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <shlwapi.h>
#include "preview_target.h"

#ifdef __cplusplus
extern "C" {
#endif

struct PrintRenderContext;
#ifndef __IPrintDialogCallback2_FWD_DEFINED__
typedef interface IPrintDialogCallback2 IPrintDialogCallback2;
#endif
#ifndef __IPrintDialogServices_FWD_DEFINED__
typedef interface IPrintDialogServices IPrintDialogServices;
#endif

HRESULT CreatePrintDialogCallback(IPrintDialogCallback2 **out);
HRESULT PrintDialogCallbackGetServices(IPrintDialogCallback2 *cb, IPrintDialogServices **outServices);
HRESULT PrintDialogCallbackCopyDevMode(IPrintDialogCallback2 *cb, HGLOBAL *outDevMode);
HRESULT PrintDialogCallbackGetPreviewTarget(IPrintDialogCallback2 *cb, PreviewTarget **outTarget);
HRESULT PrintDialogCallbackSetContext(IPrintDialogCallback2 *cb, struct PrintRenderContext *ctx);

#ifdef __cplusplus
}
#endif
