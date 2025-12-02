#pragma once

#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <shlwapi.h>
#include "preview_target.h"

struct PrintRenderContext;

#ifdef __cplusplus
extern "C" {
#endif

HRESULT CreatePrintDialogCallback(IPrintDialogCallback **out);
HRESULT PrintDialogCallbackGetServices(IPrintDialogCallback *cb, IPrintDialogServices **outServices);
HRESULT PrintDialogCallbackCopyDevMode(IPrintDialogCallback *cb, HGLOBAL *outDevMode);
HRESULT PrintDialogCallbackGetPreviewTarget(IPrintDialogCallback *cb, PreviewTarget **outTarget);
HRESULT PrintDialogCallbackSetContext(IPrintDialogCallback *cb, struct PrintRenderContext *ctx);

#ifdef __cplusplus
}
#endif
