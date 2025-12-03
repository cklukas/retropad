# Print Preview Implementation Checklist

1. Rendering abstraction layer
   - [x] Extract existing GDI print layout logic into reusable renderer functions/modules  
   - [x] Define interfaces/structures that describe pages, margins, fonts, and text content
2. XPS rendering backend
   - [x] Define renderer interface types that support both GDI and XPS targets  
   - [x] Add XPS OM dependencies and initialization helpers  
   - [x] Implement XPS page generation using the shared rendering abstraction
       - [x] Build XPS page/package writer plumbing (create package writer, page URIs, manage lifecycle)
       - [x] Flesh out XpsRenderTarget to track current page/visuals and write glyphs
       - [x] Map renderer operations to XPS OM primitives (glyph creation, text positioning, headers/footers)
       - [ ] Hook the XPS target into the printing flow and validate output
           - [x] Send print jobs through IPrintDocumentPackageTarget + IXpsOMPackageWriter (no preview yet)
           - [x] Use IPrintDialogServices to get current printer info from PrintDlgEx (name handled; ready for preview callbacks)
           - [ ] Provide the dialog with preview pages via XPS (IPrintPreviewCallback / DXGI preview target; stub in place)
           - [x] Remove the GDI StartDoc path when XPS succeeds; Do NOT keep GDI as fallback
           - [ ] Validate end-to-end print and preview behavior, update error handling/messages
3. COM callback class
   - [x] Create a COM object (IUnknown, IPrintDialogCallback) with site support  
   - [ ] Provide preview page data via the renderer when the dialog requests it (IPrintPreviewCallback / DXGI target; stub exists)  
   - [x] Manage current printer/DevMode via dialog services for XPS jobs
4. Integrate with PrintDlgEx
   - [ ] Wire the COM callback into PRINTDLGEX::lpCallback and set PD_USEXPSCONVERSION  
       - [x] Set PD_USEXPSCONVERSION flag in the dialog  
       - [x] Attach custom IPrintDialogCallback implementation
   - [ ] Obtain IPrintDialogServices to get printer/device context info  
   - [ ] Feed rendered XPS pages to IPrintDocumentPackageTarget for the final print job
5. Cleanup & fallbacks
   - [x] Release COM/XPS resources correctly in the backends  
   - [x] Remove legacy GDI fallback; assume Windows 11 mainline (surface errors if XPS fails)
6. Documentation & testing
   - [ ] Update README/help describing the new preview requirements  
   - [ ] Test with multiple printers and DPI settings to validate preview accuracy


## Status

Requirement (Windows 11 24H2),Your Implementation,Status
Manifest: DPI-aware + long-path + printerDriverIsolation,Perfect,Pass
`PD_RETURNDC,PD_ALLPAGES,PD_NOPAGENUMS
"nMinPage = 1, nMaxPage = 1",Correct,Pass
No PD_USEXPSCONVERSION,Removed,Pass
lpCallback = exact IPrintDialogCallback* (no cast),Fixed,Pass
Callback implements IPrintDialogCallback2 with 8 methods including SetPrintTicket,Done,Pass
IPrintDialogCallback2 is first in struct,Done,Pass
CreatePrintDialogCallback returns pointer to object base,Done,Pass
Callback lifetime outlives PrintDlgExW (global g_printCallback),Done,Pass
SetSite sets impl->ctx = &g_printContext,Done,Pass
RefreshPreviewTarget uses real IPrintDialogServices,Done,Pass
DrawPage uses DXGI + DIB fallback,Done,Pass