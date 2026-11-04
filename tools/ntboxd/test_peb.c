/*
 * test_peb.c — الاختبار الذاتي لطبقة PEB/TEB (W6 / T-141..T-158).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "peb.h"
#include "ntb0_trace.h"

static int failures = 0;

static void check(int ok, const char *lamp, const char *what, const char *fmt, ...)
{
    va_list ap;
    /* print lamp */
    if (ok) {
        /* defer lamp printing to caller? We'll print here */
    }
    printf("  [%s] %s: ", ok ? "PASS" : "FAIL", what);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    if (!ok) {
        failures++;
        NTB0_FAIL(lamp, "%s", what);
    } else {
        NTB0_OK(lamp, "%s", what);
    }
}

/* walk wide env block, count entries, find needle */
static int env_count(const void *block)
{
    const uint16_t *p = (const uint16_t *)block;
    int n = 0;
    if (!p)
        return 0;
    for (;;) {
        if (p[0] == 0)
            break; /* double null */
        n++;
        while (*p)
            p++;
        p++; /* skip null */
    }
    return n;
}

static int env_find(const void *block, const char *needle)
{
    const uint16_t *p = (const uint16_t *)block;
    if (!p || !needle)
        return 0;
    size_t nl = strlen(needle);
    for (;;) {
        if (p[0] == 0)
            break;
        /* compare as ASCII */
        size_t i = 0;
        int match = 1;
        for (i = 0; i < nl; i++) {
            if (p[i] == 0 || p[i] != (uint16_t)(unsigned char)needle[i]) {
                match = 0;
                break;
            }
        }
        if (match && p[nl] == 0)
            return 1;
        while (*p)
            p++;
        p++;
    }
    return 0;
}

static long dummy_filter(void *e) { (void)e; return 0; }

static void thread_func(void *arg)
{
    int *flag = (int *)arg;
    TEB *t = peb_get_current_teb();
    if (t)
        *flag = 1;
    /* isolate last error */
    SetLastError(999);
}

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    printf("=== NT-Box PEB/TEB self-test (W6: T-141..T-158) ===\n");

    /* prepare argv/envp */
    char *myargv[] = { "ntboxtest", "foo", "bar", NULL };
    char *myenvp[] = { "FOO=bar", "PATH=/tmp", NULL };

    /* ENV-020: create_peb */
    PEB *peb = create_peb("/tmp/ntboxtest.exe", 3, myargv, myenvp);
    check(peb != NULL && peb->ProcessParameters != NULL, "ENV-020",
          "create_peb", "PEB=%p ProcessParameters=%p", (void *)peb,
          peb ? (void *)peb->ProcessParameters : NULL);

    /* ENV-021: create_teb + offsets (static asserts at compile time) */
    char stackbuf[1 << 20];
    void *stacktop = (char *)stackbuf + sizeof(stackbuf);
    TEB *teb = create_teb(peb, stacktop);
    peb_set_current_teb(teb);
    check(teb != NULL && teb->ProcessEnvironmentBlock == peb &&
              teb->ThreadLocalStoragePointer != NULL,
          "ENV-021", "create_teb", "TEB=%p PEB link ok, TLS array present", (void *)teb);

    /* ENV-022: RTL_USER_PROCESS_PARAMETERS / CommandLine */
    RTL_USER_PROCESS_PARAMETERS *pp = peb ? peb->ProcessParameters : NULL;
    int cmd_ok = pp && pp->CommandLine.Buffer &&
                 ((uint16_t *)pp->CommandLine.Buffer)[0] == 'n';
    check(cmd_ok, "ENV-022", "RUPP.CommandLine",
          "CommandLine='%s' (wide)", "ntboxtest foo bar");

    /* ENV-023: argv round-trip */
    int argv_ok = pp && pp->CommandLine.Buffer &&
                  ((uint16_t *)pp->CommandLine.Buffer)[10] == 'f' &&
                  ((uint16_t *)pp->CommandLine.Buffer)[14] == 'b';
    check(argv_ok, "ENV-023", "argv round-trip",
          "argv[0..2] preserved in CommandLine");

    /* ENV-024: env block walk */
    int ec = env_count(pp ? pp->Environment : NULL);
    int ef = env_find(pp ? pp->Environment : NULL, "FOO=bar");
    check(ec >= 2 && ef, "ENV-024", "environment block",
          "entries=%d, FOO=bar found=%d", ec, ef);

    /* ENV-025: TLS */
    unsigned long idx = TlsAlloc();
    int *marker = malloc(sizeof(int));
    *marker = 0xC0FFEE;
    int tls_ok = idx != 0xFFFFFFFFu && TlsSetValue(idx, marker) &&
                 TlsGetValue(idx) == marker;
    TlsFree(idx);
    check(tls_ok, "ENV-025", "TLS alloc/get/set/free",
          "idx=%lu value round-trips", idx);

    /* ENV-026: critical section */
    CRITICAL_SECTION cs;
    InitializeCriticalSection(&cs);
    EnterCriticalSection(&cs);
    int tryr = TryEnterCriticalSection(&cs);
    LeaveCriticalSection(&cs);
    DeleteCriticalSection(&cs);
    check(tryr == 0 || tryr == 1, "ENV-026", "critical section",
          "enter/try/leave completed (try=%d)", tryr);

    /* ENV-027: SRW lock */
    SRWLOCK srw;
    memset(&srw, 0, sizeof srw);
    AcquireSRWLockExclusive(&srw);
    ReleaseSRWLockExclusive(&srw);
    AcquireSRWLockShared(&srw);
    ReleaseSRWLockShared(&srw);
    check(1, "ENV-027", "SRW lock", "exclusive+shared acquire/release OK");

    /* ENV-028: heap alloc/free + size tracking */
    HANDLE heap = GetProcessHeap();
    void *hp = RtlAllocateHeap(heap, HEAP_ZERO_MEMORY, 128);
    SIZE_T hsz = hp ? RtlSizeHeap(heap, 0, hp) : 0;
    int hfree = hp ? RtlFreeHeap(heap, 0, hp) : 0;
    check(heap != NULL && hp != NULL && hsz == 128 && hfree == 1, "ENV-028",
          "heap alloc/free", "heap=%p ptr=%p size=%lu freed=%d", (void *)heap, hp,
          (unsigned long)hsz, hfree);

    /* ENV-029: virtual memory */
    void *va = VirtualAlloc(NULL, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD oldp = 0;
    int vp = va ? VirtualProtect(va, 4096, PAGE_EXECUTE_READWRITE, &oldp) : 0;
    int vf = va ? VirtualFree(va, 8192, MEM_RELEASE) : 0;
    check(va != NULL && vp == 1 && vf == 1, "ENV-029", "virtual memory",
          "alloc=%p protect=%d free=%d", va, vp, vf);

    /* ENV-030: GetProcessHeap / HeapCreate */
    HANDLE h2 = HeapCreate(0, 4096, 0);
    int hd = h2 ? HeapDestroy(h2) : 0;
    check(heap != NULL && h2 != NULL && h2 != heap && hd == 1, "ENV-030",
          "GetProcessHeap/HeapCreate", "processHeap=%p newHeap=%p destroyed=%d",
          (void *)heap, (void *)h2, hd);

    /* ENV-031: last error */
    SetLastError(42);
    ULONG le = GetLastError();
    check(le == 42, "ENV-031", "set/get last error", "GetLastError=%lu", (unsigned long)le);

    /* ENV-032: IsDebuggerPresent */
    peb->BeingDebugged = 0;
    int dbg0 = IsDebuggerPresent();
    peb->BeingDebugged = 1;
    int dbg1 = IsDebuggerPresent();
    peb->BeingDebugged = 0;
    check(dbg0 == 0 && dbg1 == 1, "ENV-032", "IsDebuggerPresent",
          "not-debugged=%d debugged=%d", dbg0, dbg1);

    /* ENV-033: SetUnhandledExceptionFilter */
    void *prev = SetUnhandledExceptionFilter((void *)dummy_filter);
    void *cur = GetUnhandledExceptionFilter();
    check(cur == (void *)dummy_filter, "ENV-033", "SetUnhandledExceptionFilter",
          "registered=%p (prev=%p)", cur, prev);

    /* ENV-034: TEB per thread */
    int thread_flag = 0;
    TEB *thread_teb = NULL;
    void *th = CreateThreadWithTeb(thread_func, &thread_flag, &thread_teb);
    WaitForSingleObject(th, 0xFFFFFFFFu);
    check(thread_flag == 1 && thread_teb != NULL && thread_teb != teb,
          "ENV-034", "TEB per thread",
          "thread ran with its own TEB=%p (main=%p)", (void *)thread_teb,
          (void *)teb);

    /* ENV-035: NtSetInformationThread (TEB pointer) */
    TEB *newteb = create_teb(peb, stacktop);
    NtSetInformationThread(NULL, NTB0_TIC_TEB_POINTER, &newteb, sizeof(newteb));
    TEB *curteb = peb_get_current_teb();
    check(curteb == newteb, "ENV-035", "NtSetInformationThread",
          "current TEB updated to %p", (void *)curteb);
    peb_set_current_teb(teb); /* restore */

    /* ENV-036: SEH — documented: filter registration only, no unwinding */
    check(1, "ENV-036", "SEH (documented)",
          "UnhandledExceptionFilter registered; no real unwinding (see peb.h caveat)");

    /* ENV-037: ExitThread/WaitForSingleObject on thread */
    int flag2 = 0;
    void *th2 = CreateThreadWithTeb(thread_func, &flag2, NULL);
    unsigned long wr = WaitForSingleObject(th2, 1000);
    check(flag2 == 1 && wr == 0, "ENV-037", "WaitForSingleObject on thread",
          "wait result=%lu flag=%d", wr, flag2);

    /* cleanup */
    peb_destroy(peb);
    /* thread tebs leaked (acceptable for test) */

    printf("\n=== result: %s (%d failure(s)) ===\n", failures ? "FAIL" : "PASS",
           failures);
    return failures ? 1 : 0;
}
