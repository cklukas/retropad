!IFNDEF CC
CC=cl
!ENDIF

!IFNDEF RC
RC=rc
!ENDIF

!IFNDEF RCFLAGS
RCFLAGS=/nologo /utf8
!ENDIF

!IFNDEF HHC
HHC="C:\Program Files (x86)\HTML Help Workshop\hhc.exe"
!ENDIF

!IFNDEF WINAPPSDK_DIR
WINAPPSDK_DIR=$(MAKEDIR)\microsoft.windowsappsdk.winui.1.8.251105000
!ENDIF

!IF !EXIST("$(WINAPPSDK_DIR)\include\winrt\Windows.UI.Xaml.Hosting.h")
!ERROR WINAPPSDK_DIR does not contain WinUI headers. Set WINAPPSDK_DIR to the WinAppSDK WinUI package root (e.g. microsoft.windowsappsdk.winui.1.8.xxxxx) so print preview can build.
!ENDIF

# Optional: allow overriding Windows SDK selection (useful with clang-cl or custom layouts)
!IFDEF SDKVER
WINSDKVER_FLAG=/winsdkversion:$(SDKVER)
!ENDIF
!IFDEF WINSDKROOT
WINSDKROOT_FLAG=/winsysroot "$(WINSDKROOT)"
!ENDIF

OUTDIR=binaries
CFLAGS=/nologo /DUNICODE /D_UNICODE /DWINVER=0x0A00 /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A00000A /W4 /WX /EHsc $(WINSDKVER_FLAG) $(WINSDKROOT_FLAG) /I"C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um" /I"C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\shared"
LDFLAGS=/nologo $(WINSDKROOT_FLAG)
LIBS=user32.lib gdi32.lib comdlg32.lib comctl32.lib shell32.lib ole32.lib xpsprint.lib ole32.lib xpsprint.lib ole32.lib dxgi.lib d3d11.lib uuid.lib windowsapp.lib

!IFDEF WINAPPSDK_DIR
CFLAGS=$(CFLAGS) /I"$(WINAPPSDK_DIR)\include"
!IF EXIST("$(WINAPPSDK_DIR)\lib\x64")
LDFLAGS=$(LDFLAGS) /LIBPATH:"$(WINAPPSDK_DIR)\lib\x64"
!ENDIF
!ENDIF

OBJS=$(OUTDIR)\retropad.obj $(OUTDIR)\file_io.obj $(OUTDIR)\print.obj $(OUTDIR)\rendering.obj $(OUTDIR)\print_dialog_callback.obj $(OUTDIR)\preview_target.obj $(OUTDIR)\xps_backend.obj $(OUTDIR)\xps_target.obj $(OUTDIR)\PrintPreviewWindow.obj $(OUTDIR)\WinUIHosting.obj $(OUTDIR)\retropad.res

all: $(OUTDIR)\retropad.exe $(OUTDIR)\retropad.chm

$(OUTDIR):
	@if not exist "$(OUTDIR)" mkdir "$(OUTDIR)"

$(OUTDIR)\retropad.exe: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) $(LIBS) /Fe:$(OUTDIR)\retropad.exe

$(OUTDIR)\retropad.obj: $(OUTDIR) retropad.c resource.h file_io.h
	$(CC) $(CFLAGS) /Fo$(OUTDIR)\ /c retropad.c

$(OUTDIR)\file_io.obj: $(OUTDIR) file_io.c file_io.h resource.h
	$(CC) $(CFLAGS) /Fo$(OUTDIR)\ /c file_io.c

$(OUTDIR)\print.obj: $(OUTDIR) print.c retropad.h print.h resource.h
	$(CC) $(CFLAGS) /Fo$(OUTDIR)\ /c print.c

$(OUTDIR)\print_dialog_callback.obj: $(OUTDIR) print_dialog_callback.c print_dialog_callback.h
	$(CC) $(CFLAGS) /Fo$(OUTDIR)\ /c print_dialog_callback.c

$(OUTDIR)\preview_target.obj: $(OUTDIR) preview_target.c preview_target.h
	$(CC) $(CFLAGS) /Fo$(OUTDIR)\ /c preview_target.c

$(OUTDIR)\rendering.obj: $(OUTDIR) rendering.c rendering.h retropad.h
	$(CC) $(CFLAGS) /Fo$(OUTDIR)\ /c rendering.c

$(OUTDIR)\xps_backend.obj: $(OUTDIR) xps_backend.cpp xps_backend.h
	$(CC) $(CFLAGS) /Fo$(OUTDIR)\ /c xps_backend.cpp

$(OUTDIR)\xps_target.obj: $(OUTDIR) xps_target.cpp xps_target.h rendering.h
	$(CC) $(CFLAGS) /Fo$(OUTDIR)\ /c xps_target.cpp

$(OUTDIR)\PrintPreviewWindow.obj: $(OUTDIR) PrintPreviewWindow.cpp PrintPreviewWindow.h rendering.h
	$(CC) $(CFLAGS) /std:c++20 /Zc:__cplusplus /Fo$(OUTDIR)\ /c PrintPreviewWindow.cpp

$(OUTDIR)\WinUIHosting.obj: $(OUTDIR) WinUIHosting.cpp WinUIHosting.h
	$(CC) $(CFLAGS) /std:c++20 /Zc:__cplusplus /Fo$(OUTDIR)\ /c WinUIHosting.cpp

$(OUTDIR)\retropad.res: $(OUTDIR) retropad.rc resource.h res\retropad.ico retropad.manifest
	$(RC) $(RCFLAGS) /fo $(OUTDIR)\retropad.res retropad.rc

$(OUTDIR)\retropad.chm: $(OUTDIR) help\retropad.hhp help\toc.hhc help\index.html
	@cmd /c "pushd help && $(HHC) retropad.hhp >NUL & if exist retropad.chm (exit /b 0) else (exit /b 1)"
	@if exist "help\retropad.chm" copy /Y "help\retropad.chm" "$(OUTDIR)\retropad.chm" >NUL

clean:
	-del /q $(OUTDIR)\retropad.exe $(OUTDIR)\retropad.obj $(OUTDIR)\file_io.obj $(OUTDIR)\print.obj $(OUTDIR)\rendering.obj $(OUTDIR)\print_dialog_callback.obj $(OUTDIR)\xps_backend.obj $(OUTDIR)\xps_target.obj $(OUTDIR)\retropad.res $(OUTDIR)\*.pdb 2> NUL
	-del /q retropad.exe retropad.obj file_io.obj print.obj rendering.obj print_dialog_callback.obj xps_backend.obj xps_target.obj retropad.res retropad.pdb 2> NUL
