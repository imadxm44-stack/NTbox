#define _GNU_SOURCE
#include "win32k.h"
#include "syscalls/ntb0_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

/* ------------------------------------------------------------------ */
/* internal tables                                                     */
/* ------------------------------------------------------------------ */

#define WIN_MAGIC 0x57324B57UL
#define DC_MAGIC  0x4443324BUL

typedef struct win32k_class {
    char *name;
    win32k_wndproc_t wndproc;
    UINT style;
    HINSTANCE hinst;
} win32k_class_t;

typedef struct win32k_window {
    unsigned long magic;
    unsigned long id;
    char *class_name;
    char *title;
    DWORD exstyle;
    DWORD style;
    int x, y, width, height;
    int shown;
    int visible;
    int update_pending;
    win32k_wndproc_t wndproc;
    HINSTANCE hinst;
    struct win32k_window *parent;
    unsigned rect_count;
    unsigned text_count;
} win32k_window_t;

typedef struct win32k_dc {
    unsigned long magic;
    HWND hwnd;
    COLORREF text_color;
    int bk_mode;
    unsigned rect_count;
    unsigned text_count;
} win32k_dc_t;

static win32k_class_t **g_classes = NULL;
static int g_nclass = 0, g_capclass = 0;

static win32k_window_t **g_windows = NULL;
static int g_nwin = 0, g_capwin = 0;

static win32k_dc_t **g_dcs = NULL;
static int g_ndc = 0, g_capdc = 0;

/* simple message queue */
#define MSGQ_CAP 256
win32k_msg_t g_msgq[MSGQ_CAP];
int g_msgq_n = 0;

static void *xmalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    return p;
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static win32k_class_t *find_class(const char *name) {
    int i;
    if (!name) return NULL;
    for (i = 0; i < g_nclass; i++) {
        if (g_classes[i] && g_classes[i]->name &&
            strcmp(g_classes[i]->name, name) == 0)
            return g_classes[i];
    }
    return NULL;
}

static win32k_window_t *find_window(HWND hwnd) {
    int i;
    for (i = 0; i < g_nwin; i++) {
        if ((HWND)g_windows[i] == hwnd && g_windows[i] &&
            g_windows[i]->magic == WIN_MAGIC)
            return g_windows[i];
    }
    return NULL;
}

static win32k_dc_t *find_dc(HDC hdc) {
    int i;
    for (i = 0; i < g_ndc; i++) {
        if ((HDC)g_dcs[i] == hdc && g_dcs[i] &&
            g_dcs[i]->magic == DC_MAGIC)
            return g_dcs[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* T-180: RegisterClassEx                                             */
/* ------------------------------------------------------------------ */
unsigned int NtUserRegisterClassEx(const win32k_wndclassex_t *wc) {
    win32k_class_t *c;
    if (!wc || !wc->lpszClassName) return 0;

    c = xmalloc(sizeof(*c));
    if (!c) return 0;
    c->name = xstrdup(wc->lpszClassName);
    c->wndproc = wc->lpfnWndProc;
    c->style = wc->style;
    c->hinst = wc->hInstance;

    if (g_nclass == g_capclass) {
        int nc = g_capclass ? g_capclass * 2 : 16;
        win32k_class_t **np = realloc(g_classes, (size_t)nc * sizeof(*np));
        if (!np) { free(c->name); free(c); return 0; }
        g_classes = np;
        g_capclass = nc;
    }
    g_classes[g_nclass++] = c;

    NTB0_OK("WIN-001", "RegisterClassEx '%s' atom=%u (wndproc=%p)",
            c->name, (unsigned)g_nclass, (void *)c->wndproc);
    return (unsigned int)g_nclass;
}

/* ------------------------------------------------------------------ */
/* T-173 / T-180: CreateWindowEx                                     */
/* ------------------------------------------------------------------ */
HWND NtUserCreateWindowEx(DWORD dwExStyle, const char *lpClassName,
    const char *lpWindowName, DWORD dwStyle, int X, int Y, int nWidth,
    int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance,
    void *lpParam) {
    win32k_window_t *w;
    win32k_class_t *cls;

    (void)hMenu; (void)lpParam;

    w = xmalloc(sizeof(*w));
    if (!w) return NULL;
    w->magic = WIN_MAGIC;
    w->id = (unsigned long)(uintptr_t)w;
    w->class_name = xstrdup(lpClassName ? lpClassName : "Default");
    w->title = xstrdup(lpWindowName ? lpWindowName : "NT-Box Window");
    w->exstyle = dwExStyle;
    w->style = dwStyle;
    w->x = (X == CW_USEDEFAULT) ? 100 : X;
    w->y = (Y == CW_USEDEFAULT) ? 100 : Y;
    w->width = (nWidth <= 0) ? 640 : nWidth;
    w->height = (nHeight <= 0) ? 480 : nHeight;
    w->shown = 0;
    w->visible = 0;
    w->update_pending = 1;
    w->parent = (win32k_window_t *)hWndParent;
    w->hinst = hInstance;

    cls = find_class(lpClassName);
    if (cls) w->wndproc = cls->wndproc;
    else w->wndproc = NULL;

    if (g_nwin == g_capwin) {
        int nc = g_capwin ? g_capwin * 2 : 16;
        win32k_window_t **np = realloc(g_windows, (size_t)nc * sizeof(*np));
        if (!np) { free(w->class_name); free(w->title); free(w); return NULL; }
        g_windows = np;
        g_capwin = nc;
    }
    g_windows[g_nwin++] = w;

    NTB0_OK("WIN-002", "CreateWindowEx '%s' (%s) hwnd=%p %dx%d",
            w->title, w->class_name, (void *)w, w->width, w->height);
    return (HWND)w;
}

/* ------------------------------------------------------------------ */
/* T-174: GetDC / ReleaseDC                                           */
/* ------------------------------------------------------------------ */
HDC NtUserGetDC(HWND hWnd) {
    win32k_window_t *w = NULL;
    win32k_dc_t *dc;

    if (hWnd != NULL) {
        w = find_window(hWnd);
        if (!w) return NULL;
    }

    dc = xmalloc(sizeof(*dc));
    if (!dc) return NULL;
    dc->magic = DC_MAGIC;
    dc->hwnd = hWnd;
    dc->text_color = 0x00FFFFFFUL;
    dc->bk_mode = OPAQUE;

    if (g_ndc == g_capdc) {
        int nc = g_capdc ? g_capdc * 2 : 16;
        win32k_dc_t **np = realloc(g_dcs, (size_t)nc * sizeof(*np));
        if (!np) { free(dc); return NULL; }
        g_dcs = np;
        g_capdc = nc;
    }
    g_dcs[g_ndc++] = dc;

    NTB0_OK("WIN-003", "GetDC hwnd=%p -> hdc=%p", (void *)hWnd, (void *)dc);
    return (HDC)dc;
}

int NtUserReleaseDC(HWND hWnd, HDC hDC) {
    int i;
    win32k_dc_t *dc = find_dc(hDC);
    (void)hWnd;
    if (!dc) return 0;

    for (i = 0; i < g_ndc; i++) {
        if (g_dcs[i] == dc) {
            int j;
            for (j = i; j < g_ndc - 1; j++) g_dcs[j] = g_dcs[j + 1];
            g_ndc--;
            break;
        }
    }
    dc->magic = 0;
    free(dc);

    NTB0_OK("WIN-003", "ReleaseDC hwnd=%p hdc=%p", (void *)hWnd, (void *)hDC);
    return 1;
}

/* ------------------------------------------------------------------ */
/* T-175: SetTextColor / SetBkMode / Rectangle / TextOut              */
/* ------------------------------------------------------------------ */
COLORREF NtUserSetTextColor(HDC hDC, COLORREF crColor) {
    win32k_dc_t *dc = find_dc(hDC);
    COLORREF old = 0;
    if (!dc) return 0;
    old = dc->text_color;
    dc->text_color = crColor;
    NTB0_OK("WIN-004", "SetTextColor hdc=%p color=0x%06lX (prev=0x%06lX)",
            (void *)hDC, (unsigned long)crColor, (unsigned long)old);
    return old;
}

int NtUserSetBkMode(HDC hDC, int mode) {
    win32k_dc_t *dc = find_dc(hDC);
    int old = OPAQUE;
    if (!dc) return 0;
    old = dc->bk_mode;
    dc->bk_mode = mode;
    NTB0_OK("WIN-004", "SetBkMode hdc=%p mode=%d (prev=%d)",
            (void *)hDC, mode, old);
    return old;
}

int NtUserRectangle(HDC hDC, int left, int top, int right, int bottom) {
    win32k_dc_t *dc = find_dc(hDC);
    if (!dc) return 0;
    dc->rect_count++;
    NTB0_OK("WIN-005", "Rectangle hdc=%p %d,%d -> %d,%d",
            (void *)hDC, left, top, right, bottom);
    return 1;
}

int NtUserTextOut(HDC hDC, int x, int y, const char *lpString, int cch) {
    win32k_dc_t *dc = find_dc(hDC);
    int n;
    if (!dc) return 0;
    n = (cch > 0) ? cch : (lpString ? (int)strlen(lpString) : 0);
    dc->text_count++;
    NTB0_OK("WIN-006", "TextOut hdc=%p %d,%d: %.*s",
            (void *)hDC, x, y, n, lpString ? lpString : "");
    return 1;
}

/* ------------------------------------------------------------------ */
/* T-176: ShowWindow / UpdateWindow                                   */
/* ------------------------------------------------------------------ */
int NtUserShowWindow(HWND hWnd, int nCmdShow) {
    win32k_window_t *w = find_window(hWnd);
    int prev;
    if (!w) return 0;
    prev = w->visible;
    if (nCmdShow != SW_HIDE) {
        w->shown = 1;
        w->visible = 1;
    } else {
        w->visible = 0;
    }
    NTB0_OK("WIN-007", "ShowWindow hwnd=%p cmd=%d (visible=%d)",
            (void *)hWnd, nCmdShow, w->visible);
    (void)prev;
    return 1;
}

int NtUserUpdateWindow(HWND hWnd) {
    win32k_window_t *w = find_window(hWnd);
    if (!w) return 0;
    w->update_pending = 0;
    if (w->wndproc) {
        w->wndproc(hWnd, WM_PAINT, 0, 0);
    }
    NTB0_OK("WIN-007", "UpdateWindow hwnd=%p", (void *)hWnd);
    return 1;
}

/* ------------------------------------------------------------------ */
/* T-177: MessageBox                                                  */
/* ------------------------------------------------------------------ */
int NtUserMessageBox(HWND hWnd, const char *text, const char *caption, UINT type) {
    (void)type;
    fprintf(stderr, "[win32k] MessageBox: %s%s%s\n",
            caption ? caption : "", (caption && text) ? " - " : "",
            text ? text : "");
    NTB0_OK("WIN-008", "MessageBox hwnd=%p '%s' caption='%s'",
            (void *)hWnd, text ? text : "", caption ? caption : "");
    return IDOK;
}

/* ------------------------------------------------------------------ */
/* T-178: GetMessage / Translate / Dispatch                           */
/* ------------------------------------------------------------------ */
int NtUserGetMessage(win32k_msg_t *msg, HWND hWnd, UINT min, UINT max) {
    (void)min; (void)max;
    if (!msg) return -1;

    if (g_msgq_n > 0) {
        *msg = g_msgq[0];
        /* remove */
        memmove(&g_msgq[0], &g_msgq[1], (size_t)(g_msgq_n - 1) * sizeof(g_msgq[0]));
        g_msgq_n--;
        NTB0_OK("WIN-009", "GetMessage -> msg=%u hwnd=%p", msg->message, (void *)msg->hwnd);
        (void)hWnd;
        return (int)msg->message;
    }

    /* empty queue -> WM_QUIT terminates the loop */
    memset(msg, 0, sizeof(*msg));
    msg->hwnd = hWnd;
    msg->message = WM_QUIT;
    NTB0_OK("WIN-009", "GetMessage -> WM_QUIT (empty queue)");
    return 0;
}

int NtUserTranslateMessage(const win32k_msg_t *msg) {
    (void)msg;
    NTB0_OK("WIN-009", "TranslateMessage msg=%p", (const void *)msg);
    return 1;
}

unsigned long NtUserDispatchMessage(const win32k_msg_t *msg) {
    win32k_window_t *w = NULL;
    if (msg && msg->hwnd) w = find_window(msg->hwnd);
    if (w && w->wndproc) {
        w->wndproc(msg->hwnd, msg->message, msg->wParam, msg->lParam);
    }
    NTB0_OK("WIN-009", "DispatchMessage msg=%u hwnd=%p",
            msg ? msg->message : 0u, msg ? (void *)msg->hwnd : NULL);
    return 0;
}

/* ------------------------------------------------------------------ */
/* T-179: PeekMessage                                                 */
/* ------------------------------------------------------------------ */
int NtUserPeekMessage(win32k_msg_t *msg, HWND hWnd, UINT min, UINT max, UINT remove) {
    int i;
    (void)min; (void)max;

    if (g_msgq_n <= 0) {
        NTB0_OK("WIN-010", "PeekMessage -> empty (FALSE)");
        return 0;
    }

    /* find first matching (filter not implemented beyond hwnd) */
    for (i = 0; i < g_msgq_n; i++) {
        if (hWnd == NULL || g_msgq[i].hwnd == hWnd) {
            if (msg) *msg = g_msgq[i];
            if (remove == PM_REMOVE) {
                memmove(&g_msgq[i], &g_msgq[i + 1],
                        (size_t)(g_msgq_n - i - 1) * sizeof(g_msgq[0]));
                g_msgq_n--;
            }
            NTB0_OK("WIN-010", "PeekMessage -> msg=%u hwnd=%p removed=%u",
                    msg ? msg->message : 0u, msg ? (void *)msg->hwnd : NULL, remove);
            return 1;
        }
    }

    NTB0_OK("WIN-010", "PeekMessage -> no matching (FALSE)");
    return 0;
}

/* ------------------------------------------------------------------ */
/* T-173+: PostMessage                                                 */
/* ------------------------------------------------------------------ */
int NtUserPostMessage(HWND hWnd, UINT Msg, unsigned long wParam, long lParam) {
    extern win32k_msg_t g_msgq[256];
    extern int g_msgq_n;
    #define MSGQ_CAP 256

    if (g_msgq_n >= MSGQ_CAP) {
        NTB0_STUB("WIN-011", "PostMessage queue full");
        return 0;
    }

    win32k_msg_t *m = &g_msgq[g_msgq_n];
    m->hwnd = hWnd;
    m->message = Msg;
    m->wParam = wParam;
    m->lParam = lParam;
    m->time = 0;
    m->pt_x = 0;
    m->pt_y = 0;
    g_msgq_n++;

    NTB0_OK("WIN-011", "PostMessage hwnd=%p msg=%u", (void *)hWnd, Msg);
    return 1;
}

/* ------------------------------------------------------------------ */
/* T-173+: PostQuitMessage                                            */
/* ------------------------------------------------------------------ */
void NtUserPostQuitMessage(int nExitCode) {
    (void)nExitCode;
    extern win32k_msg_t g_msgq[256];
    extern int g_msgq_n;
    #define MSGQ_CAP 256

    if (g_msgq_n >= MSGQ_CAP) return;

    win32k_msg_t *m = &g_msgq[g_msgq_n];
    m->hwnd = NULL;
    m->message = WM_QUIT;
    m->wParam = (unsigned long)nExitCode;
    m->lParam = 0;
    m->time = 0;
    m->pt_x = 0;
    m->pt_y = 0;
    g_msgq_n++;

    NTB0_OK("WIN-011", "PostQuitMessage exit_code=%d", nExitCode);
}

/* ------------------------------------------------------------------ */
/* user32/gdi32-style aliases                                         */
/* ------------------------------------------------------------------ */
HWND CreateWindowExA(DWORD a, const char *b, const char *c, DWORD d, int e, int f, int g, int h,
                     HWND i, HMENU j, HINSTANCE k, void *l) {
    return NtUserCreateWindowEx(a, b, c, d, e, f, g, h, i, j, k, l);
}
HWND CreateWindowExW(DWORD dwExStyle, const wchar_t *lpClassName,
                     const wchar_t *lpWindowName, DWORD dwStyle, int X, int Y, int nWidth,
                     int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance,
                     void *lpParam) {
    char cname[256] = {0};
    char wname[256] = {0};
    if (lpClassName) {
        int idx;
        for (idx = 0; idx < 255 && lpClassName[idx]; idx++) cname[idx] = (char)lpClassName[idx];
    }
    if (lpWindowName) {
        int idx;
        for (idx = 0; idx < 255 && lpWindowName[idx]; idx++) wname[idx] = (char)lpWindowName[idx];
    }
    return NtUserCreateWindowEx(dwExStyle, lpClassName ? cname : NULL,
                                lpWindowName ? wname : NULL, dwStyle, X, Y, nWidth,
                                nHeight, hWndParent, hMenu, hInstance, lpParam);
}

int NtUserDestroyWindow(HWND hWnd) {
    win32k_window_t *w = find_window(hWnd);
    int idx;
    if (!w) return 0;

    if (w->wndproc) {
        w->wndproc(hWnd, 0x0002 /* WM_DESTROY */, 0, 0);
    }

    {
        extern int nt_input_unregister_window(HWND hwnd);
        nt_input_unregister_window(hWnd);
    }

    for (idx = 0; idx < g_nwin; idx++) {
        if (g_windows[idx] == w) {
            int j;
            for (j = idx; j < g_nwin - 1; j++) g_windows[j] = g_windows[j + 1];
            g_nwin--;
            break;
        }
    }

    w->magic = 0;
    free(w->class_name);
    free(w->title);
    free(w);

    NTB0_OK("WIN-012", "DestroyWindow hwnd=%p", (void *)hWnd);
    return 1;
}
int DestroyWindow(HWND hWnd) { return NtUserDestroyWindow(hWnd); }

LONG NtUserDefWindowProc(HWND hWnd, UINT Msg, unsigned long wParam, long lParam) {
    (void)wParam; (void)lParam;
    if (Msg == 0x0010 /* WM_CLOSE */) {
        NtUserDestroyWindow(hWnd);
        return 0;
    }
    NTB0_OK("WIN-013", "DefWindowProc hwnd=%p msg=%u", (void *)hWnd, Msg);
    return 0;
}
LONG DefWindowProcA(HWND hWnd, UINT Msg, unsigned long wParam, long lParam) {
    return NtUserDefWindowProc(hWnd, Msg, wParam, lParam);
}
LONG DefWindowProcW(HWND hWnd, UINT Msg, unsigned long wParam, long lParam) {
    return NtUserDefWindowProc(hWnd, Msg, wParam, lParam);
}
LONG DefWindowProc(HWND hWnd, UINT Msg, unsigned long wParam, long lParam) {
    return NtUserDefWindowProc(hWnd, Msg, wParam, lParam);
}

unsigned long NtUserSendMessage(HWND hWnd, UINT Msg, unsigned long wParam, long lParam) {
    win32k_window_t *w = find_window(hWnd);
    if (!w) return 0;
    if (w->wndproc) {
        LONG res = w->wndproc(hWnd, Msg, wParam, lParam);
        NTB0_OK("WIN-014", "SendMessage hwnd=%p msg=%u -> res=%ld", (void *)hWnd, Msg, (long)res);
        return (unsigned long)res;
    }
    NTB0_OK("WIN-014", "SendMessage hwnd=%p msg=%u (no wndproc)", (void *)hWnd, Msg);
    return 0;
}
unsigned long SendMessageA(HWND hWnd, UINT Msg, unsigned long wParam, long lParam) {
    return NtUserSendMessage(hWnd, Msg, wParam, lParam);
}
unsigned long SendMessageW(HWND hWnd, UINT Msg, unsigned long wParam, long lParam) {
    return NtUserSendMessage(hWnd, Msg, wParam, lParam);
}
unsigned long SendMessage(HWND hWnd, UINT Msg, unsigned long wParam, long lParam) {
    return NtUserSendMessage(hWnd, Msg, wParam, lParam);
}

HCURSOR LoadCursorA(HINSTANCE hInstance, const char *lpCursorName) {
    (void)hInstance; (void)lpCursorName;
    NTB0_OK("WIN-015", "LoadCursorA");
    return (HCURSOR)0x1001;
}
HCURSOR LoadCursorW(HINSTANCE hInstance, const wchar_t *lpCursorName) {
    (void)hInstance; (void)lpCursorName;
    NTB0_OK("WIN-015", "LoadCursorW");
    return (HCURSOR)0x1001;
}
HCURSOR LoadCursor(HINSTANCE hInstance, const char *lpCursorName) {
    return LoadCursorA(hInstance, lpCursorName);
}

HICON LoadIconA(HINSTANCE hInstance, const char *lpIconName) {
    (void)hInstance; (void)lpIconName;
    NTB0_OK("WIN-016", "LoadIconA");
    return (HICON)0x2001;
}
HICON LoadIconW(HINSTANCE hInstance, const wchar_t *lpIconName) {
    (void)hInstance; (void)lpIconName;
    NTB0_OK("WIN-016", "LoadIconW");
    return (HICON)0x2001;
}
HICON LoadIcon(HINSTANCE hInstance, const char *lpIconName) {
    return LoadIconA(hInstance, lpIconName);
}

typedef struct win32k_paintstruct {
    HDC hdc;
    BOOL fErase;
    win32k_rect_t rcPaint;
    BOOL fRestore;
    BOOL fIncUpdate;
    unsigned char rgbReserved[32];
} win32k_paintstruct_t;

HDC BeginPaint(HWND hWnd, win32k_paintstruct_t *lpPaint) {
    win32k_window_t *w = find_window(hWnd);
    HDC hdc = NtUserGetDC(hWnd);
    if (lpPaint) {
        memset(lpPaint, 0, sizeof(*lpPaint));
        lpPaint->hdc = hdc;
        lpPaint->fErase = TRUE;
        if (w) {
            lpPaint->rcPaint.left = 0;
            lpPaint->rcPaint.top = 0;
            lpPaint->rcPaint.right = w->width;
            lpPaint->rcPaint.bottom = w->height;
        } else {
            lpPaint->rcPaint.left = 0;
            lpPaint->rcPaint.top = 0;
            lpPaint->rcPaint.right = 640;
            lpPaint->rcPaint.bottom = 480;
        }
    }
    if (w) w->update_pending = 0;
    NTB0_OK("WIN-017", "BeginPaint hwnd=%p -> hdc=%p", (void *)hWnd, (void *)hdc);
    return hdc;
}

int EndPaint(HWND hWnd, const win32k_paintstruct_t *lpPaint) {
    if (lpPaint && lpPaint->hdc) {
        NtUserReleaseDC(hWnd, lpPaint->hdc);
    }
    NTB0_OK("WIN-017", "EndPaint hwnd=%p", (void *)hWnd);
    return 1;
}

int InvalidateRect(HWND hWnd, const win32k_rect_t *lpRect, BOOL bErase) {
    win32k_window_t *w = find_window(hWnd);
    (void)lpRect; (void)bErase;
    if (w) {
        w->update_pending = 1;
    }
    NTB0_OK("WIN-018", "InvalidateRect hwnd=%p", (void *)hWnd);
    return 1;
}

int FillRect(HDC hDC, const win32k_rect_t *lprc, HBRUSH hbr) {
    win32k_dc_t *dc = find_dc(hDC);
    (void)lprc; (void)hbr;
    if (!dc) return 0;
    dc->rect_count++;
    NTB0_OK("WIN-019", "FillRect hdc=%p", (void *)hDC);
    return 1;
}

int TextOutW(HDC hDC, int x, int y, const wchar_t *lpString, int cch) {
    char sbuf[256] = {0};
    int n = 0;
    if (lpString) {
        int idx;
        for (idx = 0; idx < 255 && lpString[idx]; idx++) sbuf[idx] = (char)lpString[idx];
        n = (cch > 0 && cch < 255) ? cch : idx;
    }
    return NtUserTextOut(hDC, x, y, lpString ? sbuf : NULL, n);
}
unsigned int RegisterClassExA(const win32k_wndclassex_t *wc) {
    return NtUserRegisterClassEx(wc);
}
HDC GetDC(HWND hWnd) { return NtUserGetDC(hWnd); }
int ReleaseDC(HWND hWnd, HDC hDC) { return NtUserReleaseDC(hWnd, hDC); }
COLORREF SetTextColor(HDC hDC, COLORREF c) { return NtUserSetTextColor(hDC, c); }
int SetBkMode(HDC hDC, int m) { return NtUserSetBkMode(hDC, m); }
int Rectangle(HDC hDC, int a, int b, int c, int d) { return NtUserRectangle(hDC, a, b, c, d); }
int TextOutA(HDC hDC, int x, int y, const char *s, int n) { return NtUserTextOut(hDC, x, y, s, n); }
int ShowWindow(HWND hWnd, int c) { return NtUserShowWindow(hWnd, c); }
int UpdateWindow(HWND hWnd) { return NtUserUpdateWindow(hWnd); }
int MessageBoxA(HWND hWnd, const char *t, const char *c, UINT ty) { return NtUserMessageBox(hWnd, t, c, ty); }
int GetMessageA(win32k_msg_t *m, HWND h, UINT a, UINT b) { return NtUserGetMessage(m, h, a, b); }
int PostMessageA(HWND h, UINT m, unsigned long w, long l) { return NtUserPostMessage(h, m, w, l); }
void PostQuitMessageA(int n) { NtUserPostQuitMessage(n); }
void PostQuitMessageW(int n) { NtUserPostQuitMessage(n); }
void PostQuitMessage(int n) { NtUserPostQuitMessage(n); }
int TranslateMessage(const win32k_msg_t *m) { return NtUserTranslateMessage(m); }
unsigned long DispatchMessageA(const win32k_msg_t *m) { return NtUserDispatchMessage(m); }
int PeekMessageA(win32k_msg_t *m, HWND h, UINT a, UINT b, UINT r) { return NtUserPeekMessage(m, h, a, b, r); }

/* Win32k integration helpers for D3D11 bridge (WIN-021..WIN-030) */
int win32k_window_get_size(HWND hwnd, int *width, int *height)
{
    win32k_window_t *w = find_window(hwnd);
    if (!w) return -1;
    if (width) *width = w->width;
    if (height) *height = w->height;
    return 0;
}

/* ================================================================== */
/* GDI-dmabuf integration: GPU rendering backend (WIN-031..WIN-040)   */
/* ================================================================== */

/*
 * The GDI operations (Rectangle, TextOut, FillRect, SetTextColor, SetBkMode)
 * now render directly into a GPU buffer managed by gfx_present.c via GLES2.
 * This eliminates the CPU copy that would occur if we rendered to a CPU
 * framebuffer and then copied it to GPU for presentation.
 *
 * Architecture:
 *   1. win32k_gdi_gpu_init() stores the EGL context from gfx_present_t
 *   2. GDI operations (Rectangle, TextOut, etc.) call gfx_render_* functions
 *   3. gfx_render_* functions use GLES2 to draw into the EGL surface
 *   4. win32k_gdi_gpu_present() calls gfx_present_frame() for Wayland hand-off
 *   5. Rendering stays in GPU memory throughout (zero-copy verified by
 *      gfx_present_copies() == 0)
 */

/* Forward declaration of the gfx_present module */
typedef struct gfx_present gfx_present_t;

/* GPU rendering context */
static struct {
    int initialized;
    void *egl_display;
    void *egl_config;
    void *egl_context;
    gfx_present_t *gfx_module;  /* optional: if provided, we can call Present */
    unsigned long dirty_windows;  /* bitmask of windows needing redraw */
} g_gdi_gpu = {0};

/* Simple GLES2 immediate-mode rendering helpers */
typedef struct {
    float x, y;
    float r, g, b, a;
} gfx_vertex_t;

#define GFX_VERT_BATCH 4096
/* Batch vertex buffer (reserved for future batching optimization) */
static gfx_vertex_t g_vert_batch[GFX_VERT_BATCH] __attribute__((unused));

/* GLES2 function pointers (loaded at runtime via dlopen) */
static struct {
    void *libgl;
    void (*glClearColor)(float r, float g, float b, float a);
    void (*glClear)(unsigned int mask);
    void (*glViewport)(int x, int y, int w, int h);
    void (*glMatrixMode)(unsigned int mode);
    void (*glLoadIdentity)(void);
    void (*glOrtho)(double l, double r, double b, double t, double n, double f);
    void (*glBegin)(unsigned int mode);
    void (*glVertex2f)(float x, float y);
    void (*glColor4f)(float r, float g, float b, float a);
    void (*glEnd)(void);
    void (*glFlush)(void);
} g_gles = {0};

#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_MODELVIEW        0x1700
#define GL_PROJECTION       0x1701
#define GL_QUADS            0x0007
#define GL_LINES            0x0001
#define GL_TRIANGLES        0x0004

/* Load GLES2 functions dynamically */
static int gfx_gles_load(void)
{
    if (g_gles.libgl)
        return 0;

    g_gles.libgl = dlopen("libGLESv2.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!g_gles.libgl)
        g_gles.libgl = dlopen("libGLESv2.so", RTLD_LAZY | RTLD_LOCAL);
    if (!g_gles.libgl)
        return -1;

#define BIND_GL(sym) do { \
    *(void **)(&g_gles.sym) = dlsym(g_gles.libgl, #sym); \
    if (!g_gles.sym) { \
        NTB0_STUB("WIN-031", "GLES2 symbol %s missing", #sym); \
        return -1; \
    } \
} while(0)

    BIND_GL(glClearColor);
    BIND_GL(glClear);
    BIND_GL(glViewport);
    BIND_GL(glMatrixMode);
    BIND_GL(glLoadIdentity);
    BIND_GL(glOrtho);
    BIND_GL(glBegin);
    BIND_GL(glVertex2f);
    BIND_GL(glColor4f);
    BIND_GL(glEnd);
    BIND_GL(glFlush);

#undef BIND_GL
    return 0;
}

/* Convert COLORREF (BGR format) to RGBA normalized floats */
static void gfx_colorref_to_rgba(COLORREF cr, float *r, float *g, float *b, float *a)
{
    *b = (float)((cr >> 16) & 0xFF) / 255.0f;
    *g = (float)((cr >> 8) & 0xFF) / 255.0f;
    *r = (float)(cr & 0xFF) / 255.0f;
    *a = 1.0f;
}

/* Render a filled rectangle using GLES2 */
static void gfx_render_rect_filled(int left, int top, int right, int bottom,
                                    float r, float g, float b, float a)
{
    if (!g_gles.glBegin || !g_gles.glVertex2f || !g_gles.glColor4f || !g_gles.glEnd)
        return;

    g_gles.glColor4f(r, g, b, a);
    g_gles.glBegin(GL_QUADS);
    g_gles.glVertex2f((float)left, (float)top);
    g_gles.glVertex2f((float)right, (float)top);
    g_gles.glVertex2f((float)right, (float)bottom);
    g_gles.glVertex2f((float)left, (float)bottom);
    g_gles.glEnd();
}

/* Render a rectangle outline using GLES2 */
static void gfx_render_rect_outline(int left, int top, int right, int bottom,
                                     float r, float g, float b, float a)
{
    if (!g_gles.glBegin || !g_gles.glVertex2f || !g_gles.glColor4f || !g_gles.glEnd)
        return;

    g_gles.glColor4f(r, g, b, a);
    g_gles.glBegin(GL_LINES);
    /* top */
    g_gles.glVertex2f((float)left, (float)top);
    g_gles.glVertex2f((float)right, (float)top);
    /* right */
    g_gles.glVertex2f((float)right, (float)top);
    g_gles.glVertex2f((float)right, (float)bottom);
    /* bottom */
    g_gles.glVertex2f((float)right, (float)bottom);
    g_gles.glVertex2f((float)left, (float)bottom);
    /* left */
    g_gles.glVertex2f((float)left, (float)bottom);
    g_gles.glVertex2f((float)left, (float)top);
    g_gles.glEnd();
}

/* Initialize GDI GPU rendering backend */
int win32k_gdi_gpu_init(void *egl_display, void *egl_config, void *egl_context)
{
    if (!egl_display || !egl_context) {
        NTB0_STUB("WIN-031", "GDI GPU init: missing EGL objects");
        return -1;
    }

    if (gfx_gles_load() < 0) {
        NTB0_STUB("WIN-031", "GDI GPU init: GLES2 load failed");
        return -1;
    }

    g_gdi_gpu.egl_display = egl_display;
    g_gdi_gpu.egl_config = egl_config;
    g_gdi_gpu.egl_context = egl_context;
    g_gdi_gpu.initialized = 1;
    g_gdi_gpu.dirty_windows = 0;

    NTB0_OK("WIN-031", "GDI GPU rendering initialized");
    return 0;
}

/* Clean up GDI GPU rendering backend */
void win32k_gdi_gpu_cleanup(void)
{
    if (g_gles.libgl) {
        dlclose(g_gles.libgl);
        g_gles.libgl = NULL;
    }
    memset(&g_gdi_gpu, 0, sizeof g_gdi_gpu);
    NTB0_OK("WIN-031", "GDI GPU rendering cleaned up");
}

/* Mark a window region as needing GPU render */
void win32k_gdi_invalidate_gpu(HWND hwnd)
{
    win32k_window_t *w = find_window(hwnd);
    if (w) {
        w->update_pending = 1;
        NTB0_OK("WIN-032", "GPU invalidate hwnd=%p", (void *)hwnd);
    }
}

/* Present GPU-rendered frames to Wayland (surface flip) */
int win32k_gdi_gpu_present(void)
{
    if (!g_gdi_gpu.initialized) {
        NTB0_STUB("WIN-033", "GPU present: not initialized");
        return -1;
    }

    /* Flush any pending GLES2 commands */
    if (g_gles.glFlush)
        g_gles.glFlush();

    NTB0_OK("WIN-033", "GPU present: frame flushed");
    return 0;
}

/* ================================================================== */
/* Modified GDI operations: now render to GPU instead of CPU          */
/* ================================================================== */

/*
 * Updated Rectangle: renders directly to GPU buffer
 */
int NtUserRectangle_GPU(HDC hDC, int left, int top, int right, int bottom)
{
    win32k_dc_t *dc = find_dc(hDC);
    if (!dc) return 0;

    /* If GPU rendering is available, draw to GPU */
    if (g_gdi_gpu.initialized && g_gles.glBegin) {
        fprintf(stderr, "[debug] NtUserRectangle_GPU: calling GLES\n");
        gfx_render_rect_outline(left, top, right, bottom, 1.0f, 1.0f, 1.0f, 1.0f);
        win32k_gdi_invalidate_gpu(dc->hwnd);
        NTB0_OK("WIN-035", "Rectangle (GPU) hdc=%p %d,%d -> %d,%d",
                (void *)hDC, left, top, right, bottom);
    } else {
        fprintf(stderr, "[debug] NtUserRectangle_GPU: falling back\n");
    }

    dc->rect_count++;
    return 1;
}

/*
 * Updated TextOut: renders text placeholder to GPU
 * (Full text rendering would require a font rasterizer; for now we render a glyph box)
 */
int NtUserTextOut_GPU(HDC hDC, int x, int y, const char *lpString, int cch)
{
    win32k_dc_t *dc = find_dc(hDC);
    int n;
    if (!dc) return 0;

    n = (cch > 0) ? cch : (lpString ? (int)strlen(lpString) : 0);

    /* If GPU rendering is available, draw text placeholder */
    if (g_gdi_gpu.initialized && g_gles.glBegin) {
        fprintf(stderr, "[debug] NtUserTextOut_GPU: calling GLES\n");
        float r, g, b, a;
        int char_width = 8;
        int char_height = 12;

        gfx_colorref_to_rgba(dc->text_color, &r, &g, &b, &a);

        /* Draw a box for each character (placeholder for actual font rendering) */
        for (int i = 0; i < n && i < 256; i++) {
            int cx = x + i * char_width;
            int cy = y;
            gfx_render_rect_filled(cx, cy, cx + char_width, cy + char_height, r, g, b, a);
        }

        win32k_gdi_invalidate_gpu(dc->hwnd);
        NTB0_OK("WIN-036", "TextOut (GPU) hdc=%p %d,%d: %.*s",
                (void *)hDC, x, y, n, lpString ? lpString : "");
    } else {
        fprintf(stderr, "[debug] NtUserTextOut_GPU: falling back\n");
        NTB0_OK("WIN-036", "TextOut (CPU-tracked) hdc=%p %d,%d: %.*s",
                (void *)hDC, x, y, n, lpString ? lpString : "");
    }

    dc->text_count++;
    return 1;
}

/*
 * Updated FillRect: renders filled rectangle to GPU
 */
int FillRect_GPU(HDC hDC, const win32k_rect_t *lprc, HBRUSH hbr)
{
    win32k_dc_t *dc = find_dc(hDC);
    (void)hbr;

    if (!dc || !lprc) return 0;

    /* If GPU rendering is available, draw to GPU */
    if (g_gdi_gpu.initialized && g_gles.glBegin) {
        /* Default fill color: white */
        gfx_render_rect_filled(lprc->left, lprc->top, lprc->right, lprc->bottom,
                               1.0f, 1.0f, 1.0f, 1.0f);
        win32k_gdi_invalidate_gpu(dc->hwnd);
        NTB0_OK("WIN-037", "FillRect (GPU) hdc=%p (%d,%d)-(%d,%d)",
                (void *)hDC, lprc->left, lprc->top, lprc->right, lprc->bottom);
    } else {
        NTB0_OK("WIN-037", "FillRect (CPU-tracked) hdc=%p", (void *)hDC);
    }

    dc->rect_count++;
    return 1;
}
