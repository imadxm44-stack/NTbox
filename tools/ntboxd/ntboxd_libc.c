/*
 * ntboxd_libc.c — طبقة C صغيرة تقابل استيرادات msvcrt.dll.
 *
 * لماذا؟
 *   تطبيقات ويندوز الحقيقية (cmd.exe، notepad.exe، وكل ما بُني بـ mingw)
 *   تستورد msvcrt.dll ⇒ وداخل الصندوق تُحلّ إلى "stub عام" ⇒
 *   memcpy/memset (مطلوبان **قبل** main) ⇒ انهيار فوري.
 *
 *   ⇒ نوفّر تنفيذاً حقيقياً لبقية CRig فوق libc المضيف (حلقة译مة:
 *   win32 → glibc). اصطلاح الاستدعاء: **windows x64** (ms_abi).
 *
 * القاعدة (NTBOX.md §9): كل دالة = مصباح (LIB-001…).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>

#include "ntboxd_libc.h"

typedef uint32_t DWORD;
typedef int BOOL;
typedef void *HWND;
typedef void *HDC;
typedef void *HGDIOBJ;
typedef void *HBRUSH;
typedef void *HPEN;
typedef void *HFONT;
typedef void *HMENU;
typedef void *HICON;
typedef void *HCURSOR;
typedef void *HINSTANCE;
typedef unsigned int UINT;
typedef long LONG;
typedef intptr_t LONG_PTR;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef const char *LPCSTR;
typedef const uint16_t *LPCWSTR;
typedef uint16_t *LPWSTR;
typedef char *LPSTR;
typedef long LRESULT;
typedef uint16_t ATOM;

/* ------------------------------------------------------------------ */
/* CRT-critical stubs (implemented in api_stubs.c, exported via
 * NTBOXD_CRT_ALIAS assembler aliases under ntboxd_<name> names).
 * المؤشرات هنا تُربط في الجدول أدناه ليحلّها lookup تلقائياً.          */
/* ------------------------------------------------------------------ */
extern void   __attribute__((ms_abi)) ntboxd__initterm(void *pfbegin, void *pfend);
extern void * __attribute__((ms_abi)) ntboxd__onexit(void *func);
extern void * __attribute__((ms_abi)) ntboxd___dllonexit(void *func, void **pdata, void **ptdata);
extern void   __attribute__((ms_abi)) ntboxd__cexit(void);
extern void   __attribute__((ms_abi)) ntboxd__amsg_exit(int r);
extern void   __attribute__((ms_abi)) ntboxd___setusermatherr(void *handler);
extern void   __attribute__((ms_abi)) ntboxd___set_app_type(int app_type);
extern void   __attribute__((ms_abi)) ntboxd__fpreset(void);
extern void   __attribute__((ms_abi)) ntboxd_abort(void);
extern void   __attribute__((ms_abi)) ntboxd_exit(int code);
extern void * __attribute__((ms_abi)) ntboxd_signal(int sig, void *handler);
extern char * __attribute__((ms_abi)) ntboxd_strerror(int errnum);
extern void * __attribute__((ms_abi)) ntboxd_localeconv(void);
extern size_t __attribute__((ms_abi)) ntboxd_fwrite(const void *buf, size_t size, size_t count, void *stream);
extern int    __attribute__((ms_abi)) ntboxd_fputc(int c, void *stream);
extern int    __attribute__((ms_abi)) ntboxd_fprintf(void *stream, const char *fmt);
extern int  * __attribute__((ms_abi)) ntboxd__errno(void);
extern int  * __attribute__((ms_abi)) ntboxd__commode(void);
extern int  * __attribute__((ms_abi)) ntboxd__fmode(void);
extern void   __attribute__((ms_abi)) ntboxd_msvcrt_except_handler4_common(void);


/* ------------------------------------------------------------------ */
/* ذاكرة                                                               */
/* ------------------------------------------------------------------ */

void *__attribute__((ms_abi)) ntboxd_memcpy(void *dst, const void *src, size_t n)
{
    if (dst && src && n)
        memcpy(dst, src, n);
    return dst;
}

void *__attribute__((ms_abi)) ntboxd_memmove(void *dst, const void *src, size_t n)
{
    if (dst && src && n)
        memmove(dst, src, n);
    return dst;
}

void *__attribute__((ms_abi)) ntboxd_memset(void *dst, int c, size_t n)
{
    if (dst && n)
        memset(dst, c, n);
    return dst;
}

int __attribute__((ms_abi)) ntboxd_memcmp(const void *a, const void *b, size_t n)
{
    if (!a || !b)
        return 0;
    return memcmp(a, b, n);
}

void *__attribute__((ms_abi)) ntboxd_malloc(size_t n)
{
    return malloc(n ? n : 1);
}

void *__attribute__((ms_abi)) ntboxd_calloc(size_t n, size_t m)
{
    return calloc(n ? n : 1, m ? m : 1);
}

void *__attribute__((ms_abi)) ntboxd_realloc(void *p, size_t n)
{
    return realloc(p, n ? n : 1);
}

void __attribute__((ms_abi)) ntboxd_free(void *p)
{
    free(p);
}

/* ------------------------------------------------------------------ */
/* نصوص (ASCII + UTF-16)                                               */
/* ------------------------------------------------------------------ */

size_t __attribute__((ms_abi)) ntboxd_strlen(const char *s)
{
    return s ? strlen(s) : 0;
}

size_t __attribute__((ms_abi)) ntboxd_wcslen(const uint16_t *s)
{
    size_t n = 0;

    if (!s)
        return 0;
    while (s[n])
        n++;
    return n;
}

int __attribute__((ms_abi)) ntboxd_strcmp(const char *a, const char *b)
{
    if (!a || !b)
        return a == b ? 0 : 1;
    return strcmp(a, b);
}

int __attribute__((ms_abi)) ntboxd_strncmp(const char *a, const char *b, size_t n)
{
    if (!a || !b)
        return a == b ? 0 : 1;
    return strncmp(a, b, n);
}

char *__attribute__((ms_abi)) ntboxd_strcpy(char *d, const char *s)
{
    if (d && s)
        strcpy(d, s);
    return d;
}

char *__attribute__((ms_abi)) ntboxd_strcat(char *d, const char *s)
{
    if (d && s)
        strcat(d, s);
    return d;
}

char *__attribute__((ms_abi)) ntboxd_strchr(const char *s, int c)
{
    return s ? strchr(s, c) : NULL;
}

char *__attribute__((ms_abi)) ntboxd_strrchr(const char *s, int c)
{
    return s ? strrchr(s, c) : NULL;
}

/* ------------------------------------------------------------------ */
/* رقم/نص                                                             */
/* ------------------------------------------------------------------ */

int __attribute__((ms_abi)) ntboxd_atoi(const char *s)
{
    return s ? atoi(s) : 0;
}

long __attribute__((ms_abi)) ntboxd_atol(const char *s)
{
    return s ? atol(s) : 0;
}

double __attribute__((ms_abi)) ntboxd_atof(const char *s)
{
    return s ? atof(s) : 0.0;
}

/* ------------------------------------------------------------------ */
/* I/O                                                                 */
/* ------------------------------------------------------------------ */

/*
 * fwrite/fputs على stdout ⇒ المضيف (مفيد للطباعة من داخل الصندوق).
 * ملاحظة: msvcrt يستخدم FILE* داخلياً خاص به ⇒ نستعمل stdout مباشرة.
 */
int __attribute__((ms_abi)) ntboxd_fputs(const char *s, void *stream)
{
    (void)stream;
    if (!s)
        return EOF;
    fputs(s, stdout);
    fflush(stdout);
    return 0;
}

int __attribute__((ms_abi)) ntboxd_putchar(int c)
{
    int r = putchar(c);

    fflush(stdout);
    return r;
}

int __attribute__((ms_abi)) ntboxd_printf(const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    return r;
}

int __attribute__((ms_abi)) ntboxd_vfprintf(void *stream, const char *fmt,
                                           void *ap)
{
    va_list vl;
    int r;

    (void)stream;
    /* va_list على x86-64 مصفوفة من عنصر واحد ⇒ ننسخ المؤشّر كما هو */
    memcpy(&vl, ap, sizeof(vl));
    r = vfprintf(stdout, fmt, vl);
    fflush(stdout);
    return r;
}
/* ------------------------------------------------------------------ */
/* الجدول                                                               */
/* ------------------------------------------------------------------ */

struct ntboxd_libc_entry {
    const char *name;
    void       *fn;
};

static const struct ntboxd_libc_entry ntboxd_libc_table[] = {
    { "memcpy", (void *)ntboxd_memcpy },
    { "memmove", (void *)ntboxd_memmove },
    { "memset", (void *)ntboxd_memset },
    { "memcmp", (void *)ntboxd_memcmp },
    { "malloc", (void *)ntboxd_malloc },
    { "calloc", (void *)ntboxd_calloc },
    { "realloc", (void *)ntboxd_realloc },
    { "free", (void *)ntboxd_free },
    { "strlen", (void *)ntboxd_strlen },
    { "wcslen", (void *)ntboxd_wcslen },
    { "strcmp", (void *)ntboxd_strcmp },
    { "strncmp", (void *)ntboxd_strncmp },
    { "strcpy", (void *)ntboxd_strcpy },
    { "strcat", (void *)ntboxd_strcat },
    { "strchr", (void *)ntboxd_strchr },
    { "strrchr", (void *)ntboxd_strrchr },
    { "atoi", (void *)ntboxd_atoi },
    { "atol", (void *)ntboxd_atol },
    { "atof", (void *)ntboxd_atof },
    { "fputs", (void *)ntboxd_fputs },
    { "putchar", (void *)ntboxd_putchar },
    { "printf", (void *)ntboxd_printf },
    { "vfprintf", (void *)ntboxd_vfprintf },
    { "_msize", (void *)ntboxd_malloc },
    /*
     * CRT-critical msvcrt entries (MinGW startup actually CALLS these).
     * Implementations live in api_stubs.c and are exported as
     * ntboxd_<name> assembler aliases (NTBOXD_CRT_ALIAS / .globl .set).
     * الربط هنا يجعل جدول libc يحلّل الأسماء تلقائياً دون تعديل resolve().
     */
    { "_initterm", (void *)ntboxd__initterm },
    { "_onexit", (void *)ntboxd__onexit },
    { "__dllonexit", (void *)ntboxd___dllonexit },
    { "_cexit", (void *)ntboxd__cexit },
    { "_amsg_exit", (void *)ntboxd__amsg_exit },
    { "__setusermatherr", (void *)ntboxd___setusermatherr },
    { "__set_app_type", (void *)ntboxd___set_app_type },
    { "_fpreset", (void *)ntboxd__fpreset },
    { "abort", (void *)ntboxd_abort },
    { "exit", (void *)ntboxd_exit },
    { "signal", (void *)ntboxd_signal },
    { "strerror", (void *)ntboxd_strerror },
    { "localeconv", (void *)ntboxd_localeconv },
    { "fwrite", (void *)ntboxd_fwrite },
    { "fputc", (void *)ntboxd_fputc },
    { "fprintf", (void *)ntboxd_fprintf },
    { "_errno", (void *)ntboxd__errno },
    { "_commode", (void *)ntboxd__commode },
    { "_fmode", (void *)ntboxd__fmode },
    { "_except_handler4_common", (void *)ntboxd_msvcrt_except_handler4_common },
};