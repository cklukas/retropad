# Print Preview Investigation Notes

## Attempts already made
- Passed custom `IPrintDialogCallback` to `PrintDlgEx` and wired `IObjectWithSite::GetSite` to return `IID_IPrintPreviewDxgiPackageTarget` from our `PreviewTarget`.
- Initialized the preview target with the current render context and printer metrics via `IPrintDialogServices`.
- Added DXGI fallback: if `IDXGISurface1::GetDC` fails, render into a temporary DIB and copy pixels into the mapped surface; surface is cleared first and rendering is filtered to the requested page.
- Added detailed tracing (QI/AddRef/Release/SetSite/GetSite/DrawPage/PrintDlgEx results) to `%TEMP%\retropad_preview.log`; `PrintDlgEx` keeps returning `0x80070006` (E_HANDLE) and never calls the callback (no QI/SetSite/DrawPage).
- Tried multiple flag combos: with/without `PD_USEXPSCONVERSION`, with/without `PD_RETURNDC`, with cached handles, with fresh default DEVMODE/DEVNAMES, and a classic `PrintDlg` fallback; `PrintDlgEx` still fails before preview is requested.
- Probed `IPrintDocumentPackageTarget::GetPackageTarget` for `ID_PREVIEWPACKAGETARGET_DXGI`; OS returns no preview target.
- Build is clean; preview still not shown by the dialog.

## Next things to try
- Focus on the `E_HANDLE` root cause: try absolute minimal flags (`PD_ALLPAGES | PD_USEXPSCONVERSION` only) with only fresh default DEVMODE/DEVNAMES; also try a call with `lpCallback = NULL` to see if the callback triggers the error.
- Check SDK more carefully
- Explore WinRT `IPrintPreviewPageCollection`/DocumentSource preview route as a fallback if the legacy dialog refuses to provide preview callbacks.
- THIS IS NOT AN OPTION: If `PrintDlgEx` still fails, bypass it: use classic `PrintDlg` for printing and build an in-app preview pane (rendering to DIB/XPS) instead of relying on the dialog preview - WE WANT PROPER XPS-BASED PREVIEW SUPPORT!!
