#include <windows.h>
#include <objbase.h>
#include <strsafe.h>
#include <xpsprint.h>
#include <xpsobjectmodel.h>

#include "xps_backend.h"

HRESULT XpsBackendInitialize(XpsBackend *backend) {
    if (!backend) return E_POINTER;
    ZeroMemory(backend, sizeof(*backend));

    HRESULT hr = CoCreateInstance(__uuidof(XpsOMObjectFactory), NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&backend->factory));
    if (FAILED(hr)) {
        return hr;
    }

    hr = CoCreateInstance(__uuidof(PrintDocumentPackageTargetFactory), NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&backend->targetFactory));
    if (FAILED(hr)) {
        backend->factory->Release();
        backend->factory = NULL;
        return hr;
    }
    return S_OK;
}

void XpsBackendShutdown(XpsBackend *backend) {
    if (!backend) return;
    if (backend->targetFactory) {
        backend->targetFactory->Release();
        backend->targetFactory = NULL;
    }
    if (backend->factory) {
        backend->factory->Release();
        backend->factory = NULL;
    }
}

HRESULT XpsBackendCreateTarget(XpsBackend *backend, LPCWSTR printerName, IPrintDocumentPackageTarget **target) {
    if (!backend || !backend->targetFactory || !target) return E_POINTER;
    *target = NULL;
    LPCWSTR jobName = L"retropad";
    return backend->targetFactory->CreateDocumentPackageTargetForPrintJob(printerName, jobName, nullptr, nullptr, target);
}

HRESULT XpsBackendCreateFileWriter(XpsBackend *backend, LPCWSTR filePath, IXpsOMPackageWriter **writer) {
    if (!backend || !backend->factory || !filePath || !writer) return E_POINTER;
    *writer = NULL;

    IOpcPartUri *seqUri = NULL;
    IOpcPartUri *docUri = NULL;
    HRESULT hr = backend->factory->CreatePartUri(L"/FixedDocSeq.fdseq", &seqUri);
    if (FAILED(hr)) goto cleanup;
    hr = backend->factory->CreatePartUri(L"/Documents/1/FixedDocument.fdoc", &docUri);
    if (FAILED(hr)) goto cleanup;

    hr = backend->factory->CreatePackageWriterOnFile(
        filePath,
        NULL,
        0,
        TRUE,
        XPS_INTERLEAVING_ON,
        seqUri,
        NULL,
        NULL,
        NULL,
        NULL,
        writer);

cleanup:
    if (seqUri) seqUri->Release();
    if (docUri) docUri->Release();
    if (FAILED(hr) && writer) {
        *writer = NULL;
    }
    return hr;
}

void XpsPackageWriterClose(IXpsOMPackageWriter *writer) {
    if (writer) writer->Close();
}

void XpsPackageWriterRelease(IXpsOMPackageWriter *writer) {
    if (writer) writer->Release();
}

HRESULT XpsBackendCreateWriterForTarget(XpsBackend *backend, IPrintDocumentPackageTarget *target, IXpsOMPackageWriter **writer) {
    if (!backend || !backend->factory || !target || !writer) return E_POINTER;
    *writer = NULL;

    IXpsOMPackageTarget *packageTarget = NULL;
    IOpcPartUri *docSeqUri = NULL;
    HRESULT hr = target->QueryInterface(__uuidof(IXpsOMPackageTarget), (void **)&packageTarget);
    if (FAILED(hr)) goto cleanup;

    hr = backend->factory->CreatePartUri(L"/FixedDocSeq.fdseq", &docSeqUri);
    if (FAILED(hr)) goto cleanup;

    hr = packageTarget->CreateXpsOMPackageWriter(docSeqUri, NULL, NULL, writer);

cleanup:
    if (docSeqUri) docSeqUri->Release();
    if (packageTarget) packageTarget->Release();
    if (FAILED(hr)) {
        *writer = NULL;
    }
    return hr;
}
