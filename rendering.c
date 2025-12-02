#include <windows.h>
#include <strsafe.h>

#include "retropad.h"
#include "rendering.h"

static void SplitHeaderFooterSegments(const WCHAR *format, const WCHAR *fileName, int pageNumber, int totalPages, const WCHAR *dateStr, const WCHAR *timeStr, WCHAR *left, size_t cchLeft, WCHAR *center, size_t cchCenter, WCHAR *right, size_t cchRight);
static int ComputeTotalPages(const WCHAR *text, int charsPerLine, int linesPerPage);

static BOOL RenderInternal(const PrintRenderContext *ctx, const PrintRenderTarget *target);

static BOOL HdcGetMetrics(void *userData, PrintMetrics *metrics);
static BOOL HdcGetPageSize(void *userData, int *width, int *height);
static BOOL HdcBeginDocument(void *userData, const WCHAR *docName, int totalPages);
static BOOL HdcEndDocument(void *userData);
static BOOL HdcAbortDocument(void *userData);
static BOOL HdcBeginPage(void *userData, int pageNumber);
static BOOL HdcEndPage(void *userData);
static BOOL HdcDrawText(void *userData, int x, int y, const WCHAR *text, int length);
static BOOL HdcMeasureText(void *userData, const WCHAR *text, int length, SIZE *size);

static const PrintRenderTargetOps g_hdcOps = {
    HdcGetMetrics,
    HdcGetPageSize,
    HdcBeginDocument,
    HdcEndDocument,
    HdcAbortDocument,
    HdcBeginPage,
    HdcEndPage,
    HdcDrawText,
    HdcMeasureText,
};

BOOL RenderDocument(const PrintRenderContext *ctx, const PrintRenderTarget *target) {
    if (!ctx || !target || !target->ops) return FALSE;
    return RenderInternal(ctx, target);
}

void InitHdcRenderTarget(HdcRenderTarget *target, HDC hdc) {
    if (!target) return;
    target->base.ops = &g_hdcOps;
    target->base.userData = target;
    target->hdc = hdc;
    target->documentStarted = FALSE;
}

static BOOL RenderInternal(const PrintRenderContext *ctx, const PrintRenderTarget *target) {
    PrintMetrics metrics;
    if (!target->ops->GetMetrics(target->userData, &metrics)) return FALSE;

    int pageWidth = 0, pageHeight = 0;
    if (!target->ops->GetPageSize(target->userData, &pageWidth, &pageHeight)) return FALSE;

    int lineHeight = metrics.lineHeight;
    int avgCharWidth = metrics.averageCharWidth;
    BOOL headerEnabled = ctx->headerText && ctx->headerText[0] != L'\0';
    BOOL footerEnabled = ctx->footerText && ctx->footerText[0] != L'\0';
    int headerHeight = headerEnabled ? lineHeight : 0;
    int footerHeight = footerEnabled ? lineHeight : 0;

    RECT m = ctx->marginsThousandths;
    if (m.left == 0 && m.right == 0 && m.top == 0 && m.bottom == 0) {
        m.left = m.right = 750;
        m.top = m.bottom = 1000;
    }

    int marginLeft = MulDiv(m.left, metrics.dpiX, 1000);
    int marginRight = MulDiv(m.right, metrics.dpiX, 1000);
    int marginTop = MulDiv(m.top, metrics.dpiY, 1000);
    int marginBottom = MulDiv(m.bottom, metrics.dpiY, 1000);

    int printableWidth = pageWidth - marginLeft - marginRight;
    int printableHeight = pageHeight - marginTop - marginBottom - headerHeight - footerHeight;
    if (printableWidth < avgCharWidth) printableWidth = avgCharWidth;
    if (printableHeight < lineHeight) printableHeight = lineHeight;

    int charsPerLine = max(1, printableWidth / max(1, avgCharWidth));
    int linesPerPage = max(1, printableHeight / lineHeight);
    int contentTop = marginTop + headerHeight;

    WCHAR dateStr[64] = {0};
    WCHAR timeStr[64] = {0};
    SYSTEMTIME st;
    GetLocalTime(&st);
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, dateStr, ARRAYSIZE(dateStr));
    GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, timeStr, ARRAYSIZE(timeStr));

    int totalPages = ComputeTotalPages(ctx->text, charsPerLine, linesPerPage);
    const WCHAR *fullPath = ctx->fullPath ? ctx->fullPath : UNTITLED_NAME;

    if (!target->ops->BeginDocument(target->userData, fullPath, totalPages)) {
        return FALSE;
    }

    BOOL success = FALSE;
    const WCHAR *fileName = fullPath;
    const WCHAR *slash = wcsrchr(fullPath, L'\\');
    if (slash && *(slash + 1)) {
        fileName = slash + 1;
    }

    const int tabWidth = 8;
    int lineOnPage = 0;
    int y = contentTop;
    int pageNumber = 1;
    WCHAR lineBuf[4096];
    WCHAR headerLeft[128], headerCenter[128], headerRight[128];
    WCHAR footerLeft[128], footerCenter[128], footerRight[128];

    SplitHeaderFooterSegments(ctx->headerText, fileName, pageNumber, totalPages, dateStr, timeStr, headerLeft, ARRAYSIZE(headerLeft), headerCenter, ARRAYSIZE(headerCenter), headerRight, ARRAYSIZE(headerRight));
    SplitHeaderFooterSegments(ctx->footerText, fileName, pageNumber, totalPages, dateStr, timeStr, footerLeft, ARRAYSIZE(footerLeft), footerCenter, ARRAYSIZE(footerCenter), footerRight, ARRAYSIZE(footerRight));

    if (!target->ops->BeginPage(target->userData, pageNumber)) {
        target->ops->AbortDocument(target->userData);
        return FALSE;
    }

    if (headerEnabled) {
        SIZE sz;
        if (headerLeft[0]) {
            target->ops->DrawText(target->userData, marginLeft, marginTop, headerLeft, lstrlenW(headerLeft));
        }
        if (headerCenter[0]) {
            target->ops->MeasureText(target->userData, headerCenter, lstrlenW(headerCenter), &sz);
            int cx = marginLeft + (printableWidth - sz.cx) / 2;
            target->ops->DrawText(target->userData, cx, marginTop, headerCenter, lstrlenW(headerCenter));
        }
        if (headerRight[0]) {
            target->ops->MeasureText(target->userData, headerRight, lstrlenW(headerRight), &sz);
            int rx = pageWidth - marginRight - sz.cx;
            target->ops->DrawText(target->userData, rx, marginTop, headerRight, lstrlenW(headerRight));
        }
    }

    const WCHAR *p = ctx->text;
    while (*p) {
        int bufLen = 0;
        int col = 0;
        while (*p && *p != L'\r' && *p != L'\n') {
            WCHAR ch = *p++;
            if (ch == L'\t') {
                int spaces = tabWidth - (col % tabWidth);
                for (int i = 0; i < spaces; ++i) {
                    if (bufLen < (int)ARRAYSIZE(lineBuf) - 1) {
                        lineBuf[bufLen++] = L' ';
                    }
                    col++;
                    if (col >= charsPerLine) break;
                }
            } else {
                if (bufLen < (int)ARRAYSIZE(lineBuf) - 1) {
                    lineBuf[bufLen++] = ch;
                }
                col++;
            }

            if (col >= charsPerLine) {
                target->ops->DrawText(target->userData, marginLeft, y, lineBuf, bufLen);
                y += lineHeight;
                lineOnPage++;
                bufLen = 0;
                col = 0;
                if (lineOnPage >= linesPerPage) {
                    const WCHAR *peek = p;
                    while (*peek == L'\r' || *peek == L'\n') peek++;
                    if (*peek) {
                        if (footerEnabled && (footerLeft[0] || footerCenter[0] || footerRight[0])) {
                            SIZE ft;
                            if (footerLeft[0]) {
                                target->ops->DrawText(target->userData, marginLeft, pageHeight - marginBottom - footerHeight, footerLeft, lstrlenW(footerLeft));
                            }
                            if (footerCenter[0]) {
                                target->ops->MeasureText(target->userData, footerCenter, lstrlenW(footerCenter), &ft);
                                int cx = marginLeft + (printableWidth - ft.cx) / 2;
                                target->ops->DrawText(target->userData, cx, pageHeight - marginBottom - footerHeight, footerCenter, lstrlenW(footerCenter));
                            }
                            if (footerRight[0]) {
                                target->ops->MeasureText(target->userData, footerRight, lstrlenW(footerRight), &ft);
                                int rx = pageWidth - marginRight - ft.cx;
                                target->ops->DrawText(target->userData, rx, pageHeight - marginBottom - footerHeight, footerRight, lstrlenW(footerRight));
                            }
                        }

                        if (!target->ops->EndPage(target->userData)) goto abort_doc;
                        pageNumber++;
                        if (!target->ops->BeginPage(target->userData, pageNumber)) goto abort_doc;
                        SplitHeaderFooterSegments(ctx->headerText, fileName, pageNumber, totalPages, dateStr, timeStr, headerLeft, ARRAYSIZE(headerLeft), headerCenter, ARRAYSIZE(headerCenter), headerRight, ARRAYSIZE(headerRight));
                        SplitHeaderFooterSegments(ctx->footerText, fileName, pageNumber, totalPages, dateStr, timeStr, footerLeft, ARRAYSIZE(footerLeft), footerCenter, ARRAYSIZE(footerCenter), footerRight, ARRAYSIZE(footerRight));
                        if (headerEnabled) {
                            SIZE ht;
                            if (headerLeft[0]) {
                                target->ops->DrawText(target->userData, marginLeft, marginTop, headerLeft, lstrlenW(headerLeft));
                            }
                            if (headerCenter[0]) {
                                target->ops->MeasureText(target->userData, headerCenter, lstrlenW(headerCenter), &ht);
                                int hx = marginLeft + (printableWidth - ht.cx) / 2;
                                target->ops->DrawText(target->userData, hx, marginTop, headerCenter, lstrlenW(headerCenter));
                            }
                            if (headerRight[0]) {
                                target->ops->MeasureText(target->userData, headerRight, lstrlenW(headerRight), &ht);
                                int rx = pageWidth - marginRight - ht.cx;
                                target->ops->DrawText(target->userData, rx, marginTop, headerRight, lstrlenW(headerRight));
                            }
                        }
                        lineOnPage = 0;
                        y = contentTop;
                    }
                }
            }
        }

        target->ops->DrawText(target->userData, marginLeft, y, lineBuf, bufLen);
        y += lineHeight;
        lineOnPage++;
        if (lineOnPage >= linesPerPage) {
            const WCHAR *peek = p;
            while (*peek == L'\r' || *peek == L'\n') peek++;
            if (*peek) {
                if (footerEnabled && (footerLeft[0] || footerCenter[0] || footerRight[0])) {
                    SIZE ft;
                    if (footerLeft[0]) {
                        target->ops->DrawText(target->userData, marginLeft, pageHeight - marginBottom - footerHeight, footerLeft, lstrlenW(footerLeft));
                    }
                    if (footerCenter[0]) {
                        target->ops->MeasureText(target->userData, footerCenter, lstrlenW(footerCenter), &ft);
                        int cx = marginLeft + (printableWidth - ft.cx) / 2;
                        target->ops->DrawText(target->userData, cx, pageHeight - marginBottom - footerHeight, footerCenter, lstrlenW(footerCenter));
                    }
                    if (footerRight[0]) {
                        target->ops->MeasureText(target->userData, footerRight, lstrlenW(footerRight), &ft);
                        int rx = pageWidth - marginRight - ft.cx;
                        target->ops->DrawText(target->userData, rx, pageHeight - marginBottom - footerHeight, footerRight, lstrlenW(footerRight));
                    }
                }

                if (!target->ops->EndPage(target->userData)) goto abort_doc;
                pageNumber++;
                if (!target->ops->BeginPage(target->userData, pageNumber)) goto abort_doc;
                SplitHeaderFooterSegments(ctx->headerText, fileName, pageNumber, totalPages, dateStr, timeStr, headerLeft, ARRAYSIZE(headerLeft), headerCenter, ARRAYSIZE(headerCenter), headerRight, ARRAYSIZE(headerRight));
                SplitHeaderFooterSegments(ctx->footerText, fileName, pageNumber, totalPages, dateStr, timeStr, footerLeft, ARRAYSIZE(footerLeft), footerCenter, ARRAYSIZE(footerCenter), footerRight, ARRAYSIZE(footerRight));
                if (headerEnabled) {
                    SIZE ht;
                    if (headerLeft[0]) {
                        target->ops->DrawText(target->userData, marginLeft, marginTop, headerLeft, lstrlenW(headerLeft));
                    }
                    if (headerCenter[0]) {
                        target->ops->MeasureText(target->userData, headerCenter, lstrlenW(headerCenter), &ht);
                        int hx = marginLeft + (printableWidth - ht.cx) / 2;
                        target->ops->DrawText(target->userData, hx, marginTop, headerCenter, lstrlenW(headerCenter));
                    }
                    if (headerRight[0]) {
                        target->ops->MeasureText(target->userData, headerRight, lstrlenW(headerRight), &ht);
                        int rx = pageWidth - marginRight - ht.cx;
                        target->ops->DrawText(target->userData, rx, marginTop, headerRight, lstrlenW(headerRight));
                    }
                }
                lineOnPage = 0;
                y = contentTop;
            }
        }

        if (*p == L'\r') {
            p++;
            if (*p == L'\n') p++;
        } else if (*p == L'\n') {
            p++;
        }
    }

    if (footerEnabled && (footerLeft[0] || footerCenter[0] || footerRight[0])) {
        SIZE ft;
        if (footerLeft[0]) {
            target->ops->DrawText(target->userData, marginLeft, pageHeight - marginBottom - footerHeight, footerLeft, lstrlenW(footerLeft));
        }
        if (footerCenter[0]) {
            target->ops->MeasureText(target->userData, footerCenter, lstrlenW(footerCenter), &ft);
            int cx = marginLeft + (printableWidth - ft.cx) / 2;
            target->ops->DrawText(target->userData, cx, pageHeight - marginBottom - footerHeight, footerCenter, lstrlenW(footerCenter));
        }
        if (footerRight[0]) {
            target->ops->MeasureText(target->userData, footerRight, lstrlenW(footerRight), &ft);
            int rx = pageWidth - marginRight - ft.cx;
            target->ops->DrawText(target->userData, rx, pageHeight - marginBottom - footerHeight, footerRight, lstrlenW(footerRight));
        }
    }

    if (!target->ops->EndPage(target->userData)) goto abort_doc;
    if (!target->ops->EndDocument(target->userData)) goto abort_doc;
    success = TRUE;

abort_doc:
    if (!success) {
        target->ops->AbortDocument(target->userData);
    }
    return success;
}

static BOOL HdcGetMetrics(void *userData, PrintMetrics *metrics) {
    if (!userData || !metrics) return FALSE;
    HdcRenderTarget *target = (HdcRenderTarget *)userData;
    TEXTMETRICW tm;
    if (!GetTextMetricsW(target->hdc, &tm)) return FALSE;
    metrics->lineHeight = tm.tmHeight + tm.tmExternalLeading;
    metrics->averageCharWidth = tm.tmAveCharWidth;
    metrics->dpiX = GetDeviceCaps(target->hdc, LOGPIXELSX);
    metrics->dpiY = GetDeviceCaps(target->hdc, LOGPIXELSY);
    return TRUE;
}

static BOOL HdcGetPageSize(void *userData, int *width, int *height) {
    if (!userData || !width || !height) return FALSE;
    HdcRenderTarget *target = (HdcRenderTarget *)userData;
    *width = GetDeviceCaps(target->hdc, HORZRES);
    *height = GetDeviceCaps(target->hdc, VERTRES);
    return TRUE;
}

static BOOL HdcBeginDocument(void *userData, const WCHAR *docName, int totalPages) {
    (void)totalPages;
    if (!userData) return FALSE;
    HdcRenderTarget *target = (HdcRenderTarget *)userData;
    DOCINFOW di = {0};
    di.cbSize = sizeof(di);
    di.lpszDocName = docName;
    if (StartDocW(target->hdc, &di) <= 0) {
        return FALSE;
    }
    target->documentStarted = TRUE;
    return TRUE;
}

static BOOL HdcEndDocument(void *userData) {
    if (!userData) return FALSE;
    HdcRenderTarget *target = (HdcRenderTarget *)userData;
    if (!target->documentStarted) return TRUE;
    BOOL result = (EndDoc(target->hdc) > 0);
    target->documentStarted = FALSE;
    return result;
}

static BOOL HdcAbortDocument(void *userData) {
    if (!userData) return FALSE;
    HdcRenderTarget *target = (HdcRenderTarget *)userData;
    if (!target->documentStarted) return TRUE;
    target->documentStarted = FALSE;
    AbortDoc(target->hdc);
    return TRUE;
}

static BOOL HdcBeginPage(void *userData, int pageNumber) {
    (void)pageNumber;
    if (!userData) return FALSE;
    HdcRenderTarget *target = (HdcRenderTarget *)userData;
    return StartPage(target->hdc) > 0;
}

static BOOL HdcEndPage(void *userData) {
    if (!userData) return FALSE;
    HdcRenderTarget *target = (HdcRenderTarget *)userData;
    return EndPage(target->hdc) > 0;
}

static BOOL HdcDrawText(void *userData, int x, int y, const WCHAR *text, int length) {
    if (!userData || !text) return FALSE;
    HdcRenderTarget *target = (HdcRenderTarget *)userData;
    return TextOutW(target->hdc, x, y, text, length);
}

static BOOL HdcMeasureText(void *userData, const WCHAR *text, int length, SIZE *size) {
    if (!userData || !text || !size) return FALSE;
    HdcRenderTarget *target = (HdcRenderTarget *)userData;
    return GetTextExtentPoint32W(target->hdc, text, length, size);
}

static void SplitHeaderFooterSegments(const WCHAR *format, const WCHAR *fileName, int pageNumber, int totalPages, const WCHAR *dateStr, const WCHAR *timeStr, WCHAR *left, size_t cchLeft, WCHAR *center, size_t cchCenter, WCHAR *right, size_t cchRight) {
    if (left && cchLeft) left[0] = L'\0';
    if (center && cchCenter) center[0] = L'\0';
    if (right && cchRight) right[0] = L'\0';
    if (!format) return;

    enum { SEG_LEFT = 0, SEG_CENTER = 1, SEG_RIGHT = 2 } seg = SEG_LEFT;
    WCHAR *targets[3] = { left, center, right };
    size_t sizes[3] = { cchLeft, cchCenter, cchRight };

    for (const WCHAR *p = format; *p; ++p) {
        if (*p == L'&') {
            WCHAR next = *(p + 1);
            if (next == L'l' || next == L'L') { seg = SEG_LEFT; p++; continue; }
            if (next == L'c' || next == L'C') { seg = SEG_CENTER; p++; continue; }
            if (next == L'r' || next == L'R') { seg = SEG_RIGHT; p++; continue; }

            const WCHAR *insert = NULL;
            WCHAR tmp[2] = {0};
            WCHAR pageBuf[16], totalBuf[16];
            switch (next) {
            case L'&': tmp[0] = L'&'; insert = tmp; p++; break;
            case L'f': case L'F': insert = fileName; p++; break;
            case L'p': StringCchPrintfW(pageBuf, ARRAYSIZE(pageBuf), L"%d", pageNumber); insert = pageBuf; p++; break;
            case L'P': StringCchPrintfW(totalBuf, ARRAYSIZE(totalBuf), L"%d", max(1, totalPages)); insert = totalBuf; p++; break;
            case L'd': case L'D': insert = dateStr; p++; break;
            case L't': case L'T': insert = timeStr; p++; break;
            default:
                tmp[0] = L'&';
                insert = tmp;
                break;
            }

            if (insert && insert[0] && targets[seg] && sizes[seg] > 0) {
                size_t curLen = targets[seg] ? wcslen(targets[seg]) : 0;
                size_t remaining = sizes[seg] > curLen ? sizes[seg] - curLen - 1 : 0;
                if (remaining > 0 && targets[seg]) {
                    size_t len = wcslen(insert);
                    size_t toCopy = min(len, remaining);
                    CopyMemory(targets[seg] + curLen, insert, toCopy * sizeof(WCHAR));
                    targets[seg][curLen + toCopy] = L'\0';
                }
            }
        } else if (targets[seg] && sizes[seg] > 0) {
            size_t curLen = wcslen(targets[seg]);
            if (curLen + 1 < sizes[seg]) {
                targets[seg][curLen] = *p;
                targets[seg][curLen + 1] = L'\0';
            }
        }
    }
}

static int ComputeTotalPages(const WCHAR *text, int charsPerLine, int linesPerPage) {
    if (charsPerLine < 1) charsPerLine = 1;
    if (linesPerPage < 1) linesPerPage = 1;

    const int tabWidth = 8;
    int lineOnPage = 0;
    int pageNumber = 1;
    const WCHAR *p = text;
    while (*p) {
        int col = 0;
        while (*p && *p != L'\r' && *p != L'\n') {
            if (*p == L'\t') {
                int spaces = tabWidth - (col % tabWidth);
                col += spaces;
            } else {
                col++;
            }
            if (col >= charsPerLine) {
                lineOnPage++;
                col = 0;
                if (lineOnPage >= linesPerPage) {
                    const WCHAR *peek = p;
                    while (*peek == L'\r' || *peek == L'\n') peek++;
                    if (*peek) {
                        pageNumber++;
                        lineOnPage = 0;
                    }
                }
            }
            p++;
        }

        lineOnPage++;
        if (lineOnPage >= linesPerPage) {
            const WCHAR *peek = p;
            while (*peek == L'\r' || *peek == L'\n') peek++;
            if (*peek) {
                pageNumber++;
                lineOnPage = 0;
            }
        }

        if (*p == L'\r') {
            p++;
            if (*p == L'\n') p++;
        } else if (*p == L'\n') {
            p++;
        }
    }

    if (!text || text[0] == L'\0') {
        pageNumber = 1;
    }
    return pageNumber;
}
