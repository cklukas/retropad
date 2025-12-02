#pragma once

#include <windows.h>
#include <xpsobjectmodel.h>
#include <documenttarget.h>

typedef struct XpsBackend {
    IXpsOMObjectFactory *factory;
    IPrintDocumentPackageTargetFactory *targetFactory;
} XpsBackend;

#ifdef __cplusplus
extern "C" {
#endif

HRESULT XpsBackendInitialize(XpsBackend *backend);
void XpsBackendShutdown(XpsBackend *backend);
HRESULT XpsBackendCreateTarget(XpsBackend *backend, LPCWSTR printerName, IPrintDocumentPackageTarget **target);
HRESULT XpsBackendCreateFileWriter(XpsBackend *backend, LPCWSTR filePath, IXpsOMPackageWriter **writer);
void XpsPackageWriterClose(IXpsOMPackageWriter *writer);
void XpsPackageWriterRelease(IXpsOMPackageWriter *writer);
HRESULT XpsBackendCreateWriterForTarget(XpsBackend *backend, IPrintDocumentPackageTarget *target, IXpsOMPackageWriter **writer);

#ifdef __cplusplus
}
#endif
