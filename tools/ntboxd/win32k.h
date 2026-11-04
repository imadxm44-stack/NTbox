/*
 * win32k.h — minimal Win32k window manager for NT-Box (W8, T-173..T-180).
 */
#ifndef NTBOX_WIN32K_H
#define NTBOX_WIN32K_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *HWND;
typedef void *HDC;
typedef void *HINSTANCE;
typedef void *HICON;
typedef void *HCURSOR;
typedef void *HBRUSH;
typedef void *HMENU;
typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef long LONG;
typedef int BOOL;
typedef unsigned long COLORREF;
typedef uint16_t WORD;

#define TRUE  1
#define FALSE 0

#define CW_USEDEFAULT ((int)0x80000000)
#define SW_HIDE          0
#define SW_SHOWNORMAL    1
#define SW_SHOW          5
#define SW_SHOWDEFAULT  10

#define MB_OK            0x00000000u
#define MB_OKCANCEL      0x00000001u
#define MB_ICONINFORMATION 0x00000040u
#define IDOK             1

#define OPAQUE           2
#define TRANSPARENT      1

#define WM_NULL          0u
#define WM_PAINT        15u
#define WM_CLOSE        16u
#define WM_QUIT         18u
#define PM_NOREMOVE      0u
#define PM_REMOVE        1u

typedef struct win32k_rect {
    int left, top, right, bottom;
} win32k_rect_t;

typedef struct win32k_msg {
    HWND hwnd;
    UINT message;
    unsigned long wParam;
    long lParam;
    unsigned long time;
    int pt_x, pt_y;
} win32k_msg_t;

typedef LONG (*win32k_wndproc_t)(HWND, UINT, unsigned long, long);

typedef struct win32k_wndclassex {
    UINT cbSize;
    UINT style;
    win32k_wndproc_t lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    const char *lpszMenuName;
    const char *lpszClassName;
    HICON hIconSm;
} win32k_wndclassex_t;

unsigned int NtUserRegisterClassEx(const win32k_wndclassex_t *wc);

HWND NtUserCreateWindowEx(DWORD dwExStyle, const char *lpClassName,
    const char *lpWindowName, DWORD dwStyle, int X, int Y, int nWidth,
    int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance,
    void *lpParam);

HDC  NtUserGetDC(HWND hWnd);
int  NtUserReleaseDC(HWND hWnd, HDC hDC);

COLORREF NtUserSetTextColor(HDC hDC, COLORREF crColor);
int      NtUserSetBkMode(HDC hDC, int mode);
int      NtUserRectangle(HDC hDC, int left, int top, int right, int bottom);
int      NtUserTextOut(HDC hDC, int x, int y, const char *lpString, int cch);

int NtUserShowWindow(HWND hWnd, int nCmdShow);
int NtUserUpdateWindow(HWND hWnd);

int NtUserMessageBox(HWND hWnd, const char *text, const char *caption, UINT type);

/* PostMessage - posts a message to the thread's message queue */
int NtUserPostMessage(HWND hWnd, UINT Msg, unsigned long wParam, long lParam);

int           NtUserGetMessage(win32k_msg_t *msg, HWND hWnd, UINT min, UINT max);
int           NtUserTranslateMessage(const win32k_msg_t *msg);
unsigned long NtUserDispatchMessage(const win32k_msg_t *msg);

int NtUserPeekMessage(win32k_msg_t *msg, HWND hWnd, UINT min, UINT max, UINT remove);

HWND         CreateWindowExA(DWORD, const char *, const char *, DWORD, int, int, int, int,
                             HWND, HMENU, HINSTANCE, void *);
unsigned int RegisterClassExA(const win32k_wndclassex_t *);
HDC          GetDC(HWND);
int          ReleaseDC(HWND, HDC);
COLORREF     SetTextColor(HDC, COLORREF);
int          SetBkMode(HDC, int);
int          Rectangle(HDC, int, int, int, int);
int          TextOutA(HDC, int, int, const char *, int);
int          ShowWindow(HWND, int);
int          UpdateWindow(HWND);
int          MessageBoxA(HWND, const char *, const char *, UINT);
int          GetMessageA(win32k_msg_t *, HWND, UINT, UINT);
int          PostMessageA(HWND, UINT, unsigned long, long);
int          TranslateMessage(const win32k_msg_t *);
unsigned long DispatchMessageA(const win32k_msg_t *);
int          PeekMessageA(win32k_msg_t *, HWND, UINT, UINT, UINT);
void         PostQuitMessage(int);

/* Win32k integration helpers for D3D11 bridge (WIN-021..WIN-030) */
int win32k_window_get_size(HWND hwnd, int *width, int *height);

/* ------------------------------------------------------------------ */
/* GDI-dmabuf integration (WIN-031..WIN-040)                           */
/* ------------------------------------------------------------------ */

/* Initialize GDI rendering backend with GPU context from gfx_present.
 * Call this once after gfx_present_create() and gfx_present_make_current().
 * Returns 0 on success, -1 on failure. */
int win32k_gdi_gpu_init(void *egl_display, void *egl_config, void *egl_context);

/* Clean up GDI rendering backend. */
void win32k_gdi_gpu_cleanup(void);

/* Mark a window region as needing GPU render (triggers Present).
 * Called internally by Rectangle, TextOut, FillRect, etc. */
void win32k_gdi_invalidate_gpu(HWND hwnd);

/* Present all pending GPU renders to Wayland (surface flip). 
 * Call this at frame boundaries or after batches of GDI operations. */
int win32k_gdi_gpu_present(void);

#ifdef __cplusplus
}
#endif
#endif /* NTBOX_WIN32K_H */
