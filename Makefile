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

OUTDIR=binaries
CFLAGS=/nologo /DUNICODE /D_UNICODE /W4 /EHsc
LDFLAGS=/nologo
LIBS=user32.lib gdi32.lib comdlg32.lib comctl32.lib shell32.lib ole32.lib xpsprint.lib ole32.lib xpsprint.lib ole32.lib dxgi.lib d3d11.lib uuid.lib

OBJS=$(OUTDIR)\retropad.obj $(OUTDIR)\file_io.obj $(OUTDIR)\print.obj $(OUTDIR)\rendering.obj $(OUTDIR)\print_dialog_callback.obj $(OUTDIR)\preview_target.obj $(OUTDIR)\xps_backend.obj $(OUTDIR)\xps_target.obj $(OUTDIR)\retropad.res

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

$(OUTDIR)\retropad.res: $(OUTDIR) retropad.rc resource.h res\retropad.ico retropad.manifest
	$(RC) $(RCFLAGS) /fo $(OUTDIR)\retropad.res retropad.rc

$(OUTDIR)\retropad.chm: $(OUTDIR) help\retropad.hhp help\toc.hhc help\index.html
	@cmd /c "pushd help && $(HHC) retropad.hhp >NUL & if exist retropad.chm (exit /b 0) else (exit /b 1)"
	@if exist "help\retropad.chm" copy /Y "help\retropad.chm" "$(OUTDIR)\retropad.chm" >NUL

clean:
	-del /q $(OUTDIR)\retropad.exe $(OUTDIR)\retropad.obj $(OUTDIR)\file_io.obj $(OUTDIR)\print.obj $(OUTDIR)\rendering.obj $(OUTDIR)\print_dialog_callback.obj $(OUTDIR)\xps_backend.obj $(OUTDIR)\xps_target.obj $(OUTDIR)\retropad.res $(OUTDIR)\*.pdb 2> NUL
	-del /q retropad.exe retropad.obj file_io.obj print.obj rendering.obj print_dialog_callback.obj xps_backend.obj xps_target.obj retropad.res retropad.pdb 2> NUL
