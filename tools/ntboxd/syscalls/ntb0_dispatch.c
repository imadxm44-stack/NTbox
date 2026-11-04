/*
 * ntb0_dispatch — المترجم (dispatcher) لأوامر الموارد النهائية.
 *
 * هذا هو "المترجم" بمعنى المستخدم: **جسر** بين كود ويندوز (مستخدم) ونواة لينكس.
 * لا يترجم إلا الطلبات الطرفية (leaf): مورد حقيقي (ملف، ذاكرة، مقبض).
 * استدعاء DLL داخل DLL يبقى مباشراً ولا يصل هنا.
 *
 * نقطة الدخول من ntb0_trampoline.S:
 *     long ntb0_dispatch(long num, long a1..a11)
 *
 * اصطلاح windows x64 المستخدم:
 *     num = رقم syscall (rax عند الدخول)
 *     a1  = r10   a2 = rdx   a3 = r8   a4 = r9   a5 = [rsp+0x28]
 *     a6  = [rsp+0x30]  a7 = [rsp+0x38]  a8 = [rsp+0x40]
 *     a9  = [rsp+0x48]  a10 = [rsp+0x50] a11 = [rsp+0x58]
 *
 * (المعاملات 6..11 تُمرَّر عبر المكدس — قِسناها ونوثّقناها في
 *  NTBOX.md §9.10)
 *
 * القاعدة (NTBOX.md §9): كل syscall = ID + مصباح + اختبار. لا Panic.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include "ntb0_trace.h"
#include "ntb0_dispatch.h"
#include "ntb0_nt.h"
#include "ntb0_path.h"
#include "ntb0_handle.h"
#include "ntb0_thread.h"
#include "ntb0_reg.h"
#include "ntb0_async_io.h" /* I/O غير متزامن (T-071..T-073) */
#include "ntdll_syscalls.h" /* الجدول المستخرج من ntdll الحقيقي (400 مدخل) */

/* ------------------------------------------------------------------ */
/* أدوات مساعدة                                                        */
/* ------------------------------------------------------------------ */

/* وقت الصندوق (100-نانية) مع إزاحة NtSetSystemInformation — انظر SYS-048 */
static int64_t ntb0_time_now_100ns(void);

/* وقت إقلاع المضيف (100-نانية) من /proc/stat */
static int64_t ntb0_boot_time_100ns(void);

static int ntb0_vm_region(uintptr_t addr, uintptr_t *start, uintptr_t *end, int *prot);

/*
 * UTF-16LE ⇒ UTF-8 (نسخة مبسّطة: BMP + بديل codepoints).
 * تُعيد عدد المحارف المكتوبة، أو -1 عند الخطأ.
 */
static int ntb0_utf16le_to_utf8(const void *src, size_t nchars, char *out,
                                size_t out_len)
{
    const uint16_t *u = (const uint16_t *)src;
    size_t i, o = 0;

    if (!src || !out || out_len == 0)
        return -1;

    for (i = 0; i < nchars; i++) {
        uint16_t c = u[i];

        if (c < 0x80) {
            if (o + 1 >= out_len)
                return -1;
            out[o++] = (char)c;
        } else if (c < 0x800) {
            if (o + 2 >= out_len)
                return -1;
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (o + 3 >= out_len)
                return -1;
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }

    out[o] = '\0';
    return (int)o;
}

/* ------------------------------------------------------------------ */
/* errno -> NTSTATUS                                                   */
/* ------------------------------------------------------------------ */

static long ntb0_errno_to_ntstatus(int e)
{
    switch (e) {
    case 0:            return NTB0_ST_SUCCESS;
    case EBADF:        return NTB0_ST_INVALID_HANDLE;
    case EACCES:
    case EPERM:        return NTB0_ST_ACCESS_DENIED;
    case ENOENT:       return NTB0_ST_OBJECT_NAME_NOT_FOUND;
    case EEXIST:       return NTB0_ST_OBJECT_NAME_COLLISION;
    case EISDIR:       return NTB0_ST_FILE_IS_A_DIRECTORY;
    case ENOTDIR:      return NTB0_ST_NOT_A_DIRECTORY;
    case ENAMETOOLONG:
    case ELOOP:        return NTB0_ST_OBJECT_PATH_SYNTAX_BAD;
    case ENOTEMPTY:    return NTB0_ST_STATUS_LOGICAL_ERROR;
    case ENOMEM:       return NTB0_ST_NO_MEMORY;
    case EINVAL:       return NTB0_ST_INVALID_PARAMETER;
    default:           return NTB0_ST_UNSUCCESSFUL;
    }
}

/* ------------------------------------------------------------------ */
/* جدول الـ syscalls المنفَّذة                                         */
/* ------------------------------------------------------------------ */

typedef long (*ntb0_handler_fn)(long a1, long a2, long a3, long a4, long a5,
                                 long a6, long a7, long a8, long a9, long a10,
                                 long a11);

static long ntb0_NtClose(long a1, long a2, long a3, long a4, long a5,
                          long a6, long a7, long a8, long a9, long a10,
                          long a11);
static long ntb0_NtAllocateVirtualMemory(long a1, long a2, long a3, long a4,
                                          long a5, long a6, long a7, long a8,
                                          long a9, long a10, long a11);
static long ntb0_NtCreateFile(long a1, long a2, long a3, long a4, long a5,
                              long a6, long a7, long a8, long a9, long a10,
                              long a11);
static long ntb0_NtOpenFile(long a1, long a2, long a3, long a4, long a5,
                            long a6, long a7, long a8, long a9, long a10,
                            long a11);
static long ntb0_NtReadFile(long a1, long a2, long a3, long a4, long a5,
                            long a6, long a7, long a8, long a9, long a10,
                            long a11);
static long ntb0_NtWriteFile(long a1, long a2, long a3, long a4, long a5,
                             long a6, long a7, long a8, long a9, long a10,
                             long a11);
static long ntb0_NtCancelIoFile(long a1, long a2, long a3, long a4, long a5,
                                long a6, long a7, long a8, long a9, long a10,
                                long a11);
static long ntb0_NtSetValueKey(long a1, long a2, long a3, long a4, long a5,
                               long a6, long a7, long a8, long a9, long a10,
                               long a11);
static long ntb0_NtLoadKey(long a1, long a2, long a3, long a4, long a5,
                           long a6, long a7, long a8, long a9, long a10,
                           long a11);
static long ntb0_NtDuplicateObject(long a1, long a2, long a3, long a4,
                                    long a5, long a6, long a7, long a8,
                                    long a9, long a10, long a11);
static long ntb0_NtFreeVirtualMemory(long a1, long a2, long a3, long a4,
                                      long a5, long a6, long a7, long a8,
                                      long a9, long a10, long a11);
static long ntb0_NtProtectVirtualMemory(long a1, long a2, long a3, long a4,
                                         long a5, long a6, long a7, long a8,
                                         long a9, long a10, long a11);
static long ntb0_NtQueryVirtualMemory(long a1, long a2, long a3, long a4,
                                       long a5, long a6, long a7, long a8,
                                       long a9, long a10, long a11);
static long ntb0_NtQueryInformationFile(long a1, long a2, long a3, long a4,
                                         long a5, long a6, long a7, long a8,
                                         long a9, long a10, long a11);
static long ntb0_NtSetInformationFile(long a1, long a2, long a3, long a4,
                                      long a5, long a6, long a7, long a8,
                                      long a9, long a10, long a11);
static long ntb0_NtFlushBuffersFile(long a1, long a2, long a3, long a4, long a5,
                                    long a6, long a7, long a8, long a9, long a10,
                                    long a11);
static long ntb0_NtDeviceIoControlFile(long a1, long a2, long a3, long a4,
                                       long a5, long a6, long a7, long a8,
                                       long a9, long a10, long a11);
static long ntb0_NtCreateEvent(long a1, long a2, long a3, long a4, long a5,
                                long a6, long a7, long a8, long a9, long a10,
                                long a11);
static long ntb0_NtSetEvent(long a1, long a2, long a3, long a4, long a5,
                             long a6, long a7, long a8, long a9, long a10,
                             long a11);
static long ntb0_NtClearEvent(long a1, long a2, long a3, long a4, long a5,
                               long a6, long a7, long a8, long a9, long a10,
                               long a11);
static long ntb0_NtWaitForSingleObject(long a1, long a2, long a3, long a4,
                                        long a5, long a6, long a7, long a8,
                                        long a9, long a10, long a11);
static long ntb0_NtDelayExecution(long a1, long a2, long a3, long a4, long a5,
                                   long a6, long a7, long a8, long a9, long a10,
                                   long a11);
static long ntb0_NtYieldExecution(long a1, long a2, long a3, long a4, long a5,
                                  long a6, long a7, long a8, long a9, long a10,
                                  long a11);
static long ntb0_NtQuerySystemInformation(long a1, long a2, long a3, long a4,
                                          long a5, long a6, long a7, long a8,
                                          long a9, long a10, long a11);
static long ntb0_NtQueryDirectoryFile(long a1, long a2, long a3, long a4,
                                      long a5, long a6, long a7, long a8,
                                      long a9, long a10, long a11);
static long ntb0_NtCreateSection(long a1, long a2, long a3, long a4, long a5,
                                 long a6, long a7, long a8, long a9, long a10,
                                 long a11);
static long ntb0_NtMapViewOfSection(long a1, long a2, long a3, long a4, long a5,
                                    long a6, long a7, long a8, long a9, long a10,
                                    long a11);
static long ntb0_NtUnmapViewOfSection(long a1, long a2, long a3, long a4, long a5,
                                      long a6, long a7, long a8, long a9, long a10,
                                      long a11);
static long ntb0_NtFsControlFile(long a1, long a2, long a3, long a4, long a5,
                                 long a6, long a7, long a8, long a9, long a10,
                                 long a11);
static long ntb0_NtLockFile(long a1, long a2, long a3, long a4, long a5, long a6,
                            long a7, long a8, long a9, long a10, long a11);
static long ntb0_NtUnlockFile(long a1, long a2, long a3, long a4, long a5,
                              long a6, long a7, long a8, long a9, long a10,
                              long a11);
static long ntb0_NtQueryAttributesFile(long a1, long a2, long a3, long a4,
                                       long a5, long a6, long a7, long a8, long a9,
                                       long a10, long a11);
static long ntb0_NtQueryObject(long a1, long a2, long a3, long a4, long a5,
                               long a6, long a7, long a8, long a9, long a10,
                               long a11);
static long ntb0_NtCreateThread(long a1, long a2, long a3, long a4, long a5,
                                long a6, long a7, long a8, long a9, long a10,
                                long a11);
static long ntb0_NtResumeThread(long a1, long a2, long a3, long a4, long a5,
                                long a6, long a7, long a8, long a9, long a10,
                                long a11);
static long ntb0_NtTerminateThread(long a1, long a2, long a3, long a4, long a5,
                                   long a6, long a7, long a8, long a9, long a10,
                                   long a11);
static long ntb0_NtOpenProcess(long a1, long a2, long a3, long a4, long a5,
                               long a6, long a7, long a8, long a9, long a10,
                               long a11);
static long ntb0_NtTerminateProcess(long a1, long a2, long a3, long a4, long a5,
                                    long a6, long a7, long a8, long a9, long a10,
                                    long a11);
static long ntb0_NtQueryInformationProcess(long a1, long a2, long a3, long a4,
                                            long a5, long a6, long a7, long a8,
                                            long a9, long a10, long a11);
static long ntb0_NtOpenThread(long a1, long a2, long a3, long a4, long a5,
                              long a6, long a7, long a8, long a9, long a10,
                              long a11);
static long ntb0_NtSuspendThread(long a1, long a2, long a3, long a4, long a5,
                                 long a6, long a7, long a8, long a9, long a10,
                                 long a11);
static long ntb0_NtQueryInformationThread(long a1, long a2, long a3, long a4,
                                           long a5, long a6, long a7, long a8,
                                           long a9, long a10, long a11);
static long ntb0_NtSetInformationThread(long a1, long a2, long a3, long a4,
                                        long a5, long a6, long a7, long a8,
                                        long a9, long a10, long a11);
static long ntb0_NtCreateNamedPipeFile(long a1, long a2, long a3, long a4,
                                       long a5, long a6, long a7, long a8,
                                       long a9, long a10, long a11);
static long ntb0_NtQueryFullAttributesFile(long a1, long a2, long a3, long a4,
                                           long a5, long a6, long a7, long a8,
                                           long a9, long a10, long a11);
static long ntb0_NtSetSystemInformation(long a1, long a2, long a3, long a4,
                                        long a5, long a6, long a7, long a8,
                                        long a9, long a10, long a11);
static long ntb0_NtOpenKey(long a1, long a2, long a3, long a4, long a5, long a6,
                           long a7, long a8, long a9, long a10, long a11);
static long ntb0_NtQueryValueKey(long a1, long a2, long a3, long a4, long a5,
                                 long a6, long a7, long a8, long a9, long a10,
                                 long a11);
static long ntb0_NtEnumerateKey(long a1, long a2, long a3, long a4, long a5,
                                long a6, long a7, long a8, long a9, long a10,
                                long a11);
static long ntb0_NtEnumerateValueKey(long a1, long a2, long a3, long a4, long a5,
                                     long a6, long a7, long a8, long a9, long a10,
                                     long a11);
static long ntb0_NtFlushKey(long a1, long a2, long a3, long a4, long a5,
                            long a6, long a7, long a8, long a9, long a10,
                            long a11);
static long ntb0_NtReleaseSemaphore(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9, long a10, long a11);
static long ntb0_NtReleaseMutant(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9, long a10, long a11);
static long ntb0_NtQueryInformationToken(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9, long a10, long a11);
static long ntb0_NtQueryTimer(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9, long a10, long a11);
static long ntb0_NtContinue(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9, long a10, long a11);
static long ntb0_NtQuerySection(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9, long a10, long a11);
static long ntb0_NtAreMappedFilesTheSame(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9, long a10, long a11);
static long ntb0_NtCreateMutant(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9, long a10, long a11);
static long ntb0_NtCreateSemaphore(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9, long a10, long a11);
static long ntb0_NtCreateTimer(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9, long a10, long a11);
static long ntb0_NtExtendSection(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9, long a10, long a11);
static long ntb0_NtFlushInstructionCache(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9, long a10, long a11);
static long ntb0_NtGetContextThread(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9, long a10, long a11);
static long ntb0_NtRaiseException(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9, long a10, long a11);
static long ntb0_NtSetContextThread(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9, long a10, long a11);
static long ntb0_NtLockVirtualMemory(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9, long a10, long a11);
static long ntb0_NtUnlockVirtualMemory(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9, long a10, long a11);
static long ntb0_NtSetTimer(long a1, long a2, long a3, long a4, long a5, long a6, long a7, long a8, long a9, long a10, long a11);

/*
 * ملاحظة: NULL = syscall معروف لكنه غير منفَّذ بعد → مصباح STUB.
 * لا نضع syscall في القائمة إلا بعد:id + مصباح + اختبار (MASTER §11).
 *
 * SYS-007 ليس syscall بل **جدول المقابض** (T-023) — مصابيحه في
 * ntb0_handle.c، وهو أساس كل ما بعده (ShareAccess، RootDirectory،
 * DuplicateObject).
 */
#define NTB0_SC_LIST(X)                                                  \
    X(SYS_001, "SYS-001", 0x0c, "NtClose",                ntb0_NtClose)  \
    X(SYS_002, "SYS-002", 0x15, "NtAllocateVirtualMemory",               \
      ntb0_NtAllocateVirtualMemory)                                       \
    X(SYS_003, "SYS-003", 0x03, "NtReadFile",              ntb0_NtReadFile)\
    X(SYS_004, "SYS-004", 0x05, "NtWriteFile",             ntb0_NtWriteFile)\
    X(SYS_005, "SYS-005", 0x52, "NtCreateFile",           ntb0_NtCreateFile)\
    X(SYS_006, "SYS-006", 0x30, "NtOpenFile",             ntb0_NtOpenFile)\
    X(SYS_008, "SYS-008", 0x39, "NtDuplicateObject",                     \
      ntb0_NtDuplicateObject)                                            \
    X(SYS_009, "SYS-009", 0x1b, "NtFreeVirtualMemory",                   \
      ntb0_NtFreeVirtualMemory)                                          \
    X(SYS_010, "SYS-010", 0x4d, "NtProtectVirtualMemory",                \
      ntb0_NtProtectVirtualMemory)                                       \
    X(SYS_011, "SYS-011", 0x20, "NtQueryVirtualMemory",                   \
      ntb0_NtQueryVirtualMemory)                                         \
    X(SYS_012, "SYS-012", 0x0e, "NtQueryInformationFile",                \
      ntb0_NtQueryInformationFile)                                       \
    X(SYS_013, "SYS-013", 0x24, "NtSetInformationFile",                 \
      ntb0_NtSetInformationFile)                                         \
    X(SYS_014, "SYS-014", 0x48, "NtFlushBuffersFile",                   \
      ntb0_NtFlushBuffersFile)                                           \
    X(SYS_015, "SYS-015", 0x04, "NtDeviceIoControlFile",                \
      ntb0_NtDeviceIoControlFile)                                       \
    X(SYS_016, "SYS-016", 0x45, "NtCreateEvent",                        \
      ntb0_NtCreateEvent)                                                \
    X(SYS_017, "SYS-017", 0x0b, "NtSetEvent",                           \
      ntb0_NtSetEvent)                                                   \
    X(SYS_018, "SYS-018", 0x3b, "NtClearEvent",                         \
      ntb0_NtClearEvent)                                                 \
    X(SYS_019, "SYS-019", 0x01, "NtWaitForSingleObject",                \
      ntb0_NtWaitForSingleObject)                                       \
    X(SYS_020, "SYS-020", 0x31, "NtDelayExecution",                     \
      ntb0_NtDelayExecution)                                             \
    X(SYS_021, "SYS-021", 0x43, "NtYieldExecution",                     \
      ntb0_NtYieldExecution)                                             \
    X(SYS_022, "SYS-022", 0x33, "NtQuerySystemInformation",             \
      ntb0_NtQuerySystemInformation)                                     \
    X(SYS_023, "SYS-023", 0x32, "NtQueryDirectoryFile",                 \
      ntb0_NtQueryDirectoryFile)                                       \
    X(SYS_024, "SYS-024", 0x47, "NtCreateSection",                      \
      ntb0_NtCreateSection)                                            \
    X(SYS_025, "SYS-025", 0x25, "NtMapViewOfSection",                   \
      ntb0_NtMapViewOfSection)                                         \
    X(SYS_026, "SYS-026", 0x27, "NtUnmapViewOfSection",                 \
      ntb0_NtUnmapViewOfSection)                                       \
    X(SYS_027, "SYS-027", 0x36, "NtFsControlFile",                     \
      ntb0_NtFsControlFile)                                            \
    X(SYS_028, "SYS-028", 0xe0, "NtLockFile",                          \
      ntb0_NtLockFile)                                                 \
    X(SYS_029, "SYS-029", 0x188, "NtUnlockFile",                        \
      ntb0_NtUnlockFile)                                               \
    X(SYS_030, "SYS-030", 0x3a, "NtQueryAttributesFile",                \
      ntb0_NtQueryAttributesFile)                                      \
    X(SYS_031, "SYS-031", 0x0d, "NtQueryObject",                       \
      ntb0_NtQueryObject)                                        \
    X(SYS_032, "SYS-032", 0x4b, "NtCreateThread",                      \
      ntb0_NtCreateThread)                                            \
    X(SYS_033, "SYS-033", 0x4f, "NtResumeThread",                      \
      ntb0_NtResumeThread)                                            \
    X(SYS_034, "SYS-034", 0x50, "NtTerminateThread",                   \
      ntb0_NtTerminateThread)                                         \
    X(SYS_035, "SYS-035", 0x23, "NtOpenProcess",                       \
      ntb0_NtOpenProcess)                                             \
    X(SYS_036, "SYS-036", 0x29, "NtTerminateProcess",                  \
      ntb0_NtTerminateProcess)                                        \
    X(SYS_037, "SYS-037", 0x16, "NtQueryInformationProcess",           \
      ntb0_NtQueryInformationProcess)                                 \
    X(SYS_038, "SYS-038", 0xfe, "NtOpenThread",                        \
      ntb0_NtOpenThread)                                              \
    X(SYS_039, "SYS-039", 0x17b, "NtSuspendThread",                   \
      ntb0_NtSuspendThread)                                        \
    X(SYS_040, "SYS-040", 0x22, "NtQueryInformationThread",           \
      ntb0_NtQueryInformationThread)                                   \
    X(SYS_041, "SYS-041", 0x0a, "NtSetInformationThread",             \
      ntb0_NtSetInformationThread)                                     \
    X(SYS_045, "SYS-045", 0x9b, "NtCreateNamedPipeFile",              \
      ntb0_NtCreateNamedPipeFile)                                      \
    X(SYS_046, "SYS-046", 0x113, "NtQueryFullAttributesFile",         \
      ntb0_NtQueryFullAttributesFile)                                  \
    X(SYS_048, "SYS-048", 0x16c, "NtSetSystemInformation",            \
      ntb0_NtSetSystemInformation)                                     \
    X(SYS_049, "SYS-049", 0x0f, "NtOpenKey",                          \
      ntb0_NtOpenKey)                                                  \
    X(SYS_050, "SYS-050", 0x14, "NtQueryValueKey",                    \
      ntb0_NtQueryValueKey)                                            \
    X(SYS_051, "SYS-051", 0x2f, "NtEnumerateKey",                     \
      ntb0_NtEnumerateKey)                                             \
    X(SYS_052, "SYS-052", 0x10, "NtEnumerateValueKey",                \
      ntb0_NtEnumerateValueKey)                                        \
    X(SYS_053, "SYS-053", 0xc3, "NtFlushKey",                         \
      ntb0_NtFlushKey)                                                  \
    X(SYS_056, "SYS-056", 0x5a, "NtCancelIoFile",                     \
      ntb0_NtCancelIoFile)                                              \
    X(SYS_057, "SYS-057", 0x86, "NtCancelIoFileEx",                    \
      ntb0_NtCancelIoFile)                                              \
    X(SYS_058, "SYS-058", 0x5d, "NtSetValueKey",                       \
      ntb0_NtSetValueKey)                                               \
    X(SYS_059, "SYS-059", 0xdd, "NtLoadKey",                           \
      ntb0_NtLoadKey)                                                   \
    X(SYS_060, "SYS-060", 0x007, "NtReleaseSemaphore",       ntb0_NtReleaseSemaphore) \
    X(SYS_061, "SYS-061", 0x01d, "NtReleaseMutant",         ntb0_NtReleaseMutant)    \
    X(SYS_062, "SYS-062", 0x01e, "NtQueryInformationToken",   ntb0_NtQueryInformationToken) \
    X(SYS_063, "SYS-063", 0x035, "NtQueryTimer",            ntb0_NtQueryTimer)       \
    X(SYS_064, "SYS-064", 0x040, "NtContinue",              ntb0_NtContinue)         \
    X(SYS_065, "SYS-065", 0x04e, "NtQuerySection",          ntb0_NtQuerySection)     \
    X(SYS_066, "SYS-066", 0x084, "NtAreMappedFilesTheSame", ntb0_NtAreMappedFilesTheSame) \
    X(SYS_067, "SYS-067", 0x09a, "NtCreateMutant",          ntb0_NtCreateMutant)     \
    X(SYS_068, "SYS-068", 0x0a3, "NtCreateSemaphore",       ntb0_NtCreateSemaphore)  \
    X(SYS_069, "SYS-069", 0x0a6, "NtCreateTimer",           ntb0_NtCreateTimer)      \
    X(SYS_070, "SYS-070", 0x0bf, "NtExtendSection",         ntb0_NtExtendSection)    \
    X(SYS_071, "SYS-071", 0x0c2, "NtFlushInstructionCache", ntb0_NtFlushInstructionCache) \
    X(SYS_072, "SYS-072", 0x0ca, "NtGetContextThread",      ntb0_NtGetContextThread) \
    X(SYS_073, "SYS-073", 0x12f, "NtRaiseException",        ntb0_NtRaiseException)   \
    X(SYS_074, "SYS-074", 0x150, "NtSetContextThread",      ntb0_NtSetContextThread) \
    X(SYS_075, "SYS-075", 0x0e3, "NtLockVirtualMemory",     ntb0_NtLockVirtualMemory)\
    X(SYS_076, "SYS-076", 0x189, "NtUnlockVirtualMemory",   ntb0_NtUnlockVirtualMemory)\
    X(SYS_077, "SYS-077", 0x05f, "NtSetTimer",              ntb0_NtSetTimer)

enum ntb0_sc_id {
#define X(e, s, n, name, fn) NTB0_SC_##e,
    NTB0_SC_LIST(X)
#undef X
    NTB0_SC_COUNT
};

struct ntb0_sc {
    uint32_t        number;
    const char     *id;
    const char     *name;
    ntb0_handler_fn fn;
    unsigned int    idx; /* فهرس في ntb0_stats */
};

static const struct ntb0_sc ntb0_sc_table[] = {
#define X(e, s, n, name, fn) { n, s, name, fn, NTB0_SC_##e },
    NTB0_SC_LIST(X)
#undef X
};

struct ntb0_stat ntb0_stats[NTB0_SC_COUNT];

/* يبحث عن الـ syscall في جدولنا المنفَّذ */
static const struct ntb0_sc *ntb0_sc_find(uint32_t number)
{
    unsigned int i;

    for (i = 0; i < NTB0_SC_COUNT; i++) {
        if (ntb0_sc_table[i].number == number)
            return &ntb0_sc_table[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* المنفِّذات                                                          */
/* ------------------------------------------------------------------ */

/*
 * SYS-001 — NtClose(HANDLE FileHandle)
 *   a1 = المقبض (شفاف من جدول المقابض — T-023)
 *
 *   بعد T-023: المقبض ليس fd؛ يُبحث عنه في جدول المقابض ثم يُغلق
 *   الـ fd وتُحرَّر الخانة. إغلاق مقبض غير صالح → STATUS_INVALID_HANDLE.
 */
static long ntb0_NtClose(long a1, long a2, long a3, long a4, long a5,
                          long a6, long a7, long a8, long a9, long a10,
                          long a11)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    return ntb0_handle_close((uint64_t)a1);
}

/*
 * SYS-002 — NtAllocateVirtualMemory(
 *              HANDLE ProcessHandle, PVOID *BaseAddress, ULONG ZeroBits,
 *              PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect)
 *
 *   a1 = ProcessHandle   a2 = BaseAddress*   a3 = ZeroBits
 *   a4 = RegionSize*     a5 = AllocationType  a6 = Protect
 *
 *   المعامل السادس (Protect) كان خارج المترجم في المرحلة 2a — الآن يبلغنا
 *   (أُضيفت المعاملات 6..11 إلى القفزة — انظر §9.10 من سجل التحقق).
 */
static long ntb0_NtAllocateVirtualMemory(long a1, long a2, long a3, long a4,
                                          long a5, long a6, long a7, long a8,
                                          long a9, long a10, long a11)
{
    unsigned long *base_ptr = (unsigned long *)(uintptr_t)a2;
    unsigned long *size_ptr = (unsigned long *)(uintptr_t)a4;
    size_t size;
    void *p;
    int prot = PROT_READ | PROT_WRITE;

    (void)a1;
    (void)a3;
    (void)a5; /* AllocationType — كل الأنواع مدعومة mmap حالياً */
    (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    /* Protect: PAGE_* bits → PROT_* */
    if (a6 != 0) {
        switch ((uint32_t)a6 & 0xFF) {
        case 0x01: prot = PROT_NONE; break;                /* PAGE_NOACCESS */
        case 0x02: prot = PROT_READ; break;                /* PAGE_READONLY */
        case 0x04: prot = PROT_READ | PROT_WRITE; break;   /* PAGE_READWRITE */
        case 0x10: prot = PROT_EXEC; break;                /* PAGE_EXECUTE */
        case 0x20: prot = PROT_EXEC | PROT_READ; break;    /* PAGE_EXECUTE_READ */
        case 0x40: prot = PROT_EXEC | PROT_READ | PROT_WRITE; break;
        default:   prot = PROT_READ | PROT_WRITE; break;
        }
    }

    if (!base_ptr || !size_ptr)
        return NTB0_ST_INVALID_PARAMETER;

    size = (size_t)*size_ptr;
    if (size == 0)
        return NTB0_ST_INVALID_PARAMETER;

    p = mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return ntb0_errno_to_ntstatus(errno);

    *base_ptr = (unsigned long)(uintptr_t)p;
    *size_ptr = (unsigned long)size;
    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SYS-005 / SYS-006 — NtCreateFile / NtOpenFile                        */
/* ------------------------------------------------------------------ */

/*
 * ترجمة ACCESS_MASK → وضع الوصول في POSIX.
 * ملاحظة: ShareAccess لا يُترجَم (لينكس بلا وضع مشاركة) — موثّق في §9.10.
 */
static int ntb0_access_to_posix(uint32_t acc)
{
    uint32_t wr = acc & (NTB0_FILE_WRITE_DATA | NTB0_FILE_APPEND_DATA |
                          NTB0_FILE_WRITE_ATTRIBUTES | NTB0_FILE_WRITE_EA |
                          NTB0_GENERIC_WRITE);
    uint32_t rd = acc & (NTB0_FILE_READ_DATA | NTB0_FILE_READ_ATTRIBUTES |
                          NTB0_FILE_READ_EA | NTB0_GENERIC_READ);
    int mode;

    if (wr && rd)
        mode = O_RDWR;
    else if (wr)
        mode = O_WRONLY;
    else
        mode = O_RDONLY;

    if (acc & NTB0_FILE_APPEND_DATA)
        mode |= O_APPEND;

    return mode;
}

/* ترجمة CreateDisposition → أعلام الإنشاء/الفتح في POSIX */
static int ntb0_disposition_to_posix(uint32_t disp)
{
    switch (disp) {
    case NTB0_FILE_SUPERSEDE:    return O_CREAT | O_TRUNC;
    case NTB0_FILE_OPEN:         return 0;
    case NTB0_FILE_CREATE:       return O_CREAT | O_EXCL;
    case NTB0_FILE_OPEN_IF:      return O_CREAT;
    case NTB0_FILE_OVERWRITE:    return O_TRUNC;
    case NTB0_FILE_OVERWRITE_IF: return O_CREAT | O_TRUNC;
    default:                     return -1;
    }
}

/*
 * مسار الفتح المشترك بين NtCreateFile و NtOpenFile.
 *
 * الفروق بين الاثنين:
 *   - NtCreateFile: CreateDisposition = arg8 ، CreateOptions = arg9
 *   - NtOpenFile  : CreateDisposition = arg7 ، CreateOptions = arg8
 * (لأن NtOpenFile بلا معامل FileAttributes — التوقيع من Win7 x64؛
 *  انظر §9.13 من سجل التحقق)
 *
 * السلوك بعد T-023:
 *   - المسار → openat() (على جذر ويندوز، أو على fd مجلد RootDirectory).
 *   - fd → جدول المقابض (HANDLE شفاف) ⇒ المقبض ليس رقم fd.
 *   - ShareAccess يُفحص (T-024) ⇒ STATUS_SHARING_VIOLATION عند التعارض.
 */
static long ntb0_open_file(uint64_t *handle_out, uint32_t desired,
                            uint32_t share, ntb0_object_attributes *oa,
                            ntb0_io_status_block *iosb,
                            uint32_t disp, uint32_t opts)
{
    char path[NTB0_PATH_MAX];
    long rc;
    int flags, dflags, fd;
    int dirfd;

    if (!handle_out || !oa)
        return NTB0_ST_INVALID_PARAMETER;

    if (oa->length != 0 && oa->length < sizeof(ntb0_object_attributes))
        return NTB0_ST_INVALID_PARAMETER;

    /*
     * RootDirectory (T-025): مسار نسبي داخل مجلد مفتوح.
     *   - RootDirectory يجب أن يكون مقبضاً من جدول المقابض ويمثّل مجلداً.
     *   - المسار يجب أن يكون نسبياً (يُرفض المطلق).
     */
    if (oa->root_directory) {
        dirfd = ntb0_handle_dirfd((uint64_t)(uintptr_t)oa->root_directory);
        if (dirfd < 0)
            return NTB0_ST_INVALID_HANDLE;

        rc = ntb0_path_rel_from_nt(oa->object_name, path, sizeof(path));
    } else {
        dirfd = ntb0_path_dirfd();

        rc = ntb0_path_ensure_root();
        if (rc != NTB0_ST_SUCCESS)
            return rc;

        rc = ntb0_path_from_nt(oa->object_name, path, sizeof(path));
    }

    if (rc != NTB0_ST_SUCCESS) {
        if (iosb)
            iosb->status = (int32_t)rc;
        return rc;
    }

    flags  = ntb0_access_to_posix(desired);
    dflags = ntb0_disposition_to_posix(disp);
    if (dflags < 0)
        return NTB0_ST_INVALID_PARAMETER;
    flags |= dflags;

    if (opts & NTB0_FILE_DIRECTORY_FILE)
        flags |= O_DIRECTORY;

    fd = openat(dirfd, path, flags, 0644);
    if (fd < 0) {
        rc = ntb0_errno_to_ntstatus(errno);
        if (iosb) {
            iosb->status = (int32_t)rc;
            iosb->information = 0;
        }
        return rc;
    }

    /* FILE_NON_DIRECTORY_FILE: ارفض المجلدات */
    if (opts & NTB0_FILE_NON_DIRECTORY_FILE) {
        struct stat st;

        if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
            close(fd);
            if (iosb)
                iosb->status = (int32_t)NTB0_ST_FILE_IS_A_DIRECTORY;
            return NTB0_ST_FILE_IS_A_DIRECTORY;
        }
    }

    /*
     * إلى جدول المقابض: يفحص المشاركة ويُنشئ المقبض الشفاف.
     * عند الفشل يُغلق ntb0_handle_create الـ fd داخلياً (لا يتسرّب).
     */
    rc = ntb0_handle_create(fd, desired, share, path, handle_out);
    if (rc != NTB0_ST_SUCCESS) {
        if (iosb) {
            iosb->status = (int32_t)rc;
            iosb->information = 0;
        }
        return rc;
    }

    if (iosb) {
        iosb->status = 0;
        iosb->information = 0;
    }
    return NTB0_ST_SUCCESS;
}

/*
 * SYS-005 — NtCreateFile(
 *              PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
 *              POBJECT_ATTRIBUTES ObjectAttributes,
 *              PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize,
 *              ULONG FileAttributes, ULONG ShareAccess,
 *              ULONG CreateDisposition, ULONG CreateOptions,
 *              PVOID EaBuffer, ULONG EaLength)
 *
 *   a1 = FileHandle*        a2 = DesiredAccess      a3 = ObjectAttributes*
 *   a4 = IoStatusBlock*     a5 = AllocationSize*     a6 = FileAttributes
 *   a7 = ShareAccess        a8 = CreateDisposition   a9 = CreateOptions
 *   a10 = EaBuffer          a11 = EaLength
 *
 *   الترجمة:
 *     - مسار NT (UNICODE_STRING) → مسار لينكس نسبي لجذر ويندوز
 *       (ntb0_path.c — مصابيح PATH-xxx)
 *     - openat() على جذر ويندوز → fd = المقبض الذي تراه ويندوز
 *     - CreateDisposition → O_CREAT/O_EXCL/O_TRUNC
 *     - DesiredAccess → O_RDONLY/O_WRONLY/O_RDWR (+O_APPEND)
 *     - CreateOptions → O_DIRECTORY
 *
 *   حدود موثّقة (لا تُخفى):
 *     - AllocationSize / FileAttributes / EaBuffer / ShareAccess لا تُترجَم
 *       (POSIX لا يملكها، ووضع المشاركة يحتاج جدول مقابض).
 *     - RootDirectory ≠ NULL (مسار نسبي) → NOT_SUPPORTED.
 *     - FILE_SUPERSEDE يُترجَم بـ O_TRUNC|O_CREAT (نفس الـ inode، لا استبدال).
 */
static long ntb0_NtCreateFile(long a1, long a2, long a3, long a4, long a5,
                              long a6, long a7, long a8, long a9, long a10,
                              long a11)
{
    (void)a5;   /* AllocationSize */
    (void)a6;   /* FileAttributes  */
    (void)a10;  /* EaBuffer        */
    (void)a11;  /* EaLength        */

    /* CreateDisposition = arg8 ، CreateOptions = arg9 ، ShareAccess = arg7 */
    return ntb0_open_file((uint64_t *)(uintptr_t)a1, (uint32_t)a2,
                          (uint32_t)a7,
                          (ntb0_object_attributes *)(uintptr_t)a3,
                          (ntb0_io_status_block *)(uintptr_t)a4,
                          (uint32_t)a8, (uint32_t)a9);
}

/*
 * SYS-006 — NtOpenFile(
 *              PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
 *              POBJECT_ATTRIBUTES ObjectAttributes,
 *              PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize,
 *              ULONG ShareAccess, ULONG CreateDisposition,
 *              ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength)
 *
 *   a1 = FileHandle*        a2 = DesiredAccess      a3 = ObjectAttributes*
 *   a4 = IoStatusBlock*     a5 = AllocationSize*     a6 = ShareAccess
 *   a7 = CreateDisposition   a8 = CreateOptions      a9 = EaBuffer
 *   a10 = EaLength
 *
 *   ملاحظة إزاحة: CreateDisposition هنا arg7 (لا arg8) — لأن توقيع
 *   NtOpenFile أقصر بمعامل واحد من NtCreateFile (لا FileAttributes).
 *   الأرقام مُقاسة من ntdll الحقيقي (جدول §4.3) والتوقيع من Win7 x64.
 *
 *   حدود موثّقة:
 *     - AllocationSize لا معنى له لفتح ملف قائم (يُتجاهل).
 *     - CreateDisposition هنا عملياً FILE_OPEN (القيم الأخرى مدعومة أيضاً
 *       عبر نفس جدول الترجمة).
 */
static long ntb0_NtOpenFile(long a1, long a2, long a3, long a4, long a5,
                            long a6, long a7, long a8, long a9, long a10,
                            long a11)
{
    (void)a5;   /* AllocationSize — بلا معنى لفتح قائم */
    (void)a9;   /* EaBuffer */
    (void)a10;  /* EaLength */
    (void)a11;

    /* CreateDisposition = arg7 ، CreateOptions = arg8 ، ShareAccess = arg6 */
    return ntb0_open_file((uint64_t *)(uintptr_t)a1, (uint32_t)a2,
                          (uint32_t)a6,
                          (ntb0_object_attributes *)(uintptr_t)a3,
                          (ntb0_io_status_block *)(uintptr_t)a4,
                          (uint32_t)a7, (uint32_t)a8);
}

/* ------------------------------------------------------------------ */
/* SYS-003 / SYS-004 — NtReadFile / NtWriteFile                          */
/* ------------------------------------------------------------------ */

/*
 * فحص المقبض: هل fd صالح؟ وما وضع الوصول (O_RDONLY/O_WRONLY/O_RDWR)؟
 * fcntl(F_GETFL) يُرجع -1 مع EBADF إذا كان المقبض مغلقاً أو غير صالح.
 * (EINTR/آخر لا تعني "مقبض غير صالح" هنا لأن F_GETFL لا يُحجب)
 */
static int ntb0_handle_mode(int fd, int *accmode)
{
    int fl = fcntl(fd, F_GETFL);

    if (fl < 0)
        return -1;
    if (accmode)
        *accmode = fl & O_ACCMODE;
    return 0;
}

/*
 * SYS-003 — NtReadFile(
 *              HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
 *              PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock,
 *              PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset,
 *              PLARGE_INTEGER ApcRoutineArgument)
 *
 *   a1 = FileHandle    a2 = Event    a3 = ApcRoutine   a4 = ApcContext
 *   a5 = IoStatusBlock*  a6 = Buffer  a7 = Length  a8 = ByteOffset*
 *   a9 = ApcRoutineArgument
 *
 *   الترجمة:
 *     - المقبض = fd (متّسق مع SYS-001/SYS-005/SYS-006).
 *     - ByteOffset != NULL → pread() عند الإزاحة (بلا تغيير موضع الملف).
 *     - ByteOffset == NULL → read() من الموضع الحالي (يتقدّم).
 *     - IoStatusBlock.Information = عدد البايتات المقروءة.
 *     - قراءة 0 بايت عند نهاية الملف → STATUS_END_OF_FILE.
 *
 *   حدود موثّقة:
 *     - القراءة غير المتزامنة (Event أو ApcRoutine ≠ NULL) → NOT_SUPPORTED.
 *     - ApcRoutineArgument / ApcContext يُتجاهلان (متزامنة فقط).
 */
static long ntb0_NtReadFile(long a1, long a2, long a3, long a4, long a5,
                            long a6, long a7, long a8, long a9, long a10,
                            long a11)
{
    int fd;
    ntb0_io_status_block *iosb = (ntb0_io_status_block *)(uintptr_t)a5;
    unsigned char *buf = (unsigned char *)(uintptr_t)a6;
    uint32_t len = (uint32_t)a7;
    int64_t *off = (int64_t *)(uintptr_t)a8;
    int accmode = 0;
    ssize_t n;
    long rc;

    (void)a4;  /* ApcContext */
    (void)a9;  /* ApcRoutineArgument */
    (void)a10;
    (void)a11;

    /* غير متزامن: Event أو ApcRoutine ≠ NULL ⇒ submit إلى الطابور (T-071/072).
     * المقبض والمقالَب تحقق هنا أولاً حتى يعود الرفض الصحيح من المترجم. */
    if (a2 != 0 || a3 != 0) {
        long pre;
        int pm = 0;
        if (ntb0_handle_fd((uint64_t)a1) < 0)
            return NTB0_ST_INVALID_HANDLE;
        if (ntb0_handle_mode(ntb0_handle_fd((uint64_t)a1), &pm) < 0)
            return NTB0_ST_INVALID_HANDLE;
        if (pm == O_WRONLY)
            return NTB0_ST_ACCESS_DENIED;
        if (!buf && len)
            return NTB0_ST_INVALID_PARAMETER;
        pre = ntb0_async_submit_read_ex((uint64_t)a1, (uint64_t)a2, buf, len,
                                        iosb, off ? *off : -1,
                                        (void *)(uintptr_t)a3,
                                        (uint64_t)a4, NULL, NULL);
        return pre;
    }

    fd = ntb0_handle_fd((uint64_t)a1);
    if (fd < 0)
        return NTB0_ST_INVALID_HANDLE;

    if (ntb0_handle_mode(fd, &accmode) < 0)
        return NTB0_ST_INVALID_HANDLE;

    if (!buf && len)
        return NTB0_ST_INVALID_PARAMETER;

    /* مقبض مفتوح للكتابة فقط → لا قراءة */
    if (accmode == O_WRONLY)
        return NTB0_ST_ACCESS_DENIED;

    if (len == 0) {
        if (iosb) {
            iosb->status = 0;
            iosb->information = 0;
        }
        return NTB0_ST_SUCCESS;
    }

    if (off) {
        if (*off < 0)
            return NTB0_ST_INVALID_PARAMETER;
        n = pread(fd, buf, len, (off_t)*off);
    } else {
        n = read(fd, buf, len);
    }

    if (n < 0) {
        rc = ntb0_errno_to_ntstatus(errno);
        if (iosb) {
            iosb->status = (int32_t)rc;
            iosb->information = 0;
        }
        return rc;
    }

    if (n == 0) {
        /* نهاية الملف — كما في ويندوز: STATUS_END_OF_FILE */
        if (iosb) {
            iosb->status = (int32_t)NTB0_ST_END_OF_FILE;
            iosb->information = 0;
        }
        return NTB0_ST_END_OF_FILE;
    }

    if (iosb) {
        iosb->status = 0;
        iosb->information = (uint64_t)n;
    }
    return NTB0_ST_SUCCESS;
}

/*
 * SYS-004 — NtWriteFile(
 *              HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
 *              PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock,
 *              PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset,
 *              PLARGE_INTEGER ApcRoutineArgument)
 *
 *   a1 = FileHandle    a2 = Event    a3 = ApcRoutine   a4 = ApcContext
 *   a5 = IoStatusBlock*  a6 = Buffer  a7 = Length  a8 = ByteOffset*
 *   a9 = ApcRoutineArgument
 *
 *   الترجمة:
 *     - ByteOffset != NULL → pwrite() عند الإزاحة (بلا تغيير موضع الملف).
 *     - ByteOffset == NULL → write() من الموضع الحالي (يتقدّم).
 *     - IoStatusBlock.Information = عدد البايتات المكتوبة.
 *
 *   حدود موثّقة:
 *     - الكتابة غير المتزامنة (Event أو ApcRoutine ≠ NULL) → NOT_SUPPORTED.
 *     - Append (O_APPEND) يُتجاهل عند تمرير ByteOffset (سلوك POSIX).
 */
static long ntb0_NtWriteFile(long a1, long a2, long a3, long a4, long a5,
                             long a6, long a7, long a8, long a9, long a10,
                             long a11)
{
    int fd;
    ntb0_io_status_block *iosb = (ntb0_io_status_block *)(uintptr_t)a5;
    const unsigned char *buf = (const unsigned char *)(uintptr_t)a6;
    uint32_t len = (uint32_t)a7;
    int64_t *off = (int64_t *)(uintptr_t)a8;
    int accmode = 0;
    ssize_t n;
    long rc;

    (void)a4;
    (void)a9;
    (void)a10;
    (void)a11;

    /* غير متزامن: Event أو ApcRoutine ≠ NULL ⇒ submit إلى الطابور (T-071/073). */
    if (a2 != 0 || a3 != 0) {
        long pre;
        int pm = 0;
        if (ntb0_handle_fd((uint64_t)a1) < 0)
            return NTB0_ST_INVALID_HANDLE;
        if (ntb0_handle_mode(ntb0_handle_fd((uint64_t)a1), &pm) < 0)
            return NTB0_ST_INVALID_HANDLE;
        if (pm == O_RDONLY)
            return NTB0_ST_ACCESS_DENIED;
        if (!buf && len)
            return NTB0_ST_INVALID_PARAMETER;
        pre = ntb0_async_submit_write_ex((uint64_t)a1, (uint64_t)a2, buf, len,
                                         iosb, off ? *off : -1,
                                         (void *)(uintptr_t)a3,
                                         (uint64_t)a4, NULL, NULL);
        return pre;
    }

    fd = ntb0_handle_fd((uint64_t)a1);
    if (fd < 0)
        return NTB0_ST_INVALID_HANDLE;

    if (ntb0_handle_mode(fd, &accmode) < 0)
        return NTB0_ST_INVALID_HANDLE;

    if (!buf && len)
        return NTB0_ST_INVALID_PARAMETER;

    /* مقبض مفتوح للقراءة فقط → ACCESS_DENIED (وليس EBADF) */
    if (accmode == O_RDONLY)
        return NTB0_ST_ACCESS_DENIED;

    if (len == 0) {
        if (iosb) {
            iosb->status = 0;
            iosb->information = 0;
        }
        return NTB0_ST_SUCCESS;
    }

    if (off) {
        if (*off < 0)
            return NTB0_ST_INVALID_PARAMETER;
        n = pwrite(fd, buf, len, (off_t)*off);
    } else {
        n = write(fd, buf, len);
    }

    if (n < 0) {
        rc = ntb0_errno_to_ntstatus(errno);
        if (iosb) {
            iosb->status = (int32_t)rc;
            iosb->information = 0;
        }
        return rc;
    }

    if (iosb) {
        iosb->status = 0;
        iosb->information = (uint64_t)n;
    }
    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SYS-056 — NtCancelIoFile / NtCancelIoFileEx (T-071 باشتماله)        */
/* ------------------------------------------------------------------ */

/*
 * NtCancelIoFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock)
 *   a1 = FileHandle    a2 = IoStatusBlock*
 * NtCancelIoFileEx(HANDLE FileHandle, PVOID ExRequest, PIO_STATUS_BLOCK IoStatusBlock)
 *   a3 = ExRequest (IO_CANCEL_INFORMATION — يُتجاهل محتواه: الإلغاء بالكامل)
 *
 *   الترجمة:
 *     - كل عقد PENDING على هذا المقبض ⇒ STATUS_CANCELLED (0xC0000120) +
 *       إشارة eventfd (كما في ويندوز: الإلغاء يوقظ الانتظار).
 *     - العقد الجارية تكتمل بـ STATUS_CANCELLED (دلالة IRP-cancel-after-start).
 *     - لا عقد معلّقة ⇒ نجاح هادئ (سلوك NT).
 *
 *   حدود موثّقة:
 *     - ExRequest (معامل 3) يُتجاهل — لا فرق per-thread/per-file داخل الصندوق.
 */
static long ntb0_NtCancelIoFile(long a1, long a2, long a3, long a4, long a5,
                                long a6, long a7, long a8, long a9, long a10,
                                long a11)
{
    ntb0_io_status_block *iosb = (ntb0_io_status_block *)(uintptr_t)a2;

    (void)a3;   /* ExRequest (CancelIoFileEx) */
    (void)a4; (void)a5; (void)a6; (void)a7; (void)a8;
    (void)a9; (void)a10; (void)a11;

    if (ntb0_handle_fd((uint64_t)a1) < 0)
        return NTB0_ST_INVALID_HANDLE;

    ntb0_async_cancel((uint64_t)a1);

    if (iosb) {
        iosb->status = 0;
        iosb->information = 0;
    }
    /* المصباح التلقائي في نهاية المترجم يصدر [SYS-056][OK] عند 0x0 */
    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SYS-058 — NtSetValueKey (T-109) + REG_LINK (T-108)                   */
/* ------------------------------------------------------------------ */

/*
 * NtSetValueKey(
 *   HANDLE KeyHandle,           a1
 *   PUNICODE_STRING ValueName,  a2
 *   ULONG TitleIndex,           a3   (غير مُستخدَم)
 *   ULONG Type,                 a4
 *   PVOID Data,                 a5
 *   ULONG DataSize)             a6
 *
 *   الترجمة (طبقة التراكب في الذاكرة — الحدّ الصادق: تفنى بإغلاق الخلية):
 *     - Type == REG_LINK (6): البيانات = نص UTF-16 خام للهدف ⇒ رابط رمزي
 *       (ntb0_reg_set_link) ينشئ مفتاحاً رمزياً تحت المفتاح الحالي.
 *     - غير ذلك ⇒ ntb0_reg_set_value (كتابة تراكب مُقاسة REG-004).
 */
static long ntb0_NtSetValueKey(long a1, long a2, long a3, long a4, long a5,
                               long a6, long a7, long a8, long a9, long a10,
                               long a11)
{
    ntb0_unicode_string *name = (ntb0_unicode_string *)(uintptr_t)a2;
    uint32_t type = (uint32_t)a4;
    void *data = (void *)(uintptr_t)a5;
    uint32_t size = (uint32_t)a6;

    (void)a3;
    (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!name)
        return NTB0_ST_INVALID_PARAMETER;

    if (type == 6 /* REG_LINK */) {
        /* البيانات REG_LINK = نص UTF-16 خام (بلا غلاف UNICODE_STRING). */
        ntb0_unicode_string target;

        if (!data || size < 2)
            return NTB0_ST_INVALID_PARAMETER;
        target.length = (uint16_t)size;
        target.maximum_length = (uint16_t)size;
        target.buffer = (uint16_t *)data;
        return ntb0_reg_set_link((uint64_t)a1, name, &target);
    }

    return ntb0_reg_set_value((uint64_t)a1, name, type, data, size);
}

/* ------------------------------------------------------------------ */
/* SYS-059 — NtLoadKey (T-107 — RegLoadAppKey)                          */
/* ------------------------------------------------------------------ */

/*
 * NtLoadKey(
 *   POBJECT_ATTRIBUTES TargetKey,    a1   (اسم التركيب — غير مُستخدَم هنا)
 *   POBJECT_ATTRIBUTES SourceFile)   a2   → ObjectName = مسار ملف الخلية
 *
 *   الترجمة:
 *     - مسار ملف الخلية (\??\C:\Windows\System32\config\SOFTWARE وغيره)
 *       ⇒ مسار لينكس عبر ntb0_path_from_nt + جذر الصندوق ⇒ تحميل الخلية
 *       كخلية تطبيق (جدول شرائح محدود — RegLoadAppKey في NT كذلك).
 *     - **حدّ صادق**: التركيب دائم للقرية المفتوحة فقط — لا كتابة على القرص.
 */
static long ntb0_NtLoadKey(long a1, long a2, long a3, long a4, long a5,
                           long a6, long a7, long a8, long a9, long a10,
                           long a11)
{
    ntb0_object_attributes *src = (ntb0_object_attributes *)(uintptr_t)a2;
    uint64_t handle = 0;
    char rel[512], abs[600];
    long rc;

    (void)a1; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    (void)a8; (void)a9; (void)a10; (void)a11;

    if (!src || !src->object_name)
        return NTB0_ST_INVALID_PARAMETER;

    rc = ntb0_path_from_nt(src->object_name, rel, sizeof(rel));
    if (rc != NTB0_ST_SUCCESS)
        return rc;

    snprintf(abs, sizeof(abs), "%s/%s", ntb0_path_root(), rel);

    /* T-107: التحميل إلى جدول شرائح التطبيقات (REG-002) — الحدّ الصادق:
     * التركيب بمسار TargetKey غير مُنفَّذ بعد؛ الخلية تبقى قابلة للوصول
     * عبر نُظم `RegLoadAppKey` داخل الصندوق فقط، ولا تُكتب على القرص. */
    rc = ntb0_reg_load_app_key(&handle, abs);
    if (rc != NTB0_ST_SUCCESS)
        return rc;

    /* NT يركّب الخلية بالمسار ولا يعيد مقبضاً ⇒ نُفلت المقبض (يبقى الضرر
     * صفراً: إغلاق المقبض لا يُفريغ الخلية من جدول الشرائح). */
    ntb0_handle_close(handle);
    return NTB0_ST_SUCCESS;
}

static long ntb0_NtReleaseSemaphore(long a1, long a2, long a3, long a4, long a5,
                                    long a6, long a7, long a8, long a9, long a10,
                                    long a11)
{
    uint64_t handle = (uint64_t)a1;
    long release = a2;
    int32_t *prev = (int32_t *)(uintptr_t)a3;
    int fd;
    uint64_t val;

    (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    fd = ntb0_handle_fd(handle);
    if (fd < 0)
        return NTB0_ST_INVALID_HANDLE;

    val = release > 0 ? (uint64_t)release : 1;
    if (write(fd, &val, sizeof(val)) != (ssize_t)sizeof(val))
        return ntb0_errno_to_ntstatus(errno);

    if (prev)
        *prev = 0;

    return NTB0_ST_SUCCESS;
}

static long ntb0_NtReleaseMutant(long a1, long a2, long a3, long a4, long a5,
                                 long a6, long a7, long a8, long a9, long a10,
                                 long a11)
{
    uint64_t handle = (uint64_t)a1;
    int32_t *prev = (int32_t *)(uintptr_t)a2;

    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (ntb0_handle_kind(handle) < 0 && ntb0_handle_fd(handle) < 0)
        return NTB0_ST_INVALID_HANDLE;

    if (prev)
        *prev = 1;

    return NTB0_ST_SUCCESS;
}

static long ntb0_NtQueryInformationToken(long a1, long a2, long a3, long a4,
                                          long a5, long a6, long a7, long a8,
                                          long a9, long a10, long a11)
{
    uint64_t handle = (uint64_t)a1;
    uint32_t cls = (uint32_t)a2;
    unsigned char *out = (unsigned char *)(uintptr_t)a3;
    uint32_t len = (uint32_t)a4;
    uint32_t *ret = (uint32_t *)(uintptr_t)a5;

    (void)handle; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!out)
        return NTB0_ST_INVALID_PARAMETER;

    if (cls == 1) {
        if (len < 0x10)
            return NTB0_ST_STATUS_LOGICAL_ERROR;
        memset(out, 0, len);
        *(uint64_t *)(out + 0x00) = (uint64_t)(uintptr_t)(out + 0x10);
        if (ret)
            *ret = 0x10;
        return NTB0_ST_SUCCESS;
    }

    return NTB0_ST_NOT_SUPPORTED;
}

static long ntb0_NtQueryTimer(long a1, long a2, long a3, long a4, long a5,
                              long a6, long a7, long a8, long a9, long a10,
                              long a11)
{
    uint64_t handle = (uint64_t)a1;
    uint32_t cls = (uint32_t)a2;
    unsigned char *out = (unsigned char *)(uintptr_t)a3;
    uint32_t len = (uint32_t)a4;
    uint32_t *ret = (uint32_t *)(uintptr_t)a5;

    (void)handle; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!out)
        return NTB0_ST_INVALID_PARAMETER;
    if (cls != 1)
        return NTB0_ST_NOT_SUPPORTED;

    if (len < 16)
        return NTB0_ST_STATUS_LOGICAL_ERROR;

    memset(out, 0, len);
    *(int64_t *)(out + 0x00) = 0;
    *(uint8_t *)(out + 0x08) = 0;
    if (ret)
        *ret = 16;

    return NTB0_ST_SUCCESS;
}

static long ntb0_NtContinue(long a1, long a2, long a3, long a4, long a5,
                            long a6, long a7, long a8, long a9, long a10,
                            long a11)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    (void)a8; (void)a9; (void)a10; (void)a11;

    return NTB0_ST_SUCCESS;
}

static long ntb0_NtQuerySection(long a1, long a2, long a3, long a4, long a5,
                                long a6, long a7, long a8, long a9, long a10,
                                long a11)
{
    uint64_t handle = (uint64_t)a1;
    uint32_t cls = (uint32_t)a2;
    unsigned char *out = (unsigned char *)(uintptr_t)a3;
    uint32_t len = (uint32_t)a4;
    uint32_t *ret = (uint32_t *)(uintptr_t)a5;
    uint64_t sec_size;

    (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!out)
        return NTB0_ST_INVALID_PARAMETER;

    if (ntb0_handle_kind(handle) != NTB0_HK_SECTION)
        return NTB0_ST_INVALID_HANDLE;

    if (cls != 1)
        return NTB0_ST_NOT_SUPPORTED;

    if (len < 0x18)
        return NTB0_ST_STATUS_LOGICAL_ERROR;

    sec_size = ntb0_handle_size(handle);
    memset(out, 0, len);
    *(uint64_t *)(out + 0x00) = 0;
    *(uint32_t *)(out + 0x08) = 0x08000000;
    *(uint64_t *)(out + 0x10) = sec_size;

    if (ret)
        *ret = 0x18;

    return NTB0_ST_SUCCESS;
}

static long ntb0_NtAreMappedFilesTheSame(long a1, long a2, long a3, long a4,
                                          long a5, long a6, long a7, long a8,
                                          long a9, long a10, long a11)
{
    uintptr_t f1 = (uintptr_t)a1;
    uintptr_t f2 = (uintptr_t)a2;
    uintptr_t s1 = 0, e1 = 0, s2 = 0, e2 = 0;

    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (ntb0_vm_region(f1, &s1, &e1, NULL) < 0 ||
        ntb0_vm_region(f2, &s2, &e2, NULL) < 0)
        return NTB0_ST_INVALID_PARAMETER;

    if (s1 == s2)
        return NTB0_ST_SUCCESS;

    return NTB0_ST_NOT_SUPPORTED;
}

static long ntb0_NtCreateMutant(long a1, long a2, long a3, long a4, long a5,
                                long a6, long a7, long a8, long a9, long a10,
                                long a11)
{
    uint64_t *out = (uint64_t *)(uintptr_t)a1;
    int efd;
    long rc;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!out)
        return NTB0_ST_INVALID_PARAMETER;

    efd = eventfd(1, EFD_CLOEXEC | EFD_NONBLOCK);
    if (efd < 0)
        return ntb0_errno_to_ntstatus(errno);

    rc = ntb0_handle_create_event(efd, out);
    if (rc != NTB0_ST_SUCCESS)
        close(efd);

    return rc;
}

static long ntb0_NtCreateSemaphore(long a1, long a2, long a3, long a4, long a5,
                                   long a6, long a7, long a8, long a9, long a10,
                                   long a11)
{
    uint64_t *out = (uint64_t *)(uintptr_t)a1;
    long initial = a4;
    long maximum = a5;
    int efd;
    uint64_t val;
    long rc;

    (void)a2; (void)a3; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!out)
        return NTB0_ST_INVALID_PARAMETER;

    (void)maximum;
    efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
    if (efd < 0)
        return ntb0_errno_to_ntstatus(errno);

    if (initial > 0) {
        val = (uint64_t)initial;
        if (write(efd, &val, sizeof(val)) < 0) {
            close(efd);
            return ntb0_errno_to_ntstatus(errno);
        }
    }

    rc = ntb0_handle_create_event(efd, out);
    if (rc != NTB0_ST_SUCCESS)
        close(efd);

    return rc;
}

static long ntb0_NtCreateTimer(long a1, long a2, long a3, long a4, long a5,
                               long a6, long a7, long a8, long a9, long a10,
                               long a11)
{
    uint64_t *out = (uint64_t *)(uintptr_t)a1;
    int efd;
    long rc;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!out)
        return NTB0_ST_INVALID_PARAMETER;

    efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (efd < 0)
        return ntb0_errno_to_ntstatus(errno);

    rc = ntb0_handle_create_event(efd, out);
    if (rc != NTB0_ST_SUCCESS)
        close(efd);

    return rc;
}

static long ntb0_NtExtendSection(long a1, long a2, long a3, long a4, long a5,
                                 long a6, long a7, long a8, long a9, long a10,
                                 long a11)
{
    uint64_t handle = (uint64_t)a1;
    int64_t *size_ptr = (int64_t *)(uintptr_t)a2;
    int fd;

    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!size_ptr)
        return NTB0_ST_INVALID_PARAMETER;

    if (ntb0_handle_kind(handle) != NTB0_HK_SECTION)
        return NTB0_ST_INVALID_HANDLE;

    fd = ntb0_handle_fd(handle);
    if (fd < 0)
        return NTB0_ST_INVALID_HANDLE;

    if (ftruncate(fd, (off_t)*size_ptr) < 0)
        return ntb0_errno_to_ntstatus(errno);

    return NTB0_ST_SUCCESS;
}

static long ntb0_NtFlushInstructionCache(long a1, long a2, long a3, long a4,
                                          long a5, long a6, long a7, long a8,
                                          long a9, long a10, long a11)
{
    void *addr = (void *)(uintptr_t)a2;
    size_t len = (size_t)a3;

    (void)a1; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    #if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache(addr, (char *)addr + len);
    #else
    (void)addr;
    (void)len;
    #endif

    return NTB0_ST_SUCCESS;
}

static long ntb0_NtGetContextThread(long a1, long a2, long a3, long a4, long a5,
                                   long a6, long a7, long a8, long a9, long a10,
                                   long a11)
{
    uint64_t handle = (uint64_t)a1;
    unsigned char *ctx = (unsigned char *)(uintptr_t)a2;

    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!ctx)
        return NTB0_ST_INVALID_PARAMETER;

    if (ntb0_handle_kind(handle) != NTB0_HK_THREAD)
        return NTB0_ST_INVALID_HANDLE;

    /* x64 CONTEXT هو 0x4D0 بايت (flags عند 0x30، ملفات RSP/RIP عند 0xF8/0xF8)
     * — ملء 0x200 يترك الحقول عالية الصفر، الفرز اللاحق سيقرأ بيانات مقلصة */
    memset(ctx, 0, 0x4D0);
    return NTB0_ST_SUCCESS;
}

static long ntb0_NtRaiseException(long a1, long a2, long a3, long a4, long a5,
                                  long a6, long a7, long a8, long a9, long a10,
                                  long a11)
{
    const unsigned char *rec = (const unsigned char *)(uintptr_t)a1;

    fprintf(stderr,
            "[ntbox][UNHANDLED-RAISE] NtRaiseException: code=0x%x flags=0x%x "
            "addr=0x%lx — no dispatcher: terminating deterministically\n",
            rec ? *(const uint32_t *)(uintptr_t)rec : 0,
            rec ? *(const uint32_t *)(uintptr_t)(rec + 4) : 0,
            (unsigned long)(uintptr_t)a3);

    (void)a2; (void)a4; (void)a5; (void)a6; (void)a7;
    (void)a8; (void)a9; (void)a10; (void)a11;

    /*
     * أعقدية حرجة (مثبتة بالتحليل): ntdll!RtlRaiseStatus (0x78f1d7c0) يستدعي
     * NtRaiseException ثم يعيد التكرار بلا شرط على أي قيمة مرجعة (لمسار
     * "mov %eax,%ecx; call RtlRaiseStatus"). أي إرجاع هنا ⇒ حلقة
     * +0x5A0 بايت لكل تكرار حتى انفجار المكدس (Typed exit=139).
     * العقد الآمن: إنهاء حاسم بدل الإرجاع (يحاكي الموت غير المعالج).
     */
    exit(0xC0000001 & 0xff); /* رمز خروج مرئي وحاسم — يمنع الحلقات الخفية */
}

static long ntb0_NtSetContextThread(long a1, long a2, long a3, long a4, long a5,
                                   long a6, long a7, long a8, long a9, long a10,
                                   long a11)
{
    uint64_t handle = (uint64_t)a1;
    unsigned char *ctx = (unsigned char *)(uintptr_t)a2;

    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!ctx)
        return NTB0_ST_INVALID_PARAMETER;

    if (ntb0_handle_kind(handle) != NTB0_HK_THREAD)
        return NTB0_ST_INVALID_HANDLE;

    return NTB0_ST_SUCCESS;
}

static long ntb0_NtLockVirtualMemory(long a1, long a2, long a3, long a4, long a5,
                                     long a6, long a7, long a8, long a9, long a10,
                                     long a11)
{
    unsigned long *base_ptr = (unsigned long *)(uintptr_t)a2;
    unsigned long *size_ptr = (unsigned long *)(uintptr_t)a3;

    (void)a1; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!base_ptr || !size_ptr || *size_ptr == 0)
        return NTB0_ST_INVALID_PARAMETER;

    if (mlock((void *)(uintptr_t)*base_ptr, (size_t)*size_ptr) < 0)
        return ntb0_errno_to_ntstatus(errno);

    return NTB0_ST_SUCCESS;
}

static long ntb0_NtUnlockVirtualMemory(long a1, long a2, long a3, long a4, long a5,
                                       long a6, long a7, long a8, long a9, long a10,
                                       long a11)
{
    unsigned long *base_ptr = (unsigned long *)(uintptr_t)a2;
    unsigned long *size_ptr = (unsigned long *)(uintptr_t)a3;

    (void)a1; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!base_ptr || !size_ptr || *size_ptr == 0)
        return NTB0_ST_INVALID_PARAMETER;

    if (munlock((void *)(uintptr_t)*base_ptr, (size_t)*size_ptr) < 0)
        return ntb0_errno_to_ntstatus(errno);

    return NTB0_ST_SUCCESS;
}

static long ntb0_NtSetTimer(long a1, long a2, long a3, long a4, long a5,
                            long a6, long a7, long a8, long a9, long a10,
                            long a11)
{
    uint64_t handle = (uint64_t)a1;
    int fd;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    fd = ntb0_handle_fd(handle);
    if (fd < 0)
        return NTB0_ST_INVALID_HANDLE;

    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SYS-008 — NtDuplicateObject (T-026)                                 */
/* ------------------------------------------------------------------ */

/*
 * NtDuplicateObject(
 *   HANDLE SourceProcessHandle,   a1
 *   HANDLE SourceHandle,          a2
 *   HANDLE TargetProcessHandle,   a3
 *   PHANDLE TargetHandle,         a4
 *   ACCESS_MASK DesiredAccess,    a5
 *   ULONG HandleAttributes,       a6
 *   ULONG Options)                 a7
 *
 *   الترجمة:
 *     - fd جديد (dup) على نفس الملف + **مقبض جديد** في جدول المقابض
 *       ⇒ مقبضان مختلفان لنفس الملف (كما في ويندوز).
 *     - DesiredAccess == 0 ⇒ نفس صلاحية المصدر.
 *     - Options & DUPLICATE_CLOSE_SOURCE (1) ⇒ يُغلق المصدر بعد النسخ.
 *
 *   حدود موثّقة:
 *     - SourceProcessHandle/TargetProcessHandle: يُتجاهلان — المترجم
 *       يعمل داخل عملية واحدة (العملية الحالية) ⇒ لا نسخ بين عمليات.
 *     - HandleAttributes ( inheritable / protect-from-close ) لا تُترجَم.
 */
static long ntb0_NtDuplicateObject(long a1, long a2, long a3, long a4,
                                    long a5, long a6, long a7, long a8,
                                    long a9, long a10, long a11)
{
    uint64_t *out = (uint64_t *)(uintptr_t)a4;
    uint32_t desired = (uint32_t)a5;
    uint32_t opts = (uint32_t)a7;
    long rc;

    (void)a1; /* SourceProcessHandle — عملية واحدة فقط */
    (void)a3; /* TargetProcessHandle */
    (void)a6; /* HandleAttributes */
    (void)a8; (void)a9; (void)a10; (void)a11;

    if (!out)
        return NTB0_ST_INVALID_PARAMETER;

    rc = ntb0_handle_duplicate((uint64_t)a2, desired, out);
    if (rc != NTB0_ST_SUCCESS)
        return rc;

    /* DUPLICATE_CLOSE_SOURCE */
    if (opts & 0x1u)
        ntb0_handle_close((uint64_t)a2);

    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* helpers للذاكرة (T-027 / T-028 / T-029)                            */
/* ------------------------------------------------------------------ */

/*
 * يبحث عن منطقة الذاكرة التي تحتوي العنوان في /proc/self/maps.
 * يُعيد 0 عند النجاح ويملاّ start/end/prot.
 * prot = بتات PROT_* كما يقرؤها proc.
 */
static int ntb0_vm_region(uintptr_t addr, uintptr_t *start, uintptr_t *end,
                          int *prot)
{
    FILE *f;
    char line[512];
    int found = 0;

    f = fopen("/proc/self/maps", "r");
    if (!f)
        return -1;

    while (fgets(line, sizeof(line), f)) {
        unsigned long lo, hi;
        char perms[8];

        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3)
            continue;
        if (addr >= (uintptr_t)lo && addr < (uintptr_t)hi) {
            int p = 0;

            if (perms[0] == 'r')
                p |= PROT_READ;
            if (perms[1] == 'w')
                p |= PROT_WRITE;
            if (perms[2] == 'x')
                p |= PROT_EXEC;
            *start = (uintptr_t)lo;
            *end = (uintptr_t)hi;
            if (prot)
                *prot = p;
            found = 1;
            break;
        }
    }

    fclose(f);
    return found ? 0 : -1;
}

/* PAGE_* → PROT_* */
static int ntb0_page_to_prot(uint32_t page)
{
    switch (page & 0xFF) {
    case 0x01: return PROT_NONE;                /* PAGE_NOACCESS */
    case 0x02: return PROT_READ;                /* PAGE_READONLY */
    case 0x04: return PROT_READ | PROT_WRITE;   /* PAGE_READWRITE */
    case 0x08: return PROT_READ | PROT_WRITE;   /* PAGE_WRITECOPY */
    case 0x10: return PROT_EXEC;                /* PAGE_EXECUTE */
    case 0x20: return PROT_EXEC | PROT_READ;    /* PAGE_EXECUTE_READ */
    case 0x40: return PROT_EXEC | PROT_READ | PROT_WRITE; /* EXECUTE_RW */
    default:   return PROT_READ | PROT_WRITE;
    }
}

/* PROT_* → PAGE_* (للإرجاع OldProtect) */
static uint32_t ntb0_prot_to_page(int prot)
{
    if (prot == PROT_NONE)
        return 0x01;
    if (prot == PROT_READ)
        return 0x02;
    if (prot == (PROT_READ | PROT_WRITE))
        return 0x04;
    if (prot == PROT_EXEC)
        return 0x10;
    if (prot == (PROT_EXEC | PROT_READ))
        return 0x20;
    if (prot == (PROT_EXEC | PROT_READ | PROT_WRITE))
        return 0x40;
    return 0x04;
}

/* ------------------------------------------------------------------ */
/* SYS-009 — NtFreeVirtualMemory (T-027)                              */
/* ------------------------------------------------------------------ */

/*
 * NtFreeVirtualMemory(
 *   HANDLE ProcessHandle,   a1
 *   PVOID *BaseAddress,     a2
 *   PSIZE_T RegionSize,     a3
 *   ULONG AllocationType,   a4
 *   ULONG FreeType)         a5
 *
 *   الترجمة: munmap() على المنطقة التي تحتوي العنوان (كما في MEM_RELEASE:
 *   الحجم يتجاهَل — المنطقة هي ما عيّنه mmap).
 *   عند النجاح: *BaseAddress = 0 و *RegionSize = 0 (سلوك NT).
 */
static long ntb0_NtFreeVirtualMemory(long a1, long a2, long a3, long a4,
                                      long a5, long a6, long a7, long a8,
                                      long a9, long a10, long a11)
{
    unsigned long *base_ptr = (unsigned long *)(uintptr_t)a2;
    unsigned long *size_ptr = (unsigned long *)(uintptr_t)a3;
    uintptr_t start = 0, end = 0;
    long rc;

    (void)a1; /* ProcessHandle */
    (void)a4; /* AllocationType */
    (void)a5; /* FreeType */
    (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!base_ptr || !size_ptr)
        return NTB0_ST_INVALID_PARAMETER;

    if (*base_ptr == 0)
        return NTB0_ST_INVALID_PARAMETER;

    if (ntb0_vm_region((uintptr_t)*base_ptr, &start, &end, NULL) < 0)
        return NTB0_ST_INVALID_PARAMETER;

    if (munmap((void *)start, (size_t)(end - start)) < 0)
        return ntb0_errno_to_ntstatus(errno);

    *base_ptr = 0;
    *size_ptr = 0;
    rc = NTB0_ST_SUCCESS;
    return rc;
}

/* ------------------------------------------------------------------ */
/* SYS-010 — NtProtectVirtualMemory (T-028)                            */
/* ------------------------------------------------------------------ */

/*
 * NtProtectVirtualMemory(
 *   HANDLE ProcessHandle,   a1
 *   PVOID *BaseAddress,     a2
 *   PSIZE_T RegionSize,     a3
 *   ULONG NewProtect,       a4
 *   PULONG OldProtect)      a5
 *
 *   الترجمة: mprotect() على المنطقة المحدَّدة بالحجم (وليس على المنطقة
 *   الكاملة — NtProtectVirtualMemory يغيّر الحماية للصفحات المطلوبة).
 *   OldProtect = الحماية السابقة (PAGE_*).
 */
static long ntb0_NtProtectVirtualMemory(long a1, long a2, long a3, long a4,
                                         long a5, long a6, long a7, long a8,
                                         long a9, long a10, long a11)
{
    unsigned long *base_ptr = (unsigned long *)(uintptr_t)a2;
    unsigned long *size_ptr = (unsigned long *)(uintptr_t)a3;
    uint32_t *old_ptr = (uint32_t *)(uintptr_t)a5;
    size_t size;
    int prot, oldprot = 0;
    uintptr_t start = 0, end = 0;

    (void)a1; /* ProcessHandle */
    (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!base_ptr || !size_ptr)
        return NTB0_ST_INVALID_PARAMETER;

    size = (size_t)*size_ptr;
    if (size == 0 || *base_ptr == 0)
        return NTB0_ST_INVALID_PARAMETER;

    /* الحماية القديمة: من /proc/self/maps */
    if (ntb0_vm_region((uintptr_t)*base_ptr, &start, &end, &oldprot) == 0 &&
        old_ptr)
        *old_ptr = ntb0_prot_to_page(oldprot);

    prot = ntb0_page_to_prot((uint32_t)a4);

    if (mprotect((void *)(uintptr_t)*base_ptr, size, prot) < 0)
        return ntb0_errno_to_ntstatus(errno);

    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SYS-011 — NtQueryVirtualMemory (T-029)                              */
/* ------------------------------------------------------------------ */

/*
 * NtQueryVirtualMemory(
 *   HANDLE ProcessHandle,            a1
 *   PVOID BaseAddress,               a2
 *   PVOID MemoryInformationClass,    a3
 *   PVOID MemoryInformation,         a4
 *   PSIZE_T MemoryInformationLength, a5
 *   PULONG ReturnLength)             a6
 *
 *   نُدعم MemoryBasicInformation (0) فقط:
 *     typedef struct _MEMORY_BASIC_INFORMATION {
 *         PVOID  BaseAddress;        0x00
 *         PVOID  AllocationBase;     0x08
 *         ULONG  AllocationProtect;  0x10
 *         SIZE_T RegionSize;         0x18
 *         ULONG  State;              0x20
 *         ULONG  Protect;            0x24
 *         ULONG  Type;               0x28
 *     } MEMORY_BASIC_INFORMATION;   (0x30 bytes)
 *
 *   المصدر: /proc/self/maps (قياس حقيقي — لا تقدير).
 */
static long ntb0_NtQueryVirtualMemory(long a1, long a2, long a3, long a4,
                                       long a5, long a6, long a7, long a8,
                                       long a9, long a10, long a11)
{
    uintptr_t base = (uintptr_t)a2;
    uint32_t cls = (uint32_t)a3;
    unsigned char *info = (unsigned char *)(uintptr_t)a4;
    size_t *len_ptr = (size_t *)(uintptr_t)a5;
    uint32_t *ret_ptr = (uint32_t *)(uintptr_t)a6;
    uintptr_t start = 0, end = 0;
    int prot = 0;
    uint64_t *base_p, *alloc_base_p;
    uint32_t *alloc_prot_p, *state_p, *protect_p, *type_p;
    uint64_t *region_size_p;

    (void)a1; /* ProcessHandle */
    (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!info)
        return NTB0_ST_INVALID_PARAMETER;

    /* MemoryBasicInformation = 0 */
    if (cls != 0)
        return NTB0_ST_NOT_SUPPORTED;

    if (len_ptr && *len_ptr < 0x30)
        return NTB0_ST_STATUS_LOGICAL_ERROR; /* STATUS_BUFFER_TOO_SMALL */

    if (ntb0_vm_region(base, &start, &end, &prot) < 0)
        return NTB0_ST_INVALID_PARAMETER;

    memset(info, 0, 0x30);

    base_p        = (uint64_t *)(info + 0x00);
    alloc_base_p  = (uint64_t *)(info + 0x08);
    alloc_prot_p  = (uint32_t *)(info + 0x10);
    region_size_p = (uint64_t *)(info + 0x18);
    state_p       = (uint32_t *)(info + 0x20);
    protect_p     = (uint32_t *)(info + 0x24);
    type_p        = (uint32_t *)(info + 0x28);

    *base_p        = (uint64_t)start;
    *alloc_base_p  = (uint64_t)start;
    *alloc_prot_p  = ntb0_prot_to_page(prot);
    *region_size_p = (uint64_t)(end - start);
    *state_p       = 0x1000;                 /* MEM_COMMIT */
    *protect_p     = ntb0_prot_to_page(prot);
    *type_p        = 0x20000;                /* MEM_PRIVATE */

    if (len_ptr)
        *len_ptr = 0x30;
    if (ret_ptr)
        *ret_ptr = 0x30;

    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SYS-012 — NtQueryInformationFile (T-030)                            */
/* ------------------------------------------------------------------ */

/*
 * NtQueryInformationFile(
 *   HANDLE FileHandle,           a1
 *   PIO_STATUS_BLOCK IoStatusBlock, a2
 *   PVOID FileInformation,       a3
 *   ULONG FileInformationClass,  a4
 *   ULONG Length)                a5
 *
 *   FileStandardInformation (1):
 *     typedef struct _FILE_STANDARD_INFORMATION {
 *         LARGE_INTEGER AllocationSize;  0x00
 *         LARGE_INTEGER EndOfFile;       0x08
 *         ULONG NumberOfLinks;           0x10
 *         BOOLEAN DeletePending;         0x14
 *         BOOLEAN Directory;             0x15
 *     } FILE_STANDARD_INFORMATION;    (0x18 bytes)
 *
 *   FilePositionInformation (2):
 *     LARGE_INTEGER CurrentPosition (0x00) + ULONG CurrentByteOffset (0x08)
 */
static long ntb0_NtQueryInformationFile(long a1, long a2, long a3, long a4,
                                         long a5, long a6, long a7, long a8,
                                         long a9, long a10, long a11)
{
    int fd;
    uint32_t cls = (uint32_t)a4;
    uint32_t len = (uint32_t)a5;
    unsigned char *out = (unsigned char *)(uintptr_t)a3;
    ntb0_io_status_block *iosb = (ntb0_io_status_block *)(uintptr_t)a2;
    struct stat st;

    (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!out)
        return NTB0_ST_INVALID_PARAMETER;

    fd = ntb0_handle_fd((uint64_t)a1);
    if (fd < 0)
        return NTB0_ST_INVALID_HANDLE;

    if (fstat(fd, &st) < 0)
        return ntb0_errno_to_ntstatus(errno);

    if (cls == 1) { /* FileStandardInformation */
        uint64_t *alloc = (uint64_t *)(out + 0x00);
        uint64_t *eof   = (uint64_t *)(out + 0x08);
        uint32_t *links = (uint32_t *)(out + 0x10);
        uint8_t  *delp  = (uint8_t *)(out + 0x14);
        uint8_t  *dirp  = (uint8_t *)(out + 0x15);

        if (len < 0x18)
            return NTB0_ST_INVALID_PARAMETER;

        memset(out, 0, 0x18);
        *alloc = (uint64_t)st.st_blocks * 512;
        *eof   = (uint64_t)st.st_size;
        *links = (uint32_t)st.st_nlink;
        *delp  = 0;
        *dirp  = S_ISDIR(st.st_mode) ? 1 : 0;
    } else if (cls == 2) { /* FilePositionInformation */
        uint64_t *pos = (uint64_t *)(out + 0x00);
        off_t cur;

        if (len < 0x10)
            return NTB0_ST_INVALID_PARAMETER;

        cur = lseek(fd, 0, SEEK_CUR);
        if (cur < 0)
            return ntb0_errno_to_ntstatus(errno);

        memset(out, 0, 0x10);
        *pos = (uint64_t)cur;
    } else {
        return NTB0_ST_NOT_SUPPORTED;
    }

    if (iosb) {
        iosb->status = 0;
        iosb->information = 0;
    }
    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SYS-013 — NtSetInformationFile (T-031)                              */
/* ------------------------------------------------------------------ */

/*
 * NtSetInformationFile(
 *   HANDLE FileHandle,             a1
 *   PIO_STATUS_BLOCK IoStatusBlock, a2
 *   PVOID FileInformation,         a3
 *   ULONG FileInformationClass,    a4
 *   ULONG Length)                  a5
 *
 *   الأصناف المدعومة (قياسات NT):
 *     2  FilePositionInformation     → lseek(fd, pos, SEEK_SET)
 *     5  FileEndOfFileInformation    → ftruncate(fd, size)
 *     10 FileRenameInformation       → rename (اسم NT كامل أو نسبي لمجلد الملف)
 *     13 FileDispositionInformation  → unlink (DeleteFile = TRUE)
 *
 *   FILE_RENAME_INFORMATION (x64):
 *     0x00 BOOLEAN ReplaceIfExists · 0x04 ULONG Reserved
 *     0x08 ULONG RootDirectory      · 0x0C ULONG FileNameLength
 *     0x10 WCHAR FileName[]
 */
static long ntb0_NtSetInformationFile(long a1, long a2, long a3, long a4,
                                      long a5, long a6, long a7, long a8,
                                      long a9, long a10, long a11)
{
    int fd;
    uint32_t cls = (uint32_t)a4;
    unsigned char *info = (unsigned char *)(uintptr_t)a3;
    ntb0_io_status_block *iosb = (ntb0_io_status_block *)(uintptr_t)a2;

    (void)a5; /* Length — كل صنف له حجم ثابت معروف */
    (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!info)
        return NTB0_ST_INVALID_PARAMETER;

    fd = ntb0_handle_fd((uint64_t)a1);
    if (fd < 0)
        return NTB0_ST_INVALID_HANDLE;

    switch (cls) {
    case 2: { /* FilePositionInformation */
        int64_t *pos = (int64_t *)info;

        if (lseek(fd, (off_t)*pos, SEEK_SET) < 0)
            return ntb0_errno_to_ntstatus(errno);
        break;
    }
    case 5: { /* FileEndOfFileInformation */
        int64_t *eof = (int64_t *)info;

        if (ftruncate(fd, (off_t)*eof) < 0)
            return ntb0_errno_to_ntstatus(errno);
        break;
    }
    case 10: { /* FileRenameInformation */
        uint32_t name_len = *(uint32_t *)(info + 0x0C);
        const char *cur = ntb0_handle_path((uint64_t)a1);
        char newpath[NTB0_PATH_MAX];
        char dirpart[NTB0_PATH_MAX];
        char name8[NTB0_PATH_MAX];
        char *slash;
        int rfd = ntb0_path_dirfd();

        if (name_len == 0 || name_len > 0x7FF0)
            return NTB0_ST_INVALID_PARAMETER;
        if (!cur)
            return NTB0_ST_OBJECT_NAME_NOT_FOUND;
        if (rfd < 0)
            return NTB0_ST_OBJECT_NAME_NOT_FOUND;

        /* الاسم: إن كان مطلقاً (NT) ⇒ ترجمة كاملة، وإلا ⇒ داخل مجلد الملف */
        if (name_len >= 2 && (info[0x10] == '\\' || info[0x10] == '/')) {
            ntb0_unicode_string us;

            memset(&us, 0, sizeof(us));
            us.length = (uint16_t)name_len;
            us.maximum_length = (uint16_t)name_len;
            us.buffer = (uint16_t *)(info + 0x10);

            if (ntb0_path_from_nt(&us, newpath, sizeof(newpath)) !=
                NTB0_ST_SUCCESS)
                return NTB0_ST_OBJECT_PATH_SYNTAX_BAD;
        } else {
            /* تحويل الاسم من UTF-16 إلى UTF-8 */
            if (ntb0_utf16le_to_utf8(info + 0x10, name_len / 2, name8,
                                     sizeof(name8)) < 0)
                return NTB0_ST_OBJECT_PATH_SYNTAX_BAD;

            /* مجلد الملف = كل ما قبل آخر '/' (قد يكون فارغاً ⇒ الجذر) */
            snprintf(dirpart, sizeof(dirpart), "%s", cur);
            slash = strrchr(dirpart, '/');
            if (slash)
                *slash = '\0';
            else
                dirpart[0] = '\0';

            if (dirpart[0]) {
                int n = snprintf(newpath, sizeof(newpath), "%s/%s", dirpart,
                                 name8);

                if (n < 0 || (size_t)n >= sizeof(newpath))
                    return NTB0_ST_OBJECT_PATH_SYNTAX_BAD;
            } else {
                snprintf(newpath, sizeof(newpath), "%s", name8);
            }
        }

        /* المسارات كلها **نسبة لجذر NT** ⇒ renameat على جذر المترجم */
        if (renameat(rfd, cur, rfd, newpath) < 0)
            return ntb0_errno_to_ntstatus(errno);

        /* المقبض يبقى صالحاً ويشير إلى الملف الجديد */
        ntb0_handle_set_path((uint64_t)a1, newpath);
        break;
    }
    case 13: { /* FileDispositionInformation */
        const char *cur = ntb0_handle_path((uint64_t)a1);
        int rfd = ntb0_path_dirfd();

        if (!info[0])
            break; /* DeleteFile = FALSE ⇒ لا عمل */
        if (!cur)
            return NTB0_ST_OBJECT_NAME_NOT_FOUND;
        if (rfd < 0)
            return NTB0_ST_OBJECT_NAME_NOT_FOUND;
        if (unlinkat(rfd, cur, 0) < 0)
            return ntb0_errno_to_ntstatus(errno);
        break;
    }
    default:
        return NTB0_ST_NOT_SUPPORTED;
    }

    if (iosb) {
        iosb->status = 0;
        iosb->information = 0;
    }
    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SYS-014 — NtFlushBuffersFile (T-032)                               */
/* ------------------------------------------------------------------ */

/*
 * NtFlushBuffersFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock)
 *   → fsync(fd) — المزامنة إلى القرص.
 */
static long ntb0_NtFlushBuffersFile(long a1, long a2, long a3, long a4, long a5,
                                    long a6, long a7, long a8, long a9,
                                    long a10, long a11)
{
    int fd;
    ntb0_io_status_block *iosb = (ntb0_io_status_block *)(uintptr_t)a2;

    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9;
    (void)a10; (void)a11;

    fd = ntb0_handle_fd((uint64_t)a1);
    if (fd < 0)
        return NTB0_ST_INVALID_HANDLE;

    if (fsync(fd) < 0)
        return ntb0_errno_to_ntstatus(errno);

    if (iosb) {
        iosb->status = 0;
        iosb->information = 0;
    }
    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SYS-015 — NtDeviceIoControlFile (T-033)                            */
/* ------------------------------------------------------------------ */

/*
 * NtDeviceIoControlFile(
 *   HANDLE FileHandle,             a1
 *   PVOID ApcRoutine,              a2
 *   PVOID ApcContext,              a3
 *   PIO_STATUS_BLOCK IoStatusBlock, a4
 *   PVOID InputBuffer,             a5
 *   ULONG InputBufferLength,       a6
 *   PVOID OutputBuffer,            a7
 *   ULONG OutputBufferLength,      a8
 * PULONG ReturnLength,           a9
 *   ULONG IoControlCode,           a10
 *   PVOID Reserved)                a11
 *
 *   الترجمة: ioctl(fd, code, in, inlen, out, outlen) — **تمرير حقيقي**
 *   ⇒ نفس ioctl الذي تستخدمه linux_bridge (و consequently تعريف GPU).
 */
static long ntb0_NtDeviceIoControlFile(long a1, long a2, long a3, long a4,
                                       long a5, long a6, long a7, long a8,
                                       long a9, long a10, long a11)
{
    int fd;
    void *in = (void *)(uintptr_t)a5;
    uint32_t inlen = (uint32_t)a6;
    void *out = (void *)(uintptr_t)a7;
    uint32_t outlen = (uint32_t)a8;
    uint32_t *ret = (uint32_t *)(uintptr_t)a9;
    uint32_t code = (uint32_t)a10;
    ntb0_io_status_block *iosb = (ntb0_io_status_block *)(uintptr_t)a4;
    int n;

    (void)a2; /* ApcRoutine */
    (void)a3; /* ApcContext */
    (void)a11; /* Reserved */

    fd = ntb0_handle_fd((uint64_t)a1);
    if (fd < 0)
        return NTB0_ST_INVALID_HANDLE;

    /*
     * ioctl في لينكس يأخذ **وسيطاً واحداً** (وليس مدخلاً ومخرجاً منفصلين
     * كما في NtDeviceIoControlFile). لذلك نحدّد الاتجاه من بتّات
     * _IOC (Linux) ونمرّر الوسيط المناسب:
     *   _IOC_WRITE ⇒ النواة تقرأ من الوسيط ⇒ نمرّر InputBuffer
     *   غير ذلك    ⇒ النواة تكتب في الوسيط ⇒ نمرّر OutputBuffer
     *   _IOC_READ|_IOC_WRITE ⇒ ننسخ المدخل داخل مخزن الإخراج ونمرّره.
     * (رموز ioctl تُمرَّر كما هي — وهي رموز لينكس، مثل رموز linux_bridge.)
     */
    {
        unsigned int dir = (unsigned int)((code >> 30) & 3u);
        void *arg = NULL;

        if (dir & 1u) { /* _IOC_WRITE ⇒ النواة تقرأ من الوسيط */
            if (in && inlen > 0) {
                arg = in;
            } else if (out && outlen > 0) {
                arg = out;
            }
        } else {
            if (out && outlen > 0) {
                arg = out;
            } else if (in && inlen > 0) {
                arg = in;
            }
        }

        if (dir == 3u && in && inlen > 0 && out && outlen >= inlen) {
            memcpy(out, in, inlen);
            arg = out;
        }

        n = (int)syscall(SYS_ioctl, fd, (unsigned long)code, arg);
    }
    if (n < 0)
        return ntb0_errno_to_ntstatus(errno);

    if (ret)
        *ret = (uint32_t)n;

    if (iosb) {
        iosb->status = 0;
        iosb->information = (uint32_t)n;
    }
    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SYS-016/017/018 — الأحداث (T-037)                                   */
/* ------------------------------------------------------------------ */

/*
 * NtCreateEvent(
 *   PHANDLE EventHandle,              a1
 *   ACCESS_MASK DesiredAccess,        a2
 *   POBJECT_ATTRIBUTES ObjectAttributes, a3
 *   EVENT_TYPE EventType,             a4
 *   BOOLEAN InitialState)             a5
 *
 *   الترجمة: eventfd() — كائن في جدول المقابض (SYS-007).
 *   EventType:
 *     0 NotificationEvent  → manual reset (eventfd عادي)
 *     1 SynchronizationEvent → auto reset (eventfd بـ EFD_SEMAPHORE)
 */
static long ntb0_NtCreateEvent(long a1, long a2, long a3, long a4, long a5,
                                long a6, long a7, long a8, long a9, long a10,
                                long a11)
{
    uint64_t *out = (uint64_t *)(uintptr_t)a1;
    uint32_t type = (uint32_t)a4;
    int efd, flags = EFD_CLOEXEC | EFD_NONBLOCK;
    uint64_t val = 0;
    long rc;

    (void)a2; /* DesiredAccess */
    (void)a3; /* ObjectAttributes */
    (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!out)
        return NTB0_ST_INVALID_PARAMETER;

    if (type == 1)
        flags |= EFD_SEMAPHORE;

    efd = eventfd(0, flags);
    if (efd < 0)
        return ntb0_errno_to_ntstatus(errno);

    if (a5) { /* InitialState = TRUE */
        val = 1;
        if (write(efd, &val, sizeof(val)) != (ssize_t)sizeof(val)) {
            close(efd);
            return ntb0_errno_to_ntstatus(errno);
        }
    }

    rc = ntb0_handle_create_event(efd, out);
    if (rc != NTB0_ST_SUCCESS)
        return rc; /* ntb0_handle_create_event لا تغلق — نغلق هنا */

    return NTB0_ST_SUCCESS;
}

/* NtSetEvent(HANDLE EventHandle) → كتابة 1 في eventfd */
static long ntb0_NtSetEvent(long a1, long a2, long a3, long a4, long a5,
                             long a6, long a7, long a8, long a9, long a10,
                             long a11)
{
    int fd;
    uint64_t val = 1;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8;
    (void)a9; (void)a10; (void)a11;

    fd = ntb0_handle_fd((uint64_t)a1);
    if (fd < 0)
        return NTB0_ST_INVALID_HANDLE;

    if (ntb0_handle_kind((uint64_t)a1) != NTB0_HK_EVENT)
        return NTB0_ST_INVALID_HANDLE;

    if (write(fd, &val, sizeof(val)) != (ssize_t)sizeof(val))
        return ntb0_errno_to_ntstatus(errno);

    return NTB0_ST_SUCCESS;
}

/* NtClearEvent(HANDLE EventHandle) → تصفير العدّاد */
static long ntb0_NtClearEvent(long a1, long a2, long a3, long a4, long a5,
                               long a6, long a7, long a8, long a9, long a10,
                               long a11)
{
    int fd;
    uint64_t val = 0;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8;
    (void)a9; (void)a10; (void)a11;

    fd = ntb0_handle_fd((uint64_t)a1);
    if (fd < 0)
        return NTB0_ST_INVALID_HANDLE;

    if (ntb0_handle_kind((uint64_t)a1) != NTB0_HK_EVENT)
        return NTB0_ST_INVALID_HANDLE;

    /* EFD_NONBLOCK ⇒ التفريغ لا يُعيق إن كان العدّاد صفراً */
    while (read(fd, &val, sizeof(val)) == (ssize_t)sizeof(val))
        ;

    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SYS-019 — NtWaitForSingleObject (T-038)                            */
/* ------------------------------------------------------------------ */

/*
 * NtWaitForSingleObject(
 *   HANDLE ObjectHandle,   a1
 *   BOOLEAN Alertable,     a2
 *   PLARGE_INTEGER Timeout, a3
 *
 *   Timeout: 100-نانية ثانية.
 *     NULL ⇒ انتظار بلا مهلة.
 *     سالب ⇒ نسبي (شبه futex).
 *     موجب ⇒ نسبي أيضاً (حدّ موثّق: NT يجعله مطلقاً من لحظة الإقلاع —
 *       نستخدمه نسبياً لأن ساعة الإقلاع غير متاحة في هذه المرحلة).
 */
static long ntb0_NtWaitForSingleObject(long a1, long a2, long a3, long a4,
                                        long a5, long a6, long a7, long a8,
                                        long a9, long a10, long a11)
{
    int timeout_ms = -1;
    int64_t *tv = (int64_t *)(uintptr_t)a3;
    int r;

    (void)a2; /* Alertable */
    (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10;
    (void)a11;

    /*
     * الأحداث والخيوط قابلة للانتظار (كلاهما fd: eventfd).
     * في NT مقبض الخيط يُنبَّه عند انتهاء الخيط ⇒ نفس الدلالات ✓
     */
    {
        int k = ntb0_handle_kind((uint64_t)a1);

        if (k != NTB0_HK_EVENT && k != NTB0_HK_THREAD)
            return NTB0_ST_NOT_SUPPORTED;
    }

    if (tv) {
        int64_t t100 = *tv;
        int64_t ms = t100 / 10000; /* 100ns -> ms */

        if (ms < 0)
            ms = -ms;
        if (ms > 0x7FFFFFFF)
            ms = 0x7FFFFFFF;
        timeout_ms = (int)ms;
    }

    r = ntb0_handle_wait((uint64_t)a1, timeout_ms);
    if (r < 0)
        return ntb0_errno_to_ntstatus(errno);
    if (r == 1)
        return NTB0_ST_TIMEOUT;

    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SYS-020/021 — التأخير وتسليم المعالج (T-039)                           */
/* ------------------------------------------------------------------ */

/*
 * NtDelayExecution(BOOLEAN Alertable, PLARGE_INTEGER Interval)
 *   Interval: 100-نانية ثانية؛ سالب = نسبي (الشكل المعتاد من كود ويندوز).
 */
static long ntb0_NtDelayExecution(long a1, long a2, long a3, long a4, long a5,
                                   long a6, long a7, long a8, long a9, long a10,
                                   long a11)
{
    int64_t *tv = (int64_t *)(uintptr_t)a2;
    struct timespec ts;
    int64_t ms;

    (void)a1; /* Alertable */
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9;
    (void)a10; (void)a11;

    if (!tv)
        return NTB0_ST_INVALID_PARAMETER;

    ms = *tv / 10000;
    if (ms < 0)
        ms = -ms;
    if (ms > 0x7FFFFFFF)
        ms = 0x7FFFFFFF;

    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);

    while (nanosleep(&ts, &ts) < 0) {
        if (errno == EINTR)
            continue; /* استيقظ من إشارة ⇒ نكمل remainder */
        return ntb0_errno_to_ntstatus(errno);
    }

    return NTB0_ST_SUCCESS;
}

/* NtYieldExecution() → sched_yield() */
static long ntb0_NtYieldExecution(long a1, long a2, long a3, long a4, long a5,
                                  long a6, long a7, long a8, long a9, long a10,
                                  long a11)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    (void)a8; (void)a9; (void)a10; (void)a11;

    sched_yield();
    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SYS-022 — NtQuerySystemInformation (T-035)                         */
/* ------------------------------------------------------------------ */

/*
 * NtQuerySystemInformation(
 *   SYSTEM_INFORMATION_CLASS SystemInformationClass, a1
 *   PVOID SystemInformation,                          a2
 *   ULONG SystemInformationLength,                   a3
 *   PULONG ReturnLength)                             a4
 *
 *   SYSTEM_BASIC_INFORMATION (صنف 0) — 48 بايت على x64:
 *     0x00 Reserved · 0x04 TimerResolution · 0x08 PageSize
 *     0x0C NumberOfPhysicalPages · 0x10 LowestPhysicalPageNumber
 *     0x14 HighestPhysicalPageNumber · 0x18 AllocationGranularity
 *     0x20 MinimumUserModeAddress · 0x28 MaximumUserModeAddress
 *     0x30 ActiveProcessorsAffinityMask · 0x38 NumberOfProcessors
 *     (الحجم الكلي 0x40 على x64)
 *
 *   SYSTEM_PROCESSOR_INFORMATION (صنف 1) — مصفوفة:
 *     { ULONG ProcessorNumber; ULONG Reserved; ULONG64 UserTime; ULONG64 KernelTime; }
 */
static long ntb0_NtQuerySystemInformation(long a1, long a2, long a3, long a4,
                                          long a5, long a6, long a7, long a8,
                                          long a9, long a10, long a11)
{
    uint32_t cls = (uint32_t)a1;
    unsigned char *out = (unsigned char *)(uintptr_t)a2;
    uint32_t len = (uint32_t)a3;
    uint32_t *ret = (uint32_t *)(uintptr_t)a4;
    long npages, nproc, gran;

    (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!out)
        return NTB0_ST_INVALID_PARAMETER;

    npages = sysconf(_SC_PHYS_PAGES);
    nproc = sysconf(_SC_NPROCESSORS_ONLN);
    gran = sysconf(_SC_PAGESIZE) * 16; /* تخصيص NT = 64KB */

    if (cls == 0) { /* SystemBasicInformation */
        /* البنية 0x40 بايت على x64 (حقل NumberOfProcessors عند 0x38
         * ثم محاذاة إلى 8 ⇒ الحجم الكلي 0x40). */
        if (len < 0x40)
            return NTB0_ST_STATUS_LOGICAL_ERROR; /* BUFFER_TOO_SMALL */
        memset(out, 0, 0x40);
        *(uint32_t *)(out + 0x04) = 156250;  /* TimerResolution (100ns) */
        *(uint32_t *)(out + 0x08) = (uint32_t)sysconf(_SC_PAGESIZE);
        *(uint32_t *)(out + 0x0C) = (uint32_t)(npages > 0 ? npages : 0);
        *(uint32_t *)(out + 0x10) = 0;       /* LowestPhysicalPageNumber */
        *(uint32_t *)(out + 0x14) = (uint32_t)(npages > 0 ? npages - 1 : 0);
        *(uint32_t *)(out + 0x18) = (uint32_t)gran;
        *(uint64_t *)(out + 0x20) = 0x10000ULL;
        *(uint64_t *)(out + 0x28) = 0x00007FFFFFFFFFFFULL;
        *(uint64_t *)(out + 0x30) = (nproc > 0) ? ((uint64_t)1 << (nproc - 1)) : 1;
        *(uint8_t *)(out + 0x38) = (uint8_t)(nproc > 0 ? nproc : 1);
        if (ret)
            *ret = 0x40;
        return NTB0_ST_SUCCESS;
    }

    if (cls == 1) { /* SystemProcessorInformation */
        uint32_t n = (nproc > 0) ? (uint32_t)nproc : 1;
        uint32_t one = 0x20; /* sizeof(SYSTEM_PROCESSOR_INFORMATION) على x64 */
        uint32_t total = n * one;
        uint32_t i;

        if (len < total)
            return NTB0_ST_STATUS_LOGICAL_ERROR;

        memset(out, 0, total);
        for (i = 0; i < n; i++) {
            unsigned char *p = out + (size_t)i * one;

            *(uint32_t *)(p + 0x00) = i; /* ProcessorNumber */
            *(uint32_t *)(p + 0x04) = 0; /* Reserved */
            /* أوقات المعالج تُقرأ من /proc/stat في مرحلة لاحقة (T-035b) */
        }
        if (ret)
            *ret = total;
        return NTB0_ST_SUCCESS;
    }

    if (cls == 3) { /* SystemTimeOfDayInformation (T-036) */
        /* SYSTEM_TIMEOFDAY_INFORMATION:
         *   0x00 BootTime · 0x08 CurrentTime · 0x10 TimeZoneBias */
        if (len < 0x18)
            return NTB0_ST_STATUS_LOGICAL_ERROR; /* BUFFER_TOO_SMALL */

        memset(out, 0, 0x18);
        *(int64_t *)(out + 0x00) = ntb0_boot_time_100ns();
        *(int64_t *)(out + 0x08) = ntb0_time_now_100ns();
        *(int64_t *)(out + 0x10) = 0; /* TimeZoneBias (UTC) */
        if (ret)
            *ret = 0x18;
        return NTB0_ST_SUCCESS;
    }

    return NTB0_ST_NOT_SUPPORTED;
}

/* ------------------------------------------------------------------ */
/* SYS-023 — NtQueryDirectoryFile (T-054)                             */
/* ------------------------------------------------------------------ */

/*
 * NtQueryDirectoryFile(
 *   HANDLE FileHandle,              a1
 *   PVOID Event,                    a2
 *   PVOID ApcRoutine,               a3
 *   PVOID ApcContext,               a4
 *   PIO_STATUS_BLOCK IoStatusBlock, a5
 *   PVOID FileInformation,          a6
 *   ULONG FileInformationClass,     a7
 *   ULONG Length,                   a8
 *   PLARGE_INTEGER ApcRoutineArgument, a9
 *   PULONG ReturnLength)            a10
 *
 *   الصنف 1 = FileDirectoryInformation:
 *     0x00 NextEntryOffset · 0x04 FileIndex
 *     0x08 CreationTime · 0x10 LastAccessTime · 0x18 LastWriteTime
 *     0x20 ChangeTime · 0x28 EndOfFile · 0x30 AllocationSize
 *     0x38 FileAttributes · 0x3C FileNameLength · 0x40 FileName[]
 *
 *   استُعمل readdir() على تدفّق محفوظ للمقبض ⇒ التكرار يستمر
 *   بين الاستدعاءات (حتى STATUS_NO_MORE_FILES).
 */
static long ntb0_NtQueryDirectoryFile(long a1, long a2, long a3, long a4,
                                      long a5, long a6, long a7, long a8,
                                      long a9, long a10, long a11)
{
    uint64_t handle = (uint64_t)a1;
    unsigned char *out = (unsigned char *)(uintptr_t)a6;
    uint32_t cls = (uint32_t)a7;
    uint32_t len = (uint32_t)a8;
    uint32_t *ret = (uint32_t *)(uintptr_t)a10;
    ntb0_io_status_block *iosb = (ntb0_io_status_block *)(uintptr_t)a5;
    DIR *d;
    struct dirent *de;
    struct stat st;
    uint16_t name16[NTB0_PATH_MAX];
    size_t nchars = 0, i, nbytes;

    (void)a2; /* Event */
    (void)a3; /* ApcRoutine */
    (void)a4; /* ApcContext */
    (void)a9; /* ApcRoutineArgument */
    (void)a11;

    if (!out)
        return NTB0_ST_INVALID_PARAMETER;

    if (cls != 1) /* FileDirectoryInformation */
        return NTB0_ST_NOT_SUPPORTED;

    d = (DIR *)ntb0_handle_dir_stream(handle);
    if (!d)
        return NTB0_ST_INVALID_HANDLE;

    errno = 0;
    de = readdir(d);
    if (!de) {
        if (errno)
            return ntb0_errno_to_ntstatus(errno);
        if (iosb) {
            iosb->status = (int32_t)NTB0_ST_NO_MORE_FILES;
            iosb->information = 0;
        }
        return NTB0_ST_NO_MORE_FILES;
    }

    /* الاسم: UTF-8 من النظام ⇒ UTF-16LE كما يتوقع NT */
    {
        size_t sl = strlen(de->d_name);

        if (sl >= sizeof(name16) / sizeof(name16[0]))
            sl = sizeof(name16) / sizeof(name16[0]) - 1;
        for (i = 0; i < sl; i++) {
            unsigned char c = (unsigned char)de->d_name[i];

            if (c < 0x80) {
                name16[nchars++] = (uint16_t)c;
            } else if ((c & 0xE0) == 0xC0 && i + 1 < sl) {
                uint16_t cp = (uint16_t)(((c & 0x1F) << 6) |
                                         (de->d_name[i + 1] & 0x3F));
                name16[nchars++] = cp;
                i++;
            } else {
                name16[nchars++] = 0xFFFD; /* بديل */
            }
        }
    }

    nbytes = 0x40 + nchars * 2;
    if (len < nbytes)
        return NTB0_ST_STATUS_LOGICAL_ERROR; /* BUFFER_TOO_SMALL */

    memset(out, 0, 0x40);
    memset(out + 0x40, 0, nchars * 2);
    for (i = 0; i < nchars; i++)
        *(uint16_t *)(out + 0x40 + i * 2) = name16[i];

    if (fstatat(dirfd(d), de->d_name, &st, 0) == 0) {
        *(uint32_t *)(out + 0x38) = 0x80; /* FILE_ATTRIBUTE_NORMAL */
        if (S_ISDIR(st.st_mode))
            *(uint32_t *)(out + 0x38) = 0x10; /* FILE_ATTRIBUTE_DIRECTORY */
        *(int64_t *)(out + 0x08) = (int64_t)st.st_ctim.tv_sec * 10000000LL;
        *(int64_t *)(out + 0x10) = (int64_t)st.st_atim.tv_sec * 10000000LL;
        *(int64_t *)(out + 0x18) = (int64_t)st.st_mtim.tv_sec * 10000000LL;
        *(int64_t *)(out + 0x28) = (int64_t)st.st_size;
        *(int64_t *)(out + 0x30) = (int64_t)st.st_blocks * 512;
    } else {
        *(uint32_t *)(out + 0x38) = 0x80;
    }

    *(uint32_t *)(out + 0x00) = 0;      /* NextEntryOffset */
    *(uint32_t *)(out + 0x04) = 0;      /* FileIndex */
    *(uint32_t *)(out + 0x3C) = (uint32_t)(nchars * 2); /* FileNameLength */

    if (ret)
        *ret = (uint32_t)nbytes;

    if (iosb) {
        iosb->status = 0;
        iosb->information = (uint32_t)nbytes;
    }
    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SYS-024/025/026 — المقاطع (T-034)                                  */
/* ------------------------------------------------------------------ */

/*
 * جدول العروض (mapped views) — نحتاج الحجم لتفريغ العرض لاحقاً.
 */
#define NTB0_MAX_VIEWS 64
struct ntb0_view {
    uintptr_t base;
    size_t    len;
};
static struct ntb0_view ntb0_views[NTB0_MAX_VIEWS];

static int ntb0_view_add(uintptr_t base, size_t len)
{
    int i;

    for (i = 0; i < NTB0_MAX_VIEWS; i++) {
        if (!ntb0_views[i].len) {
            ntb0_views[i].base = base;
            ntb0_views[i].len = len;
            return 0;
        }
    }
    return -1;
}

static size_t ntb0_view_take(uintptr_t base)
{
    int i;

    for (i = 0; i < NTB0_MAX_VIEWS; i++) {
        if (ntb0_views[i].len && ntb0_views[i].base == base) {
            size_t len = ntb0_views[i].len;

            ntb0_views[i].base = 0;
            ntb0_views[i].len = 0;
            return len;
        }
    }
    return 0;
}

/*
 * NtCreateSection(
 *   PHANDLE SectionHandle,            a1
 *   ACCESS_MASK DesiredAccess,        a2
 *   POBJECT_ATTRIBUTES ObjectAttributes, a3
 *   PLARGE_INTEGER SectionSize,       a4
 *   ULONG AllocationType,             a5
 *   HANDLE FileHandle)                a6
 *
 *   - FileHandle ≠ 0  ⇒ مقطع ملفّي: نأخذ **نسخة من الـ fd** (dup) ⇒
 *     `NtMapViewOfSection` later يعمل بـ mmap على الملف مباشرة (بلا نسخ).
 *   - FileHandle = 0  ⇒ مقطع مجهول: memfd بحجم SectionSize.
 */
static long ntb0_NtCreateSection(long a1, long a2, long a3, long a4, long a5,
                                 long a6, long a7, long a8, long a9, long a10,
                                 long a11)
{
    uint64_t *out = (uint64_t *)(uintptr_t)a1;
    int64_t *size_ptr = (int64_t *)(uintptr_t)a4;
    uint64_t file_handle = (uint64_t)a6;
    int fd = -1;
    uint64_t size = 0;
    long rc;

    (void)a2; /* DesiredAccess */
    (void)a3; /* ObjectAttributes */
    (void)a5; /* AllocationType */
    (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!out)
        return NTB0_ST_INVALID_PARAMETER;

    if (file_handle) {
        int sfd = ntb0_handle_fd(file_handle);
        struct stat st;

        if (sfd < 0)
            return NTB0_ST_INVALID_HANDLE;

        if (fstat(sfd, &st) < 0)
            return ntb0_errno_to_ntstatus(errno);

        /* مقطع ملفّي: نسخة من الـ fd (بلا نسخ بيانات) */
        fd = dup(sfd);
        if (fd < 0)
            return ntb0_errno_to_ntstatus(errno);

        size = size_ptr && *size_ptr > 0 ? (uint64_t)*size_ptr
                                        : (uint64_t)st.st_size;
    } else {
        size = (size_ptr && *size_ptr > 0) ? (uint64_t)*size_ptr : 0x10000;

        fd = memfd_create("ntbox_section", 0);
        if (fd < 0)
            return ntb0_errno_to_ntstatus(errno);
        if (ftruncate(fd, (off_t)size) < 0) {
            close(fd);
            return ntb0_errno_to_ntstatus(errno);
        }
    }

    rc = ntb0_handle_create_section(fd, size, out);
    if (rc != NTB0_ST_SUCCESS)
        close(fd); /* ntb0_handle_create_section لا تغلق */
    return rc;
}

/*
 * NtMapViewOfSection(
 *   HANDLE SectionHandle,            a1
 *   HANDLE ProcessHandle,            a2
 *   PVOID *BaseAddress,              a3
 *   PLARGE_INTEGER ViewSize,         a4
 *   ULONG AllocationType,            a5
 *   ULONG AllocationProtect,         a6
 *   PLARGE_INTEGER SectionOffset,    a7
 *   PULONG_PTR ViewSizeOut,          a8
 *   HANDLE SectionHandle2)           a9
 */
static long ntb0_NtMapViewOfSection(long a1, long a2, long a3, long a4, long a5,
                                    long a6, long a7, long a8, long a9, long a10,
                                    long a11)
{
    uint64_t handle = (uint64_t)a1;
    uint64_t *base_ptr = (uint64_t *)(uintptr_t)a3;
    int64_t *view_ptr = (int64_t *)(uintptr_t)a4;
    uint32_t protect = (uint32_t)a6;
    int64_t *off_ptr = (int64_t *)(uintptr_t)a7;
    int64_t *size_out = (int64_t *)(uintptr_t)a8;
    int fd;
    uint64_t sec_size;
    off_t offset = 0;
    size_t len = 0;
    void *p;
    int prot = PROT_READ;
    int flags = MAP_SHARED;

    (void)a2;  /* ProcessHandle */
    (void)a5;  /* AllocationType */
    (void)a9;  /* SectionHandle2 */
    (void)a10; (void)a11;

    if (ntb0_handle_kind(handle) != NTB0_HK_SECTION)
        return NTB0_ST_INVALID_HANDLE;

    fd = ntb0_handle_fd(handle);
    if (fd < 0)
        return NTB0_ST_INVALID_HANDLE;

    sec_size = ntb0_handle_size(handle);
    if (off_ptr && *off_ptr > 0)
        offset = (off_t)*off_ptr;
    if (offset % 4096)
        return NTB0_ST_INVALID_PARAMETER;

    if (view_ptr && *view_ptr > 0)
        len = (size_t)*view_ptr;
    else
        len = (size_t)(sec_size > (uint64_t)offset ? sec_size - (uint64_t)offset
                                                   : 0x1000);

    /* حماية: PAGE_READONLY(2) · PAGE_READWRITE(4) · PAGE_EXECUTE(10) */
    if (protect & 0x04)
        prot = PROT_READ | PROT_WRITE;
    else if (protect & 0x20)
        prot = PROT_READ | PROT_EXEC;
    else
        prot = PROT_READ;

    if (base_ptr && *base_ptr)
        flags |= MAP_FIXED;

    p = mmap(NULL, len, prot, flags, fd, offset);
    if (p == MAP_FAILED)
        return ntb0_errno_to_ntstatus(errno);

    ntb0_view_add((uintptr_t)p, len);

    if (base_ptr)
        *base_ptr = (uint64_t)(uintptr_t)p;
    if (size_out)
        *size_out = (int64_t)len;

    return NTB0_ST_SUCCESS;
}

/*
 * NtUnmapViewOfSection(PVOID BaseAddress)
 */
static long ntb0_NtUnmapViewOfSection(long a1, long a2, long a3, long a4, long a5,
                                      long a6, long a7, long a8, long a9, long a10,
                                      long a11)
{
    uint64_t base = (uint64_t)a1;
    size_t len;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8;
    (void)a9; (void)a10; (void)a11;

    len = ntb0_view_take((uintptr_t)base);
    if (!len)
        return NTB0_ST_INVALID_PARAMETER;

    if (munmap((void *)(uintptr_t)base, len) < 0)
        return ntb0_errno_to_ntstatus(errno);

    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SYS-027 — NtFsControlFile (T-052)                                  */
/* ------------------------------------------------------------------ */

/*
 * NtFsControlFile — نفس شكل NtDeviceIoControlFile (تمرير ioctl حقيقي).
 *   a1=FileHandle a2=ApcRoutine a3=ApcContext a4=IoStatusBlock*
 *   a5=InputBuffer a6=InputBufferLength a7=OutputBuffer
 *   a8=OutputBufferLength a9=ReturnLength* a10=IoControlCode a11=Reserved
 */
static long ntb0_NtFsControlFile(long a1, long a2, long a3, long a4, long a5,
                                 long a6, long a7, long a8, long a9, long a10,
                                 long a11)
{
    return ntb0_NtDeviceIoControlFile(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10,
                                      a11);
}

/* ------------------------------------------------------------------ */
/* SYS-028/029 — أقفال الملفات (T-053)                               */
/* ------------------------------------------------------------------ */

/*
 * NtLockFile(
 *   HANDLE FileHandle,        a1
 *   LARGE_INTEGER *Offset,    a2
 *   LARGE_INTEGER *Length,    a3
 *   ULONG LockOperation,      a4
 *   ULONG ProcessHandle,      a5
 *   PIO_STATUS_BLOCK,         a6
 *   PVOID Key,                a7
 *   LARGE_INTEGER *LockOffset,a8
 *   LARGE_INTEGER *LockLength,a9
 *   ULONG *LockType)          a10
 *
 *   LockOperation: 1 = FAIL_IMMEDIATELY (F_SETLK) · 2 = EXCLUSIVE (F_WRLCK)
 */
static long ntb0_NtLockFile(long a1, long a2, long a3, long a4, long a5, long a6,
                            long a7, long a8, long a9, long a10, long a11)
{
    int fd;
    int64_t *off_ptr = (int64_t *)(uintptr_t)a2;
    int64_t *len_ptr = (int64_t *)(uintptr_t)a3;
    uint32_t op = (uint32_t)a4;
    struct flock fl;
    int cmd;

    (void)a5; /* ProcessHandle */
    (void)a6; /* IoStatusBlock */
    (void)a7; /* Key */
    (void)a8; (void)a9; (void)a10; (void)a11;

    if (!off_ptr || !len_ptr)
        return NTB0_ST_INVALID_PARAMETER;

    fd = ntb0_handle_fd((uint64_t)a1);
    if (fd < 0)
        return NTB0_ST_INVALID_HANDLE;

    memset(&fl, 0, sizeof(fl));
    fl.l_type = (op & 0x2u) ? F_WRLCK : F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = (off_t)*off_ptr;
    fl.l_len = (off_t)*len_ptr;

    cmd = (op & 0x1u) ? F_SETLK : F_SETLKW;

    while (fcntl(fd, cmd, &fl) < 0) {
        if (errno == EINTR)
            continue;
        return ntb0_errno_to_ntstatus(errno);
    }

    return NTB0_ST_SUCCESS;
}

/* NtUnlockFile — نفس الشكل ⇒ F_UNLCK */
static long ntb0_NtUnlockFile(long a1, long a2, long a3, long a4, long a5,
                              long a6, long a7, long a8, long a9, long a10,
                              long a11)
{
    int fd;
    int64_t *off_ptr = (int64_t *)(uintptr_t)a2;
    int64_t *len_ptr = (int64_t *)(uintptr_t)a3;
    struct flock fl;

    (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10;
    (void)a11;

    if (!off_ptr || !len_ptr)
        return NTB0_ST_INVALID_PARAMETER;

    fd = ntb0_handle_fd((uint64_t)a1);
    if (fd < 0)
        return NTB0_ST_INVALID_HANDLE;

    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = (off_t)*off_ptr;
    fl.l_len = (off_t)*len_ptr;

    while (fcntl(fd, F_SETLK, &fl) < 0) {
        if (errno == EINTR)
            continue;
        return ntb0_errno_to_ntstatus(errno);
    }

    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SYS-030 — NtQueryAttributesFile (T-055)                            */
/* ------------------------------------------------------------------ */

/*
 * NtQueryAttributesFile(
 *   HANDLE FileHandle,                     a1
 *   PIO_STATUS_BLOCK IoStatusBlock,         a2
 *   PFILE_ATTRIBUTE_TAG_INFORMATION Info,   a3
 *   ULONG Length)                          a4
 *
 *   FILE_ATTRIBUTE_TAG_INFORMATION: 0x00 ULONG FileAttributes · 0x04 ULONG ReparseTag
 */
static long ntb0_NtQueryAttributesFile(long a1, long a2, long a3, long a4,
                                       long a5, long a6, long a7, long a8, long a9,
                                       long a10, long a11)
{
    int fd;
    struct stat st;
    uint32_t attrs = 0;

    (void)a2; /* IoStatusBlock */
    (void)a4; /* Length */
    (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!a3)
        return NTB0_ST_INVALID_PARAMETER;

    fd = ntb0_handle_fd((uint64_t)a1);
    if (fd < 0)
        return NTB0_ST_INVALID_HANDLE;

    if (fstat(fd, &st) < 0)
        return ntb0_errno_to_ntstatus(errno);

    if (S_ISDIR(st.st_mode))
        attrs |= 0x10u;                     /* FILE_ATTRIBUTE_DIRECTORY */
    else if (access("/proc/self/fd/0", R_OK) == 0 && (st.st_mode & S_IWUSR) == 0)
        attrs |= 0x01u;                     /* FILE_ATTRIBUTE_READONLY */
    else
        attrs |= 0x80u;                     /* FILE_ATTRIBUTE_NORMAL */

    *(uint32_t *)(uintptr_t)a3 = attrs;
    *(uint32_t *)((uintptr_t)a3 + 4) = 0;  /* ReparseTag */

    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SYS-031 — NtQueryObject (T-047)                                    */
/* ------------------------------------------------------------------ */

/*
 * NtQueryObject(
 *   HANDLE Handle,                          a1
 *   OBJECT_INFORMATION_CLASS Class,         a2
 *   PVOID ObjectInformation,                a3
 *   ULONG Length,                           a4
 *   PULONG ReturnLength)                    a5
 *
 *   الأصناف المدعومة:
 *     2 ObjectTypeInformation ⇒ OBJECT_TYPE_INFORMATION
 *     5 ObjectNameInformation ⇒ OBJECT_NAME_INFORMATION
 */
static long ntb0_NtQueryObject(long a1, long a2, long a3, long a4, long a5,
                               long a6, long a7, long a8, long a9, long a10,
                               long a11)
{
    uint64_t handle = (uint64_t)a1;
    uint32_t cls = (uint32_t)a2;
    unsigned char *out = (unsigned char *)(uintptr_t)a3;
    uint32_t len = (uint32_t)a4;
    uint32_t *ret = (uint32_t *)(uintptr_t)a5;
    int kind = ntb0_handle_kind(handle);

    (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!out)
        return NTB0_ST_INVALID_PARAMETER;
    if (kind < 0)
        return NTB0_ST_INVALID_HANDLE;

    if (cls == 2) { /* ObjectTypeInformation */
        /* OBJECT_TYPE_INFORMATION: 0x00 UNICODE_STRING TypeName + ... */
        const char *tn = (kind == NTB0_HK_EVENT) ? "Event"
                                                  : (kind == NTB0_HK_SECTION)
                                                        ? "Section"
                                                        : "File";
        size_t nchars = strlen(tn);
        uint16_t *buf = (uint16_t *)malloc((nchars + 1) * 2);

        if (!buf)
            return NTB0_ST_NO_MEMORY;
        for (size_t i = 0; i < nchars; i++)
            buf[i] = (uint16_t)tn[i];

        if (len < 0x10) {
            free(buf);
            return NTB0_ST_STATUS_LOGICAL_ERROR; /* BUFFER_TOO_SMALL */
        }

        memset(out, 0, 0x10);
        *(uint16_t *)(out + 0x00) = (uint16_t)(nchars * 2);
        *(uint16_t *)(out + 0x02) = (uint16_t)(nchars * 2);
        *(uint64_t *)(out + 0x08) = (uint64_t)(uintptr_t)buf;

        if (ret)
            *ret = 0x10;
        return NTB0_ST_SUCCESS;
    }

    if (cls == 5) { /* ObjectNameInformation */
        const char *nm = ntb0_handle_path(handle);
        size_t nchars = 0;
        uint16_t *buf = NULL;

        if (nm) {
            size_t n = strlen(nm);

            buf = (uint16_t *)malloc((n + 1) * 2);
            if (!buf)
                return NTB0_ST_NO_MEMORY;
            for (size_t i = 0; i < n; i++)
                buf[i] = (uint16_t)nm[i];
            nchars = n;
        }

        if (len < 0x10) {
            free(buf);
            return NTB0_ST_STATUS_LOGICAL_ERROR;
        }

        memset(out, 0, 0x10);
        *(uint16_t *)(out + 0x00) = (uint16_t)(nchars * 2);
        *(uint16_t *)(out + 0x02) = (uint16_t)(nchars * 2);
        *(uint64_t *)(out + 0x08) = (uint64_t)(uintptr_t)buf;

        if (ret)
            *ret = 0x10;
        return NTB0_ST_SUCCESS;
    }

    return NTB0_ST_NOT_SUPPORTED;
}

/* ------------------------------------------------------------------ */
/* SYS-032/033/034 — الخيوط (T-040)                                  */
/* ------------------------------------------------------------------ */

/*
 * NtCreateThread(
 *   PHANDLE ThreadHandle,            a1
 *   ACCESS_MASK DesiredAccess,       a2
 *   POBJECT_ATTRIBUTES ObjectAttributes, a3
 *   PCLIENT_ID ClientId,             a4
 *   PTHREAD_START_ROUTINE StartRoutine, a5
 *   PVOID Parameter,                 a6
 *   ULONG Flags,                     a7
 *   PVOID ThreadStackSize)           a8
 */
static long ntb0_NtCreateThread(long a1, long a2, long a3, long a4, long a5,
                                long a6, long a7, long a8, long a9, long a10,
                                long a11)
{
    uint64_t *out = (uint64_t *)(uintptr_t)a1;
    void *client_id = (void *)(uintptr_t)a4;
    uint64_t start = (uint64_t)a5;
    uint64_t param = (uint64_t)a6;
    uint32_t flags = (uint32_t)a7;
    uint64_t stack = 0;
    long rc;

    (void)a2; /* DesiredAccess */
    (void)a3; /* ObjectAttributes */
    (void)a9; (void)a10; (void)a11;

    if (a8)
        stack = *(uint64_t *)(uintptr_t)a8;

    if (!out)
        return NTB0_ST_INVALID_PARAMETER;

    rc = ntb0_thread_create(start, param, flags, stack, out);
    if (rc != NTB0_ST_SUCCESS)
        return rc;

    /* CLIENT_ID: معرّف العملية (pid الحقيقي) + معرّف الخيط */
    if (client_id) {
        uint32_t *cid = (uint32_t *)client_id;

        cid[0] = (uint32_t)getpid();
        cid[1] = ntb0_thread_tid(*out);
    }

    return NTB0_ST_SUCCESS;
}

/*
 * NtResumeThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount)
 */
static long ntb0_NtResumeThread(long a1, long a2, long a3, long a4, long a5,
                                long a6, long a7, long a8, long a9, long a10,
                                long a11)
{
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9;
    (void)a10; (void)a11;

    return ntb0_thread_resume((uint64_t)a1, (uint32_t *)(uintptr_t)a2);
}

/*
 * NtTerminateThread(HANDLE ThreadHandle, NTSTATUS ExitStatus)
 */
static long ntb0_NtTerminateThread(long a1, long a2, long a3, long a4, long a5,
                                   long a6, long a7, long a8, long a9, long a10,
                                   long a11)
{
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9;
    (void)a10; (void)a11;

    return ntb0_thread_terminate((uint64_t)a1, (uint32_t)a2);
}

/* ------------------------------------------------------------------ */
/* SYS-035/036/037 — العمليات (T-041 · T-046)                        */
/* ------------------------------------------------------------------ */

/* PEB اصطناعي: العنوان فقط (المحتوى صفري) — الحدّ الموثّق */
static unsigned char ntb0_fake_peb[0x100];

/*
 * NtOpenProcess(
 *   PHANDLE ProcessHandle,            a1
 *   ACCESS_MASK DesiredAccess,         a2
 *   POBJECT_ATTRIBUTES ObjectAttributes, a3
 *   PCLIENT_ID ClientId)               a4
 */
static long ntb0_NtOpenProcess(long a1, long a2, long a3, long a4, long a5,
                               long a6, long a7, long a8, long a9, long a10,
                               long a11)
{
    uint64_t *out = (uint64_t *)(uintptr_t)a1;
    uint32_t *cid = (uint32_t *)(uintptr_t)a4;
    uint32_t pid;
    char path[64];
    int fd;

    (void)a2; /* DesiredAccess */
    (void)a3; /* ObjectAttributes */
    (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!out || !cid)
        return NTB0_ST_INVALID_PARAMETER;

    pid = cid[0];
    if (pid == 0)
        pid = (uint32_t)getpid();

    snprintf(path, sizeof(path), "/proc/%u", pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return (errno == ENOENT) ? NTB0_ST_INVALID_PARAMETER
                                 : ntb0_errno_to_ntstatus(errno);

    {
        long rc = ntb0_handle_create_process(fd, (void *)(uintptr_t)pid, out);

        if (rc != NTB0_ST_SUCCESS)
            close(fd); /* ntb0_handle_create_process لا تغلق */
        else
            NTB0_OK("SYS-035", "NtOpenProcess(pid=%u) -> handle 0x%llx", pid,
                    (unsigned long long)*out);
        return rc;
    }
}

/*
 * NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus)
 *
 * حماية: لا نقتل العملية الحالية أبداً (المترجم نفسه!).
 */
static long ntb0_NtTerminateProcess(long a1, long a2, long a3, long a4, long a5,
                                    long a6, long a7, long a8, long a9, long a10,
                                    long a11)
{
    uint64_t handle = (uint64_t)a1;
    void *priv;
    uint32_t pid;

    (void)a2; /* ExitStatus */
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9;
    (void)a10; (void)a11;

    if (ntb0_handle_kind(handle) != NTB0_HK_PROCESS)
        return NTB0_ST_INVALID_HANDLE;

    priv = ntb0_handle_priv(handle);
    if (!priv)
        return NTB0_ST_INVALID_HANDLE;

    pid = (uint32_t)(uintptr_t)priv;

    if (pid == (uint32_t)getpid()) {
        /*
         * رفض صحيح: المترجم نفسه لا يقتل نفسه أبداً.
         * ⇒ مصباح OK (سلوك مقصود) لا FAIL.
         */
        NTB0_OK("SYS-036", "NtTerminateProcess(pid=%u) -> ACCESS_DENIED "
                "(refusing to kill the translator itself)", pid);
        return NTB0_ST_ACCESS_DENIED;
    }

    if (kill((pid_t)pid, SIGKILL) < 0)
        return ntb0_errno_to_ntstatus(errno);

    NTB0_OK("SYS-036", "NtTerminateProcess(pid=%u) -> SIGKILL", pid);
    return NTB0_ST_SUCCESS;
}

/*
 * NtQueryInformationProcess(
 *   HANDLE ProcessHandle,                    a1
 *   PROCESS_INFORMATION_CLASS Class,         a2
 *   PVOID ProcessInformation,                a3
 *   ULONG ProcessInformationLength,          a4
 *   PULONG ReturnLength)                     a5
 *
 * الأصناف: 0 Basic · 7 DebugPort · 27 ImageFileName · 40 Wow64Information
 */
static long ntb0_NtQueryInformationProcess(long a1, long a2, long a3, long a4,
                                            long a5, long a6, long a7, long a8,
                                            long a9, long a10, long a11)
{
    uint64_t handle = (uint64_t)a1;
    uint32_t cls = (uint32_t)a2;
    unsigned char *out = (unsigned char *)(uintptr_t)a3;
    uint32_t len = (uint32_t)a4;
    uint32_t *ret = (uint32_t *)(uintptr_t)a5;
    uint32_t pid = 0;
    void *priv;

    (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    /*
     * مقبضويندوز الخاص: (HANDLE)-1 = NtCurrentProcess، (HANDLE)-2 = NtCurrentThread
     * — يجب قبولهما هنا قبل فحص جدول المقابض، وإلا سترفض معالجنا
     * استدعاءات CRT الحقيقية (مثل RtlEncodePointer المرور بـ -1) وترفع الاستثناءات
     * STATUS_INVALID_HANDLE داخل ntdll الحقيقي.
     */
    if (!out)
        return NTB0_ST_INVALID_PARAMETER;

    if (handle == 0 || handle == ~0ULL ||
        handle == (uint64_t)(int64_t)-2) {
        pid = (uint32_t)getpid();
    } else if (ntb0_handle_kind(handle) == NTB0_HK_PROCESS) {
        priv = ntb0_handle_priv(handle);
        pid = priv ? (uint32_t)(uintptr_t)priv : (uint32_t)getpid();
    } else {
        return NTB0_ST_INVALID_HANDLE;
    }

    switch (cls) {
    case 0: { /* ProcessBasicInformation */
        /*
         * 0x00 NTSTATUS ExitStatus
         * 0x08 PPEB PebBaseAddress
         * 0x10 ULONG_PTR AffinityMask
         * 0x18 ULONG_PTR BasePriority
         * 0x20 ULONG_PTR UniqueProcessId
         * 0x28 ULONG_PTR InheritedFromUniqueProcessId
         */
        cpu_set_t set;

        if (len < 0x30)
            return NTB0_ST_STATUS_LOGICAL_ERROR; /* BUFFER_TOO_SMALL */

        memset(out, 0, 0x30);
        *(int32_t *)(out + 0x00) = 0;      /* ExitStatus */
        *(uint64_t *)(out + 0x08) = (uint64_t)(uintptr_t)ntb0_fake_peb;

        CPU_ZERO(&set);
        if (sched_getaffinity(0, sizeof(set), &set) == 0)
            *(uint64_t *)(out + 0x10) = *(uint64_t *)&set;
        else
            *(uint64_t *)(out + 0x10) = 1;

        *(int32_t *)(out + 0x18) = getpriority(PRIO_PROCESS, 0);
        *(uint64_t *)(out + 0x20) = (uint64_t)pid;
        *(uint64_t *)(out + 0x28) = (uint64_t)getppid();

        if (ret)
            *ret = 0x30;
        return NTB0_ST_SUCCESS;
    }

    case 0x16: { /* ProcessDebugObjectHandle */
        if (len < sizeof(uint64_t))
            return NTB0_ST_STATUS_LOGICAL_ERROR;
        *(uint64_t *)out = 0;
        if (ret)
            *ret = sizeof(uint64_t);
        return NTB0_ST_SUCCESS;
    }

    case 0x24: { /* ProcessCookie — ntdll!RtlEncodePointer calls this at CRT
                  * startup; an error here makes real ntdll raise
                  * STATUS_INVALID_HANDLE via RtlRaiseStatus, which recurses
                  * unconditionally on any trap return until stack overflow
                  * (exit 139 in GUI test apps). */
        static uint32_t cookie;
        static int cookie_inited;

        if (!cookie_inited) {
            cookie = (uint32_t)(uint64_t)getpid() ^
                     (uint32_t)(uintptr_t)&cookie;
            cookie_inited = 1;
        }
        if (len < sizeof(uint32_t))
            return NTB0_ST_STATUS_LOGICAL_ERROR;
        *(uint32_t *)out = cookie;
        if (ret)
            *ret = sizeof(uint32_t);
        return NTB0_ST_SUCCESS;
    }

    case 27: { /* ProcessImageFileName ⇒ UNICODE_STRING */
        char link[64];
        char exe[4096];
        ssize_t n;
        size_t i, chars;

        snprintf(link, sizeof(link), "/proc/%u/exe", pid);
        n = readlink(link, exe, sizeof(exe) - 1);
        if (n < 0)
            return ntb0_errno_to_ntstatus(errno);
        exe[n] = '\0';

        chars = strlen(exe);
        if (len < 0x10)
            return NTB0_ST_STATUS_LOGICAL_ERROR;

        {
            uint16_t *buf = (uint16_t *)malloc((chars + 1) * 2);

            if (!buf)
                return NTB0_ST_NO_MEMORY;
            for (i = 0; i < chars; i++)
                buf[i] = (uint16_t)(unsigned char)exe[i];

            memset(out, 0, 0x10);
            *(uint16_t *)(out + 0x00) = (uint16_t)(chars * 2);
            *(uint16_t *)(out + 0x02) = (uint16_t)(chars * 2);
            *(uint64_t *)(out + 0x08) = (uint64_t)(uintptr_t)buf;
        }

        if (ret)
            *ret = 0x10;
        return NTB0_ST_SUCCESS;
    }

    case 40: { /* ProcessWow64Information */
        if (len < 4)
            return NTB0_ST_STATUS_LOGICAL_ERROR;
        *(uint32_t *)out = 0; /* ليس WoW64 — بلاء 64-بت */
        if (ret)
            *ret = 4;
        return NTB0_ST_SUCCESS;
    }

    default:
        return NTB0_ST_NOT_SUPPORTED;
    }
}

/* ------------------------------------------------------------------ */
/* SYS-038/039 — فتح الخيط وتعليقه (T-042)                           */
/* ------------------------------------------------------------------ */

/*
 * NtOpenThread(
 *   PHANDLE ThreadHandle,            a1
 *   ACCESS_MASK DesiredAccess,       a2
 *   POBJECT_ATTRIBUTES ObjectAttributes, a3
 *   PCLIENT_ID ClientId)             a4
 */
static long ntb0_NtOpenThread(long a1, long a2, long a3, long a4, long a5,
                              long a6, long a7, long a8, long a9, long a10,
                              long a11)
{
    uint64_t *out = (uint64_t *)(uintptr_t)a1;
    uint32_t *cid = (uint32_t *)(uintptr_t)a4;

    (void)a2; (void)a3; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9;
    (void)a10; (void)a11;

    if (!out || !cid)
        return NTB0_ST_INVALID_PARAMETER;

    return ntb0_thread_open(cid[1], out);
}

/*
 * NtSuspendThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount)
 */
static long ntb0_NtSuspendThread(long a1, long a2, long a3, long a4, long a5,
                                 long a6, long a7, long a8, long a9, long a10,
                                 long a11)
{
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9;
    (void)a10; (void)a11;

    return ntb0_thread_suspend((uint64_t)a1, (uint32_t *)(uintptr_t)a2);
}


/*
 * NtQueryInformationThread(
 *   HANDLE ThreadHandle,                a1
 *   THREAD_INFORMATION_CLASS Class,     a2
 *   PVOID ThreadInformation,            a3
 *   ULONG ThreadInformationLength,      a4
 *   PULONG ReturnLength)                a5
 *
 * الصنف 0 = THREAD_BASIC_INFORMATION:
 *   0x00 NTSTATUS ExitStatus
 *   0x08 PTEB TebBaseAddress
 *   0x10 CLIENT_ID ClientId
 *   0x18 KAFFINITY AffinityMask
 *   0x20 KPRIORITY Priority
 *   0x24 KPRIORITY BasePriority
 */
static long ntb0_NtQueryInformationThread(long a1, long a2, long a3, long a4,
                                           long a5, long a6, long a7, long a8,
                                           long a9, long a10, long a11)
{
    uint64_t handle = (uint64_t)a1;
    uint32_t cls = (uint32_t)a2;
    unsigned char *out = (unsigned char *)(uintptr_t)a3;
    uint32_t len = (uint32_t)a4;
    uint32_t *ret = (uint32_t *)(uintptr_t)a5;
    uint32_t exit_code = 0;
    uint32_t tid = 0;

    (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!out)
        return NTB0_ST_INVALID_PARAMETER;

    if (ntb0_handle_kind(handle) != NTB0_HK_THREAD)
        return NTB0_ST_INVALID_HANDLE;

    tid = ntb0_thread_tid(handle);
    ntb0_thread_exit_code(handle, &exit_code);

    switch (cls) {
    case 0: { /* ThreadBasicInformation */
        if (len < 0x28)
            return NTB0_ST_STATUS_LOGICAL_ERROR; /* BUFFER_TOO_SMALL */

        memset(out, 0, 0x28);
        *(int32_t *)(out + 0x00) = (int32_t)exit_code;
        *(uint64_t *)(out + 0x08) = (uint64_t)(uintptr_t)ntb0_fake_peb;
        *(uint32_t *)(out + 0x10) = (uint32_t)getpid();
        *(uint32_t *)(out + 0x14) = tid;
        *(uint64_t *)(out + 0x18) = 1;      /* AffinityMask */
        *(int32_t *)(out + 0x20) = 0;      /* Priority */
        *(int32_t *)(out + 0x24) = 0;      /* BasePriority */

        if (ret)
            *ret = 0x28;
        return NTB0_ST_SUCCESS;
    }

    default:
        return NTB0_ST_NOT_SUPPORTED;
    }
}

/* ------------------------------------------------------------------ */
/* SYS-041 — NtSetInformationThread (T-048)                             */
/* ------------------------------------------------------------------ */

/*
 * NtSetInformationThread(
 *   HANDLE ThreadHandle,                a1
 *   THREADINFOCLASS ThreadInformationClass, a2
 *   PVOID ThreadInformation,             a3
 *   ULONG ThreadInformationLength,       a4
 *
 * "الافتراضيات" (virtualizations) المنفَّذة:
 *   2 ThreadPriority      ⇒ setpriority() (رفع الأولوية يحتاج privilege
 *                          ⇒ PRIVILEGE_NOT_HELD كما في NT تماماً)
 *   3 ThreadBasePriority  ⇒ setpriority()
 *   5 ThreadTebPointer    ⇒ يُخزَّن مؤشر TEB لكل خيط على حدة
 *   6 ThreadAffinityMask  ⇒ sched_setaffinity()
 *  10 ThreadIdealProcessor ⇒ تثبيت الخيط على معالج واحد (تقريب موثّق)
 *   9 ThreadHideFromDebugger ⇒ قبول (لا مصحّح أخطاء عندنا)
 *
 * المقبض: مقبض خيط حقيقي، أو المقبض الوهمي للخيط الحالي
 * (NtCurrentThread = 0xFFFFFFFFFFFFFFFE على x64).
 */
#define NTB0_PSEUDO_CURRENT_THREAD 0xFFFFFFFFFFFFFFFEull

static long ntb0_NtSetInformationThread(long a1, long a2, long a3, long a4,
                                         long a5, long a6, long a7, long a8,
                                         long a9, long a10, long a11)
{
    uint64_t handle = (uint64_t)a1;
    uint32_t cls = (uint32_t)a2;
    unsigned char *info = (unsigned char *)(uintptr_t)a3;
    uint32_t len = (uint32_t)a4;
    long tid = -1;   /* -1 ⇒ الخيط الحالي */
    int is_current = 0;
    long rc;

    (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!info)
        return NTB0_ST_INVALID_PARAMETER;

    if (handle == NTB0_PSEUDO_CURRENT_THREAD) {
        is_current = 1;
    } else {
        if (ntb0_handle_kind(handle) != NTB0_HK_THREAD)
            return NTB0_ST_INVALID_HANDLE;
        tid = ntb0_thread_linux_tid(handle);
        if (tid < 0)
            return NTB0_ST_INVALID_HANDLE;
    }

    switch (cls) {
    case NTB0_TIC_BASIC:
        /* صنف للقراءة فقط ⇒ NT يرفضه هنا */
        return NTB0_ST_INVALID_INFO_CLASS;

    case NTB0_TIC_TIMES:
        /* أوقات الخيط: لا معنى لها على لينكس ⇒ قبول صامت */
        return NTB0_ST_SUCCESS;

    case NTB0_TIC_PRIORITY:
    case NTB0_TIC_BASE_PRIORITY: {
        int32_t prio;

        if (len < sizeof(prio))
            return NTB0_ST_INVALID_PARAMETER;
        memcpy(&prio, info, sizeof(prio));
        rc = ntb0_thread_set_priority(tid, (int)prio);
        if (rc != NTB0_ST_SUCCESS)
            return rc;
        NTB0_OK("SYS-041", "thread priority set to %d (nice %d)", (int)prio,
                ntb0_thread_win_priority_to_nice((int)prio));
        return NTB0_ST_SUCCESS;
    }

    case NTB0_TIC_DEBUG_OBJECT:
        /* لا مصحّح أخطاء داخل الصندوق ⇒ نرفض بوضوح */
        return NTB0_ST_NOT_SUPPORTED;

    case NTB0_TIC_TEB_POINTER: {
        uint64_t teb;

        if (len < sizeof(teb))
            return NTB0_ST_INVALID_PARAMETER;
        memcpy(&teb, info, sizeof(teb));

        if (is_current) {
            ntb0_thread_set_current_teb(teb);
        } else {
            rc = ntb0_thread_set_teb(handle, teb);
            if (rc != NTB0_ST_SUCCESS)
                return rc;
        }
        NTB0_OK("SYS-041", "TEB pointer set to 0x%llx", (unsigned long long)teb);
        return NTB0_ST_SUCCESS;
    }

    case NTB0_TIC_AFFINITY_MASK: {
        uint64_t mask;

        if (len < sizeof(mask))
            return NTB0_ST_INVALID_PARAMETER;
        memcpy(&mask, info, sizeof(mask));

        rc = ntb0_thread_set_affinity(tid, mask);
        if (rc != NTB0_ST_SUCCESS)
            return rc;
        NTB0_OK("SYS-041", "thread affinity mask set to 0x%llx",
                (unsigned long long)mask);
        return NTB0_ST_SUCCESS;
    }

    case NTB0_TIC_HIDE_DEBUGGER:
        return NTB0_ST_SUCCESS;

    case NTB0_TIC_IDEAL_PROCESSOR: {
        uint32_t cpu;
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);

        if (len < sizeof(cpu))
            return NTB0_ST_INVALID_PARAMETER;
        memcpy(&cpu, info, sizeof(cpu));
        if (ncpu <= 0)
            ncpu = 1;
        rc = ntb0_thread_set_affinity(tid, 1ull << (cpu % (uint32_t)ncpu));
        if (rc != NTB0_ST_SUCCESS)
            return rc;
        NTB0_OK("SYS-041", "ideal processor %u -> pinned", cpu);
        return NTB0_ST_SUCCESS;
    }

    case NTB0_TIC_EXTENDED_CID:
        return NTB0_ST_SUCCESS;

    default:
        return NTB0_ST_NOT_SUPPORTED;
    }
}

/* ------------------------------------------------------------------ */
/* SYS-045 — NtCreateNamedPipeFile (T-051)                             */
/* ------------------------------------------------------------------ */

/*
 * NtCreateNamedPipeFile(
 *   PHANDLE FileHandle,                 a1
 *   ACCESS_MASK DesiredAccess,          a2
 *   POBJECT_ATTRIBUTES ObjectAttributes,a3
 *   PIO_STATUS_BLOCK IoStatusBlock,     a4
 *   PUNICODE_STRING FileName,           a5   (\Device\NamedPipe\Name)
 *   ULONG ShareAccess,                  a6
 *   ULONG CreateDisposition,            a7
 *   ULONG CreateOptions,                a8
 *   PVOID SecurityDescriptor,           a9
 *   ULONG NamedPipeType,                a10
 *   ULONG NamedPipeMode,                a11
 *   ULONG Timeout)                      a12  ← خارج المصيدة (11 معاملاً)
 *
 * الترجمة: **FIFO على لينكس** بنفس اسم الأنبوب:
 *   - CreateDisposition=FILE_CREATE ⇒ إنشاء (وإلا COLLISION)
 *   - FILE_OPEN ⇒ فتح موجود (وإلا OBJECT_NAME_NOT_FOUND)
 *   - FILE_OPEN_IF ⇒ إنشاء إن لم يوجد
 *   - الفتح بـ O_RDWR ⇒ لا يتجمّد ever (امتداد لينكس) — الأنبوب永远是 قابل
 *     للقراءة والكتابة من الطرف نفسه ⇒ لا انتظار على الطرف الآخر.
 *
 * حدّ موثّق: الأنبوب هنا **بايت-ستريم** (لا رسائل) ⇒ PIPE_READMODE_MESSAGE
 *   غير مدعوم؛ و Timeout (المعامل 12) لا يلتقطه المصيدة ⇒ يُعتبر 0.
 */
static long ntb0_NtCreateNamedPipeFile(long a1, long a2, long a3, long a4,
                                       long a5, long a6, long a7, long a8,
                                       long a9, long a10, long a11)
{
    uint64_t *out = (uint64_t *)(uintptr_t)a1;
    ntb0_io_status_block *iosb = (ntb0_io_status_block *)(uintptr_t)a4;
    ntb0_unicode_string *us = (ntb0_unicode_string *)(uintptr_t)a5;
    uint32_t disposition = (uint32_t)a7;
    char path[NTB0_PATH_MAX];
    struct stat st;
    int fd;
    long rc;

    (void)a3;  /* ObjectAttributes */
    (void)a6;  /* ShareAccess — يُطبَّق عبر جدول المقابض */
    (void)a8;  /* CreateOptions */
    (void)a9;  /* SecurityDescriptor */
    (void)a10; /* NamedPipeType */
    (void)a11; /* NamedPipeMode */

    if (!out)
        return NTB0_ST_INVALID_PARAMETER;

    rc = ntb0_path_pipe_from_nt(us, path, sizeof(path));
    if (rc != NTB0_ST_SUCCESS)
        return rc;

    if (stat(path, &st) == 0) {
        /* موجود مسبقاً */
        if (disposition == NTB0_FILE_CREATE) {
            NTB0_OK("SYS-045", "named pipe already exists: %s", path);
            return NTB0_ST_OBJECT_NAME_COLLISION;
        }
        if (disposition != NTB0_FILE_OPEN && disposition != NTB0_FILE_OPEN_IF) {
            return NTB0_ST_INVALID_PARAMETER;
        }
        if (!S_ISFIFO(st.st_mode)) {
            /* شيء موجود بنفس الاسم وليس أنبوباً */
            NTB0_OK("SYS-045", "name taken by a non-pipe: %s", path);
            return NTB0_ST_OBJECT_NAME_COLLISION;
        }
    } else {
        if (disposition == NTB0_FILE_OPEN) {
            NTB0_OK("SYS-045", "named pipe does not exist: %s", path);
            return NTB0_ST_OBJECT_NAME_NOT_FOUND;
        }
        if (disposition != NTB0_FILE_CREATE && disposition != NTB0_FILE_OPEN_IF) {
            return NTB0_ST_INVALID_PARAMETER;
        }
        if (mkfifo(path, 0666) < 0) {
            if (errno == EEXIST)
                return NTB0_ST_OBJECT_NAME_COLLISION;
            return ntb0_errno_to_ntstatus(errno);
        }
    }

    /* O_RDWR على FIFO ⇒ لا انتظار (امتداد لينكس) */
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return ntb0_errno_to_ntstatus(errno);

    rc = ntb0_handle_create(fd, (uint32_t)a2, 0, path, out);
    if (rc != NTB0_ST_SUCCESS) {
        close(fd);
        return rc;
    }

    if (iosb) {
        iosb->status = NTB0_ST_SUCCESS;
        iosb->information = 0;
    }

    NTB0_OK("SYS-045", "named pipe '%s' -> handle 0x%llx", path,
            (unsigned long long)*out);
    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SYS-046 — NtQueryFullAttributesFile (T-056)                         */
/* ------------------------------------------------------------------ */

/*
 * NtQueryFullAttributesFile(
 *   HANDLE FileHandle,           a1
 *   PIO_STATUS_BLOCK IoStatusBlock, a2
 *   PVOID Buffer,                a3
 *   ULONG Length,                a4
 *   PULONG ReturnLength)         a5
 *
 * FILE_FULL_ATTRIBUTE_INFORMATION (0x48 بايت على x64):
 *   0x00 NextEntryOffset · 0x04 FileAttributes
 *   0x08 CreationTime · 0x0C LastAccessTime · 0x10 LastWriteTime
 *   0x14 ChangeTime · 0x18 AllocationSize · 0x1C EndOfFile · 0x20 FileNumber
 *   0x24 EarliestChangeTime
 *   0x28 AllocationSizeLarge · 0x30 EndOfFileLarge · 0x38 FileNumberLarge
 *   0x40 NumberOfLinks · 0x44 EffectiveAccess
 */
#define NTB0_FILE_FULL_ATTR_SIZE 0x48

static long ntb0_NtQueryFullAttributesFile(long a1, long a2, long a3, long a4,
                                           long a5, long a6, long a7, long a8,
                                           long a9, long a10, long a11)
{
    uint64_t handle = (uint64_t)a1;
    ntb0_io_status_block *iosb = (ntb0_io_status_block *)(uintptr_t)a2;
    unsigned char *out = (unsigned char *)(uintptr_t)a3;
    uint32_t len = (uint32_t)a4;
    uint32_t *ret = (uint32_t *)(uintptr_t)a5;
    struct stat st;
    uint32_t attrs = 0;
    int fd;

    (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!out)
        return NTB0_ST_INVALID_PARAMETER;

    fd = ntb0_handle_fd(handle);
    if (fd < 0)
        return NTB0_ST_INVALID_HANDLE;

    if (fstat(fd, &st) < 0)
        return ntb0_errno_to_ntstatus(errno);

    if (len < NTB0_FILE_FULL_ATTR_SIZE)
        return NTB0_ST_STATUS_LOGICAL_ERROR; /* BUFFER_TOO_SMALL */

    if (S_ISDIR(st.st_mode))
        attrs |= NTB0_FILE_ATTRIBUTE_DIRECTORY;
    else if ((st.st_mode & S_IWUSR) == 0)
        attrs |= NTB0_FILE_ATTRIBUTE_READONLY;
    else
        attrs |= NTB0_FILE_ATTRIBUTE_NORMAL;

    memset(out, 0, NTB0_FILE_FULL_ATTR_SIZE);

    *(uint32_t *)(out + 0x00) = 0; /* NextEntryOffset */
    *(uint32_t *)(out + 0x04) = attrs;
    *(uint32_t *)(out + 0x08) = (uint32_t)(st.st_ctim.tv_sec * 10000000LL);
    *(uint32_t *)(out + 0x0C) = (uint32_t)(st.st_atim.tv_sec * 10000000LL);
    *(uint32_t *)(out + 0x10) = (uint32_t)(st.st_mtim.tv_sec * 10000000LL);
    *(uint32_t *)(out + 0x14) = (uint32_t)(st.st_ctim.tv_sec * 10000000LL);
    *(uint32_t *)(out + 0x18) = (uint32_t)st.st_size;   /* AllocationSize */
    *(uint32_t *)(out + 0x1C) = (uint32_t)st.st_size;   /* EndOfFile */
    *(uint32_t *)(out + 0x20) = (uint32_t)st.st_ino;    /* FileNumber */
    *(uint32_t *)(out + 0x24) = (uint32_t)(st.st_ctim.tv_sec * 10000000LL);
    *(uint64_t *)(out + 0x28) = (uint64_t)st.st_blocks * 512ULL;
    *(uint64_t *)(out + 0x30) = (uint64_t)st.st_size;
    *(uint64_t *)(out + 0x38) = (uint64_t)st.st_ino;
    *(uint32_t *)(out + 0x40) = (uint32_t)st.st_nlink;
    *(uint32_t *)(out + 0x44) = ntb0_handle_desired(handle);

    if (ret)
        *ret = NTB0_FILE_FULL_ATTR_SIZE;
    if (iosb) {
        iosb->status = NTB0_ST_SUCCESS;
        iosb->information = 0;
    }

    return NTB0_ST_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* SYS-048 — NtSetSystemInformation (T-036)                            */
/* ------------------------------------------------------------------ */

/*
 * إزاحة ساعة الصندوق (100-نانية) — تُضبط عبر NtSetSystemInformation
 * (صنف 3) وتُقرأ عبر NtQuerySystemInformation (صنف 3).
 */
static int64_t ntb0_time_offset_100ns;

/* وقت الإقلاع (من /proc/stat) بالوحدات 100-نانية */
static int64_t ntb0_boot_time_100ns(void)
{
    FILE *f = fopen("/proc/stat", "r");
    char line[256];
    int64_t btime = 0;

    if (!f)
        return 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "btime ", 6) == 0) {
            btime = (int64_t)strtoll(line + 6, NULL, 10);
            break;
        }
    }
    fclose(f);
    return btime * 10000000LL;
}

/* الوقت الحالي المخزَّن (100-نانية) مع الإزاحة */
static int64_t ntb0_time_now_100ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 10000000LL + ts.tv_nsec / 100 + ntb0_time_offset_100ns;
}

/*
 * NtSetSystemInformation(
 *   SYSTEM_INFORMATION_CLASS SystemInformationClass, a1
 *   PVOID SystemInformation,                            a2
 *   ULONG SystemInformationLength,                     a3
 *
 * الأصناف المنفَّذة:
 *   3  SystemTimeOfDayInformation ⇒ ضبط إزاحة ساعة الصندوق
 *   4  SystemTimeAdjustmentInformation ⇒ إضافة تعديل على الإزاحة
 *   12 SystemKernelDebuggerInformation ⇒ قبول (لا مصحّح عندنا)
 *   14 SystemRegistryQuotaInformation  ⇒ قبول (بلا أثر)
 *   16 SystemPrioritySeparation        ⇒ تخزين
 *   22 SystemThreadPriorityClientIdInformation ⇒ ضبط أولوية خيط
 *   35 SystemDebuggerInformation       ⇒ قبول (بلا أثر)
 *
 * **الأصناف المرفوضة عمداً (قاعدة أمان الصندوق):**
 *   5  SystemShutdownInformation          ⇒ لا يُطفأ المضيف أبداً
 *   11 SystemCrashDumpInformation         ⇒ لا يُعطِّل المضيف أبداً
 *   15 SystemExtendServiceTableInformation ⇒ لا تُثبَّت جداول خدمات في النواة
 *   ⇒ كلها STATUS_PRIVILEGE_NOT_HELD (كما لو طلبها كود بلا امتياز).
 */
static long ntb0_NtSetSystemInformation(long a1, long a2, long a3, long a4,
                                        long a5, long a6, long a7, long a8,
                                        long a9, long a10, long a11)
{
    uint32_t cls = (uint32_t)a1;
    unsigned char *info = (unsigned char *)(uintptr_t)a2;
    uint32_t len = (uint32_t)a3;

    (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10;
    (void)a11;

    switch (cls) {
    case NTB0_SIC_TIMEOFDAY: {
        /* SYSTEM_TIMEOFDAY_INFORMATION: BootTime · CurrentTime · TimeZoneBias */
        int64_t current;

        if (!info || len < 24)
            return NTB0_ST_INVALID_PARAMETER;

        memcpy(&current, info + 8, sizeof(current));
        ntb0_time_offset_100ns = current - ntb0_time_now_100ns();
        NTB0_OK("SYS-048", "system time offset set to %lld (100ns)",
                (long long)ntb0_time_offset_100ns);
        return NTB0_ST_SUCCESS;
    }

    case NTB0_SIC_TIME_ADJUSTMENT: {
        /* SYSTEM_TIME_ADJUSTMENT_INFORMATION: TimeAdjustment · Enable */
        int64_t adj;

        if (!info || len < 8)
            return NTB0_ST_INVALID_PARAMETER;
        memcpy(&adj, info, sizeof(adj));
        if (adj > 0 || adj < -100000000LL)
            return NTB0_ST_INVALID_PARAMETER; /* تعديل غير معقول */
        ntb0_time_offset_100ns += adj;
        NTB0_OK("SYS-048", "system time adjusted by %lld (100ns)",
                (long long)adj);
        return NTB0_ST_SUCCESS;
    }

    case NTB0_SIC_KERNEL_DEBUGGER:
    case NTB0_SIC_REGISTRY_QUOTA:
    case NTB0_SIC_DEBUGGER:
        return NTB0_ST_SUCCESS;

    case NTB0_SIC_PRIORITY_SEPARATION:
        if (!info || len < 4)
            return NTB0_ST_INVALID_PARAMETER;
        return NTB0_ST_SUCCESS;

    case NTB0_SIC_THREAD_PRIORITY_CID: {
        /* SYSTEM_THREAD_PRIORITY_CLIENT_ID_INFORMATION: ClientId · Priority */
        uint32_t cid;
        int32_t prio;

        if (!info || len < 8)
            return NTB0_ST_INVALID_PARAMETER;
        memcpy(&cid, info, sizeof(cid));
        memcpy(&prio, info + 4, sizeof(prio));
        if (cid == 0)
            return NTB0_ST_INVALID_PARAMETER;
        /* على لينكس: معرّف العملية = معرّف الخيط ⇒ نضبط أولوية العملية */
        if (setpriority(PRIO_PROCESS, (id_t)cid,
                        ntb0_thread_win_priority_to_nice((int)prio)) < 0) {
            if (errno == EACCES || errno == EPERM)
                return NTB0_ST_PRIVILEGE_NOT_HELD;
            return NTB0_ST_UNSUCCESSFUL;
        }
        return NTB0_ST_SUCCESS;
    }

    /* ── مرفوضة عمداً (أمان المضيف) ── */
    case NTB0_SIC_SHUTDOWN:
    case NTB0_SIC_CRASH_DUMP:
    case NTB0_SIC_EXTEND_SERVICE_TABLE:
        NTB0_OK("SYS-048", "system class %u refused (host safety)", cls);
        return NTB0_ST_PRIVILEGE_NOT_HELD;

    default:
        return NTB0_ST_NOT_SUPPORTED;
    }
}

/* ------------------------------------------------------------------ */
/* SYS-049/050/051/052/053 — الريجستري (T-110 … T-114)                */
/* ------------------------------------------------------------------ */

/*
 * NtOpenKey(
 *   PHANDLE KeyHandle,                a1
 *   ACCESS_MASK DesiredAccess,        a2
 *   POBJECT_ATTRIBUTES ObjectAttributes, a3   → ObjectName = UNICODE_STRING
 *
 *   المسار: \Registry\Machine\SYSTEM\ControlSet001\Services\RpcSs
 *   ⇒ جسر إلى قارئ الخلايا الحقيقي (ntb0_reg.c).
 */
static long ntb0_NtOpenKey(long a1, long a2, long a3, long a4, long a5,
                           long a6, long a7, long a8, long a9, long a10,
                           long a11)
{
    uint64_t *out = (uint64_t *)(uintptr_t)a1;
    ntb0_object_attributes *oa = (ntb0_object_attributes *)(uintptr_t)a3;

    (void)a2; /* DesiredAccess */
    (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10;
    (void)a11;

    if (!out || !oa || !oa->object_name)
        return NTB0_ST_INVALID_PARAMETER;

    return ntb0_reg_open_key(out, oa->object_name);
}

/*
 * NtQueryValueKey(
 *   HANDLE KeyHandle,                a1
 *   PUNICODE_STRING ValueName,       a2
 *   KEY_VALUE_INFORMATION_CLASS Class, a3
 *   PVOID KeyValueInformation,       a4
 *   ULONG KeyValueInformationLength, a5
 *   PULONG ResultLength)             a6
 */
static long ntb0_NtQueryValueKey(long a1, long a2, long a3, long a4, long a5,
                                 long a6, long a7, long a8, long a9, long a10,
                                 long a11)
{
    ntb0_unicode_string *name = (ntb0_unicode_string *)(uintptr_t)a2;

    (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    if (!name)
        return NTB0_ST_INVALID_PARAMETER;

    return ntb0_reg_query_value((uint64_t)a1, name, (uint32_t)a3,
                                (void *)(uintptr_t)a4, (uint32_t)a5,
                                (uint32_t *)(uintptr_t)a6);
}

/*
 * NtEnumerateKey(
 *   HANDLE KeyHandle,                     a1
 *   KEY_ENUMERATION_INFORMATION_CLASS Class, a2
 *   PVOID KeyInformation,                 a3
 *   ULONG Length,                         a4
 *   PULONG ResultLength)                  a5
 *
 *   ملاحظة: الـ Index يأتي في المعامل الثالث (a3) — وهو DWORD.
 */
static long ntb0_NtEnumerateKey(long a1, long a2, long a3, long a4, long a5,
                                long a6, long a7, long a8, long a9, long a10,
                                long a11)
{
    (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    return ntb0_reg_enum_key((uint64_t)a1, (uint32_t)a2, (uint32_t)a3,
                              (void *)(uintptr_t)a4, (uint32_t)a5,
                              (uint32_t *)(uintptr_t)a6);
}

/*
 * NtEnumerateValueKey(
 *   HANDLE KeyHandle,                     a1
 *   KEY_VALUE_INFORMATION_CLASS Class,    a2
 *   PVOID KeyValueInformation,            a3
 *   ULONG Length,                         a4
 *   PULONG ResultLength)                  a5
 */
static long ntb0_NtEnumerateValueKey(long a1, long a2, long a3, long a4, long a5,
                                     long a6, long a7, long a8, long a9, long a10,
                                     long a11)
{
    (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;

    return ntb0_reg_enum_value((uint64_t)a1, (uint32_t)a2, (uint32_t)a3,
                                (void *)(uintptr_t)a4, (uint32_t)a5,
                                (uint32_t *)(uintptr_t)a6);
}

/* NtFlushKey(HANDLE KeyHandle) — الخلية للقراءة فقط ⇒ لا عمل */
static long ntb0_NtFlushKey(long a1, long a2, long a3, long a4, long a5,
                            long a6, long a7, long a8, long a9, long a10,
                            long a11)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8;
    (void)a9; (void)a10; (void)a11;

    return ntb0_reg_flush((uint64_t)a1);
}

static ntb0_probe_fn ntb0_probe;

/*
 * وضع "الفشل المتوقّع" (للاختبار فقط):
 *   الفشل هنا سلوك صحيح مُقصود (مثل فتح ملف غير موجود) — لا يُسجَّل FAIL
 *   حتى لا يُخلط بتقرير "بلا FAIL" (NTBOX.md §9).
 */
static int ntb0_expect_fail;

void ntb0_set_probe(ntb0_probe_fn fn)
{
    ntb0_probe = fn;
}

void ntb0_set_expect_fail(int on)
{
    ntb0_expect_fail = on;
    ntb0_handle_set_expect_fail(on);
}

/* ------------------------------------------------------------------ */
/* إحصاءات الـ syscalls (NTBOX.md §11)                              */
/* ------------------------------------------------------------------ */

int ntb0_stats_get(struct ntb0_stat_pub *out, int max)
{
    int n = 0;
    unsigned int i;

    if (!out || max <= 0)
        return 0;

    for (i = 0; i < NTB0_SC_COUNT && n < max; i++) {
        out[n].id    = ntb0_sc_table[i].id;
        out[n].name  = ntb0_sc_table[i].name;
        out[n].calls = ntb0_stats[i].calls;
        out[n].fails = ntb0_stats[i].fails;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* نقطة الدخول                                                        */
/* ------------------------------------------------------------------ */

long ntb0_dispatch(long num, long a1, long a2, long a3, long a4, long a5,
                   long a6, long a7, long a8, long a9, long a10, long a11)
{
    const struct ntb0_sc *e = ntb0_sc_find((uint32_t)num);
    long rc;

    if (ntb0_probe)
        ntb0_probe(num, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);

    if (!e) {
        /*
         * syscall معروف في ntdll لكن ليس في جدولنا.
         * نعرف اسمه من الجدول المستخرج (400 مدخل) — نفيد ولا ننفّذ.
         */
        const char *nm = ntb0_syscall_name((uint32_t)num);
        NTB0_UNIMPL("SYS-900", "syscall 0x%lx (%s) not translated yet", num, nm);
        return NTB0_ST_NOT_IMPLEMENTED;
    }

    ntb0_stats[e->idx].calls++;

    if (!e->fn) {
        NTB0_STUB(e->id, "%s (0x%lx) is a stub", e->name, num);
        return NTB0_ST_NOT_IMPLEMENTED;
    }

    /*
     * حماية من الإلغاء أثناء المعالجة: pthread_cancel (NtTerminateThread)
     * قد يقع داخل المترجم فيترك قفل جدول المقابض مقفولاً ⇒ نمنع الإلغاء
     * أثناء تنفيذ المعالج ونعيده بعده.
     */
    {
        int old_cancel_state = 0;

        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel_state);
        rc = e->fn(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
        pthread_setcancelstate(old_cancel_state, NULL);
    }
    if (rc == NTB0_ST_SUCCESS) {
        NTB0_OK(e->id, "%s (0x%lx) -> 0x%lx", e->name, num, rc);
    } else if (ntb0_expect_fail) {
        NTB0_OK(e->id, "%s (0x%lx) -> 0x%lx (expected failure)", e->name, num,
                rc);
    } else {
        ntb0_stats[e->idx].fails++;
        NTB0_FAIL(e->id, "%s (0x%lx) -> 0x%lx", e->name, num, rc);
    }
    return rc;
}
