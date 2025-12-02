#pragma once

#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <shlwapi.h>
#include "preview_target.h"

#ifdef __cplusplus
extern "C" {
#endif

HRESULT CreatePrintDialogCallback(IPrintDialogCallback **out);
HRESULT PrintDialogCallbackGetServices(IPrintDialogCallback *cb, IPrintDialogServices **outServices);
HRESULT PrintDialogCallbackCopyDevMode(IPrintDialogCallback *cb, HGLOBAL *outDevMode);
HRESULT PrintDialogCallbackGetPreviewTarget(IPrintDialogCallback *cb, PreviewTarget **outTarget);

#ifdef __cplusplus
}
#endif
