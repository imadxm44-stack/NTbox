/*
 * test_trap — اختبار المصيدة على ntdll.dll الحقيقي (Windows 7 x64).
 *
 * ما يختبره:
 *   1. رسم صورة ntdll في الذاكرة مع قفزة اعتراض.
 *   2. تعديل كل ستوب syscall **في مكانه** والتحقق من العدد (المتوقع 400).
 *   3. التحقق من بايتات التعديل (E9 rel32).
 *   4. التحقق من وصول الرقم والـ 11 arguments كاملةً للمترجم
 *      (بما فيها arg5..arg11 التي تُمرَّر على المكدس عند [rsp+0x28]..[rsp+0x58]).
 *   5. جولة وظيفية كاملة: NtClose على ملف حقيقي → STATUS_SUCCESS.
 *   6. SYS-005: NtCreateFile على ملف حقيقي داخل جذر ويندوز 7
 *      (فتح + قراءة أول بايتين + إغلاق) — مع فحص المسار المترجَم.
 *   7. حالات الفشل المتوقّع: ملف موجود (COLLISION) / ملف غير موجود (NOT_FOUND).
 *   8. جدول ترجمة المسار: صيغ NT المختلفة + رفض ".." والرجستري و UNC.
 *   9. SYS-006: NtOpenFile (0x30) — فتح ملف/مجلد + حالات الفشل.
 *  10. SYS-003: NtReadFile (0x03) — قراءة عند إزاحة / متتابعة / EOF.
 *  11. SYS-004: NtWriteFile (0x05) — كتابة + قراءة راجعة + إثبات على القرص.
 *  12. جدول الـ syscalls المستخرج + إحصاءات الترجمة.
 *
 * البناء:
 *   gcc -O2 -Wall -Wextra -I../../ntboxreg -I. -o test_trap test_trap.c \
 *       ../../ntboxreg/ntdll_syscalls.c ntb0_trap.c ntb0_dispatch.c \
 *       ntb0_path.c ntb0_trampoline.S
 *
 * التشغيل:
 *   ./test_trap [مسار ntdll.dll] [جذر ويندوز]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "ntb0_trap.h"
#include "ntb0_dispatch.h"
#include "ntb0_trace.h"
#include "ntb0_nt.h"
#include "ntb0_path.h"
#include "ntb0_handle.h"
#include "ntb0_thread.h"
#include "ntb0_reg.h"
#include "hive.h" /* ثوابت REG_* */
#include "ntdll_syscalls.h"

#define NTB0_DEFAULT_NTDLL "/tmp/opencode/win7root/Windows/System32/ntdll.dll"

/* مُساعد النداء بالاصطلاح الحقيقي لويندوز x64 (من ntb0_trampoline.S) */
extern long ntb0_call_win64(void *fn, long a1, long a2, long a3, long a4,
                             long a5, long a6, long a7, long a8, long a9,
                             long a10, long a11);

/* ------------------------------------------------------------------------- */
/* خطّاف الالتقاط — يسجّل ما وصل المترجم                                   */
/* ------------------------------------------------------------------------- */

static long probe_num, probe_a1, probe_a2, probe_a3, probe_a4, probe_a5;
static long probe_a6, probe_a7, probe_a8, probe_a9, probe_a10, probe_a11;
static int  probe_hits;

static void probe_fn(long num, long a1, long a2, long a3, long a4, long a5,
                     long a6, long a7, long a8, long a9, long a10, long a11)
{
    probe_num = num;
    probe_a1 = a1;
    probe_a2 = a2;
    probe_a3 = a3;
    probe_a4 = a4;
    probe_a5 = a5;
    probe_a6 = a6;
    probe_a7 = a7;
    probe_a8 = a8;
    probe_a9 = a9;
    probe_a10 = a10;
    probe_a11 = a11;
    probe_hits++;
}

/* ------------------------------------------------------------------------- */
/* البحث عن ستوب داخل الصورة                                                */
/* ------------------------------------------------------------------------- */

/*
 * بعد التعديل يصبح النمط: 4C 8B D1 | B8 <num32> | E9 <rel32>
 */
static void *ntb0_find_stub(void *base, size_t size, uint32_t number)
{
    uint8_t *p = (uint8_t *)base;
    uint8_t *end = p + size;

    while (p + 16 <= end) {
        if (p[0] == 0x4C && p[1] == 0x8B && p[2] == 0xD1 && p[3] == 0xB8 &&
            p[8] == 0xE9) {
            uint32_t n = *(const uint32_t *)(p + 4);
            if (n == number)
                return p;
        }
        p++;
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* أدوات مساعدة للاختبار                                                    */
/* ------------------------------------------------------------------------- */

/* ASCII -> UTF-16LE (يحتاجه اختبار NtCreateFile) */
static uint16_t *ntb0_test_utf16(const char *s, uint16_t *len_out)
{
    size_t n = strlen(s), i;
    uint16_t *b = (uint16_t *)malloc((n + 1) * sizeof(uint16_t));

    if (!b)
        return NULL;
    for (i = 0; i < n; i++)
        b[i] = (uint16_t)(unsigned char)s[i];
    b[n] = 0;
    *len_out = (uint16_t)(n * 2);
    return b;
}

/* ينشئ OBJECT_ATTRIBUTES حقيقياً (تخطيط Win64) لمسار NT */
static long ntb0_test_create_file(void *stub, const char *nt_path,
                                  uint32_t desired, uint32_t share,
                                  uint32_t disp, uint32_t opts,
                                  uint64_t *handle_out,
                                  ntb0_io_status_block *iosb)
{
    uint16_t ulen = 0;
    uint16_t *ubuf = ntb0_test_utf16(nt_path, &ulen);
    ntb0_unicode_string us;
    ntb0_object_attributes oa;
    long st;

    if (!ubuf)
        return (long)NTB0_ST_NO_MEMORY;

    memset(&us, 0, sizeof(us));
    us.length = ulen;
    us.maximum_length = ulen;
    us.buffer = ubuf;

    memset(&oa, 0, sizeof(oa));
    oa.length = (uint32_t)sizeof(oa);
    oa.object_name = &us;

    st = ntb0_call_win64(stub, (long)handle_out, (long)desired, (long)&oa,
                         (long)iosb, 0 /*AllocationSize*/, 0x80 /*FileAttributes*/,
                         (long)share, (long)disp, (long)opts, 0 /*EaBuffer*/,
                         0 /*EaLength*/);

    free(ubuf);
    return st;
}

/*
 * SYS-006 — NtOpenFile (10 معاملات — لا FileAttributes):
 *   a1=FileHandle* a2=DesiredAccess a3=ObjectAttributes* a4=IoStatusBlock*
 *   a5=AllocationSize* a6=ShareAccess a7=CreateDisposition a8=CreateOptions
 *   a9=EaBuffer a10=EaLength
 */
static long ntb0_test_open_file(void *stub, const char *nt_path,
                                uint32_t desired, uint32_t share,
                                uint32_t disp, uint32_t opts,
                                uint64_t *handle_out,
                                ntb0_io_status_block *iosb)
{
    uint16_t ulen = 0;
    uint16_t *ubuf = ntb0_test_utf16(nt_path, &ulen);
    ntb0_unicode_string us;
    ntb0_object_attributes oa;
    long st;

    if (!ubuf)
        return (long)NTB0_ST_NO_MEMORY;

    memset(&us, 0, sizeof(us));
    us.length = ulen;
    us.maximum_length = ulen;
    us.buffer = ubuf;

    memset(&oa, 0, sizeof(oa));
    oa.length = (uint32_t)sizeof(oa);
    oa.object_name = &us;

    st = ntb0_call_win64(stub, (long)handle_out, (long)desired, (long)&oa,
                         (long)iosb, 0 /*AllocationSize*/, (long)share,
                         (long)disp, (long)opts, 0 /*EaBuffer*/, 0 /*EaLength*/,
                         0 /*a11 — لا وجود له في NtOpenFile*/);

    free(ubuf);
    return st;
}

/*
 * SYS-003 / SYS-004 — NtReadFile / NtWriteFile (9 معاملات):
 *   a1=FileHandle a2=Event a3=ApcRoutine a4=ApcContext a5=IoStatusBlock*
 *   a6=Buffer a7=Length a8=ByteOffset* a9=ApcRoutineArgument
 */
static long ntb0_test_io(void *stub, uint64_t handle, void *buf, uint32_t len,
                         int64_t *offset, ntb0_io_status_block *iosb)
{
    return ntb0_call_win64(stub, (long)handle, 0 /*Event*/, 0 /*ApcRoutine*/,
                           0 /*ApcContext*/, (long)iosb, (long)buf, (long)len,
                           (long)offset, 0 /*ApcRoutineArgument*/, 0, 0);
}

/*
 * SYS-006bis — NtOpenFile بمسار **نسبي** + RootDirectory (T-025):
 *   نفس توقيع NtOpenFile لكن oa.root_directory = مقبض مجلد.
 */
static long ntb0_test_open_rel(void *stub, const char *nt_rel,
                                uint64_t root_handle, uint32_t desired,
                                uint32_t share, uint32_t disp, uint32_t opts,
                                uint64_t *handle_out,
                                ntb0_io_status_block *iosb)
{
    uint16_t ulen = 0;
    uint16_t *ubuf = ntb0_test_utf16(nt_rel, &ulen);
    ntb0_unicode_string us;
    ntb0_object_attributes oa;
    long st;

    if (!ubuf)
        return (long)NTB0_ST_NO_MEMORY;

    memset(&us, 0, sizeof(us));
    us.length = ulen;
    us.maximum_length = ulen;
    us.buffer = ubuf;

    memset(&oa, 0, sizeof(oa));
    oa.length = (uint32_t)sizeof(oa);
    oa.root_directory = (void *)(uintptr_t)root_handle;
    oa.object_name = &us;

    st = ntb0_call_win64(stub, (long)handle_out, (long)desired, (long)&oa,
                         (long)iosb, 0 /*AllocationSize*/, (long)share,
                         (long)disp, (long)opts, 0 /*EaBuffer*/, 0 /*EaLength*/,
                         0 /*a11*/);

    free(ubuf);
    return st;
}

/*
 * SYS-008 — NtDuplicateObject:
 *   a1=SourceProcessHandle a2=SourceHandle a3=TargetProcessHandle
 *   a4=TargetHandle* a5=DesiredAccess a6=HandleAttributes a7=Options
 */
static long ntb0_test_duplicate(void *stub, uint64_t src, uint32_t desired,
                                 uint32_t opts, uint64_t *out)
{
    return ntb0_call_win64(stub, -1 /*current process*/, (long)src, -1,
                           (long)out, (long)desired, 0 /*attributes*/,
                           (long)opts, 0, 0, 0, 0);
}

/* SYS-002 — NtAllocateVirtualMemory:
 *   a1=ProcessHandle a2=BaseAddress* a3=ZeroBits a4=RegionSize*
 *   a5=AllocationType a6=Protect
 */
static long ntb0_test_alloc_vm(void *stub, uint64_t *base, uint64_t *size,
                               uint32_t protect)
{
    return ntb0_call_win64(stub, 0 /*current process*/, (long)base,
                           0 /*ZeroBits*/, (long)size, 0x1000 /*MEM_COMMIT*/,
                           (long)protect, 0, 0, 0, 0, 0);
}

/* SYS-010 — NtProtectVirtualMemory:
 *   a1=ProcessHandle a2=BaseAddress* a3=RegionSize* a4=NewProtect
 *   a5=OldProtect*
 */
static long ntb0_test_protect_vm(void *stub, uint64_t *base, uint64_t *size,
                                 uint32_t new_protect, uint32_t *old_protect)
{
    return ntb0_call_win64(stub, 0, (long)base, (long)size,
                           (long)new_protect, (long)old_protect, 0, 0, 0, 0, 0,
                           0);
}

/* SYS-009 — NtFreeVirtualMemory:
 *   a1=ProcessHandle a2=BaseAddress* a3=RegionSize* a4=AllocationType
 *   a5=FreeType
 */
static long ntb0_test_free_vm(void *stub, uint64_t *base, uint64_t *size)
{
    return ntb0_call_win64(stub, 0, (long)base, (long)size,
                           0 /*AllocationType*/, 0x8000 /*MEM_RELEASE*/, 0, 0, 0,
                           0, 0, 0);
}

/* SYS-011 — NtQueryVirtualMemory:
 *   a1=ProcessHandle a2=BaseAddress a3=MemoryInformationClass
 *   a4=MemoryInformation a5=MemoryInformationLength* a6=ReturnLength*
 */
static long ntb0_test_query_vm(void *stub, uint64_t addr, void *info,
                               uint64_t *len)
{
    return ntb0_call_win64(stub, 0, (long)addr, 0 /*MemoryBasicInformation*/,
                           (long)info, (long)len, 0 /*ReturnLength*/, 0, 0, 0, 0,
                           0);
}

/* SYS-012 — NtQueryInformationFile:
 *   a1=FileHandle a2=IoStatusBlock* a3=FileInformation
 *   a4=FileInformationClass a5=Length
 */
static long ntb0_test_query_file(void *stub, uint64_t handle, void *info,
                                 uint32_t cls, uint32_t len)
{
    return ntb0_call_win64(stub, (long)handle, 0 /*IoStatusBlock*/,
                           (long)info, (long)cls, (long)len, 0, 0, 0, 0, 0, 0);
}

/*
 * مساعدات الاختبار للجولة الثانية (T-031 … T-039, T-054).
 * ملاحظة: ntb0_call_win64 takes exactly (stub + 11 args).
 */

/* SYS-013 — NtSetInformationFile:
 *   a1=FileHandle a2=IoStatusBlock* a3=FileInformation
 *   a4=FileInformationClass a5=Length
 */
static long ntb0_test_set_info_file(void *stub, uint64_t handle, void *info,
                                    uint32_t cls, uint32_t len)
{
    return ntb0_call_win64(stub, (long)handle, 0 /*IoStatusBlock*/,
                           (long)info, (long)cls, (long)len,
                           0, 0, 0, 0, 0, 0);
}

/* SYS-014 — NtFlushBuffersFile: a1=FileHandle a2=IoStatusBlock* */
static long ntb0_test_flush(void *stub, uint64_t handle)
{
    return ntb0_call_win64(stub, (long)handle, 0 /*IoStatusBlock*/,
                           0, 0, 0, 0, 0, 0, 0, 0, 0);
}

/* SYS-015 — NtDeviceIoControlFile:
 *   a1=FileHandle a2=ApcRoutine a3=ApcContext a4=IoStatusBlock*
 *   a5=InputBuffer a6=InputBufferLength a7=OutputBuffer
 *   a8=OutputBufferLength a9=ReturnLength* a10=IoControlCode a11=Reserved
 */
static long ntb0_test_ioctl(void *stub, uint64_t handle, void *in,
                            uint32_t inlen, void *out, uint32_t outlen,
                            uint32_t code, uint32_t *ret)
{
    return ntb0_call_win64(stub, (long)handle, 0 /*ApcRoutine*/,
                           0 /*ApcContext*/, 0 /*IoStatusBlock*/, (long)in,
                           (long)inlen, (long)out, (long)outlen, (long)ret,
                           (long)code, 0 /*Reserved*/);
}

/* SYS-016 — NtCreateEvent:
 *   a1=EventHandle* a2=DesiredAccess a3=ObjectAttributes*
 *   a4=EventType a5=InitialState
 */
static long ntb0_test_create_event(void *stub, uint64_t *out, uint32_t type,
                                   int initial_state)
{
    return ntb0_call_win64(stub, (long)out, 0 /*DesiredAccess*/,
                           0 /*ObjectAttributes*/, (long)type,
                           (long)(initial_state ? 1 : 0),
                           0, 0, 0, 0, 0, 0);
}

/* SYS-017 — NtSetEvent: a1=EventHandle */
static long ntb0_test_set_event(void *stub, uint64_t handle)
{
    return ntb0_call_win64(stub, (long)handle,
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

/* SYS-018 — NtClearEvent: a1=EventHandle */
static long ntb0_test_clear_event(void *stub, uint64_t handle)
{
    return ntb0_call_win64(stub, (long)handle,
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

/* SYS-019 — NtWaitForSingleObject:
 *   a1=ObjectHandle a2=Alertable a3=Timeout*
 */
static long ntb0_test_wait(void *stub, uint64_t handle, int64_t timeout_100ns)
{
    return ntb0_call_win64(stub, (long)handle, 0 /*Alertable*/,
                           (long)&timeout_100ns,
                           0, 0, 0, 0, 0, 0, 0, 0);
}

/* SYS-020 — NtDelayExecution: a1=Alertable a2=Interval* */
static long ntb0_test_delay(void *stub, int64_t interval_100ns)
{
    return ntb0_call_win64(stub, 0 /*Alertable*/, (long)&interval_100ns,
                           0, 0, 0, 0, 0, 0, 0, 0, 0);
}

/* SYS-021 — NtYieldExecution: no arguments */
static long ntb0_test_yield(void *stub)
{
    return ntb0_call_win64(stub, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

/* SYS-022 — NtQuerySystemInformation:
 *   a1=SystemInformationClass a2=SystemInformation
 *   a3=SystemInformationLength a4=ReturnLength*
 */
static long ntb0_test_query_system(void *stub, uint32_t cls, void *buf,
                                   uint32_t len, uint32_t *ret)
{
    return ntb0_call_win64(stub, (long)cls, (long)buf, (long)len, (long)ret,
                           0, 0, 0, 0, 0, 0, 0);
}

/* SYS-023 — NtQueryDirectoryFile:
 *   a1=FileHandle a2=Event a3=ApcRoutine a4=ApcContext a5=IoStatusBlock*
 *   a6=FileInformation a7=FileInformationClass a8=Length
 *   a9=ApcRoutineArgument* a10=ReturnLength*
 */
static long ntb0_test_query_dir(void *stub, uint64_t handle, void *buf,
                                uint32_t len, uint32_t *ret)
{
    return ntb0_call_win64(stub, (long)handle, 0 /*Event*/,
                           0 /*ApcRoutine*/, 0 /*ApcContext*/,
                           0 /*IoStatusBlock*/, (long)buf,
                           1 /*FileDirectoryInformation*/, (long)len,
                           0 /*ApcRoutineArgument*/, (long)ret, 0);
}

/*
 * مساعدات الاختبار للجولة الثالثة (T-034 · T-047 · T-052 · T-053 · T-055).
 */

/* SYS-024 — NtCreateSection:
 *   a1=SectionHandle* a2=DesiredAccess a3=ObjectAttributes*
 *   a4=SectionSize* a5=AllocationType a6=FileHandle
 */
static long ntb0_test_create_section(void *stub, uint64_t *out, uint64_t size,
                                     uint64_t file_handle)
{
    int64_t sz = (int64_t)size;

    return ntb0_call_win64(stub, (long)out, 0 /*DesiredAccess*/,
                           0 /*ObjectAttributes*/, (long)&sz,
                           0 /*AllocationType*/, (long)file_handle,
                           0, 0, 0, 0, 0);
}

/* SYS-025 — NtMapViewOfSection:
 *   a1=SectionHandle a2=ProcessHandle a3=BaseAddress* a4=ViewSize*
 *   a5=AllocationType a6=AllocationProtect a7=SectionOffset*
 *   a8=ViewSizeOut* a9=SectionHandle2
 */
static long ntb0_test_map_view(void *stub, uint64_t section, uint64_t *base,
                                uint64_t *size, uint32_t protect)
{
    int64_t sz = (int64_t)*size;
    int64_t off = 0;

    return ntb0_call_win64(stub, (long)section, 0 /*ProcessHandle*/,
                           (long)base, (long)&sz, 0 /*AllocationType*/,
                           (long)protect, (long)&off, (long)&sz,
                           0 /*SectionHandle2*/, 0, 0);
}

/* SYS-026 — NtUnmapViewOfSection: a1=BaseAddress */
static long ntb0_test_unmap_view(void *stub, uint64_t base)
{
    return ntb0_call_win64(stub, (long)base, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

/* SYS-027 — NtFsControlFile (نفس شكل DeviceIoControl) */
static long ntb0_test_fs_control(void *stub, uint64_t handle, void *in,
                                 uint32_t inlen, void *out, uint32_t outlen,
                                 uint32_t code, uint32_t *ret)
{
    return ntb0_call_win64(stub, (long)handle, 0 /*ApcRoutine*/,
                           0 /*ApcContext*/, 0 /*IoStatusBlock*/, (long)in,
                           (long)inlen, (long)out, (long)outlen, (long)ret,
                           (long)code, 0 /*Reserved*/);
}

/* SYS-028 — NtLockFile:
 *   a1=FileHandle a2=Offset* a3=Length* a4=LockOperation a5=ProcessHandle
 *   a6=IoStatusBlock a7=Key a8=LockOffset* a9=LockLength* a10=LockType*
 */
static long ntb0_test_lock_file(void *stub, uint64_t handle, int64_t off,
                                int64_t len, uint32_t op)
{
    return ntb0_call_win64(stub, (long)handle, (long)&off, (long)&len,
                           (long)op, 0 /*ProcessHandle*/, 0 /*IoStatusBlock*/,
                           0 /*Key*/, 0, 0, 0 /*LockType*/, 0);
}

/* SYS-029 — NtUnlockFile (نفس الشكل) */
static long ntb0_test_unlock_file(void *stub, uint64_t handle, int64_t off,
                                  int64_t len)
{
    return ntb0_call_win64(stub, (long)handle, (long)&off, (long)&len,
                           0, 0, 0, 0, 0, 0, 0, 0);
}

/* SYS-030 — NtQueryAttributesFile:
 *   a1=FileHandle a2=IoStatusBlock a3=FileInformation a4=Length
 */
static long ntb0_test_query_attrs(void *stub, uint64_t handle, void *info,
                                  uint32_t len)
{
    return ntb0_call_win64(stub, (long)handle, 0 /*IoStatusBlock*/,
                           (long)info, (long)len, 0, 0, 0, 0, 0, 0, 0);
}

/* SYS-031 — NtQueryObject:
 *   a1=Handle a2=ObjectInformationClass a3=ObjectInformation
 *   a4=Length a5=ReturnLength*
 */
static long ntb0_test_query_object(void *stub, uint64_t handle, uint32_t cls,
                                   void *buf, uint32_t len, uint32_t *ret)
{
    return ntb0_call_win64(stub, (long)handle, (long)cls, (long)buf,
                           (long)len, (long)ret, 0, 0, 0, 0, 0, 0);
}

/* يقارن سلسلة UTF-16LE مع نص ASCII */
/* ------------------------------------------------------------------ */
/* الجولة الرابعة — الخيوط والعمليات (T-040 · T-041 · T-042 · T-046)  */
/* ------------------------------------------------------------------ */

/*
 * نقاط نداء الخيوط — **اصطلاح ويندوز x64** (rcx للمعامل الأول).
 * ms_abi يطلب من GCC توليد نفس اصطلاح MSVC ✓ — وهذا ما يجعلها
 * نقطة نداء خيط NT صالحة (نتحقّق منها بجسر ntb0_thread_entry).
 */
__attribute__((ms_abi)) static void *ntb0_thread_probe(void *param)
{
    unsigned char *p = (unsigned char *)param;

    p[0] = 0xA5;
    p[1] = 0x5A;
    p[2] = 0xC3;
    p[3] = 0x3C;
    return (void *)(uintptr_t)0x1234;
}

/* خيط لا ينتهي (للاختبار مع الإلغاء) — nanosleep نقطة إلغاء ✓ */
__attribute__((ms_abi)) static void *ntb0_thread_loop(void *param)
{
    struct timespec ts;

    (void)param;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000; /* 1ms */

    for (;;)
        nanosleep(&ts, NULL);

    return NULL;
}

/* SYS-032 — NtCreateThread:
 *   a1=ThreadHandle* a2=DesiredAccess a3=ObjectAttributes*
 *   a4=ClientId* a5=StartRoutine a6=Parameter a7=Flags a8=ThreadStackSize*
 */
static long ntb0_test_create_thread(void *stub, uint64_t *out, uint64_t start,
                                    uint64_t param, uint32_t flags,
                                    uint32_t *client_id)
{
    return ntb0_call_win64(stub, (long)out, 0 /*DesiredAccess*/,
                           0 /*ObjectAttributes*/, (long)client_id,
                           (long)start, (long)param, (long)flags,
                           0 /*ThreadStackSize*/, 0, 0, 0);
}

/* SYS-033 — NtResumeThread: a1=ThreadHandle a2=PreviousSuspendCount* */
static long ntb0_test_resume_thread(void *stub, uint64_t handle, uint32_t *prev)
{
    return ntb0_call_win64(stub, (long)handle, (long)prev,
                           0, 0, 0, 0, 0, 0, 0, 0, 0);
}

/* SYS-034 — NtTerminateThread: a1=ThreadHandle a2=ExitStatus */
static long ntb0_test_terminate_thread(void *stub, uint64_t handle,
                                       uint32_t status)
{
    return ntb0_call_win64(stub, (long)handle, (long)status,
                           0, 0, 0, 0, 0, 0, 0, 0, 0);
}

/* SYS-035 — NtOpenProcess:
 *   a1=ProcessHandle* a2=DesiredAccess a3=ObjectAttributes* a4=ClientId*
 */
static long ntb0_test_open_process(void *stub, uint64_t *out, uint32_t pid)
{
    uint32_t cid[2];

    cid[0] = pid;
    cid[1] = 0;

    return ntb0_call_win64(stub, (long)out, 0 /*DesiredAccess*/,
                           0 /*ObjectAttributes*/, (long)cid,
                           0, 0, 0, 0, 0, 0, 0);
}

/* SYS-036 — NtTerminateProcess: a1=ProcessHandle a2=ExitStatus */
static long ntb0_test_terminate_process(void *stub, uint64_t handle,
                                        uint32_t status)
{
    return ntb0_call_win64(stub, (long)handle, (long)status,
                           0, 0, 0, 0, 0, 0, 0, 0, 0);
}

/* SYS-037 — NtQueryInformationProcess:
 *   a1=Handle a2=Class a3=Info a4=Length a5=ReturnLength*
 */
static long ntb0_test_query_process(void *stub, uint64_t handle, uint32_t cls,
                                    void *buf, uint32_t len, uint32_t *ret)
{
    return ntb0_call_win64(stub, (long)handle, (long)cls, (long)buf,
                           (long)len, (long)ret, 0, 0, 0, 0, 0, 0);
}

/* SYS-038 — NtOpenThread:
 *   a1=ThreadHandle* a2=DesiredAccess a3=ObjectAttributes* a4=ClientId*
 */
static long ntb0_test_open_thread(void *stub, uint64_t *out, uint32_t tid)
{
    uint32_t cid[2];

    cid[0] = 0;
    cid[1] = tid;

    return ntb0_call_win64(stub, (long)out, 0 /*DesiredAccess*/,
                           0 /*ObjectAttributes*/, (long)cid,
                           0, 0, 0, 0, 0, 0, 0);
}

/* SYS-039 — NtSuspendThread: a1=ThreadHandle a2=PreviousSuspendCount* */
static long ntb0_test_suspend_thread(void *stub, uint64_t handle,
                                      uint32_t *prev)
{
    return ntb0_call_win64(stub, (long)handle, (long)prev,
                           0, 0, 0, 0, 0, 0, 0, 0, 0);
}

/* SYS-040 — NtQueryInformationThread:
 *   a1=Handle a2=Class a3=Info a4=Length a5=ReturnLength*
 */
static long ntb0_test_query_thread(void *stub, uint64_t handle, uint32_t cls,
                                   void *buf, uint32_t len, uint32_t *ret)
{
    return ntb0_call_win64(stub, (long)handle, (long)cls, (long)buf,
                           (long)len, (long)ret, 0, 0, 0, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* SYS-045 — NtCreateNamedPipeFile (T-051)                              */
/* ------------------------------------------------------------------ */

/*
 * a1=FileHandle* a2=DesiredAccess a3=ObjectAttributes* a4=IoStatusBlock*
 * a5=FileName* a6=ShareAccess a7=CreateDisposition a8=CreateOptions
 * a9=SecurityDescriptor a10=NamedPipeType a11=NamedPipeMode
 * (المعامل 12 = Timeout — خارج المصيدة ⇒ لا يُمرَّر)
 */
static long ntb0_test_create_pipe(void *stub, uint64_t *out, const char *nt_name,
                                  uint32_t disposition,
                                  ntb0_io_status_block *iosb)
{
    uint16_t ulen = 0;
    uint16_t *ubuf = ntb0_test_utf16(nt_name, &ulen);
    ntb0_unicode_string us;
    long st;

    if (!ubuf)
        return (long)NTB0_ST_NO_MEMORY;

    memset(&us, 0, sizeof(us));
    us.length = ulen;
    us.maximum_length = ulen;
    us.buffer = ubuf;

    st = ntb0_call_win64(stub, (long)out,
                         (long)(NTB0_GENERIC_READ | NTB0_GENERIC_WRITE),
                         0 /*ObjectAttributes*/, (long)iosb, (long)&us,
                         0 /*ShareAccess*/, (long)disposition, 0 /*CreateOptions*/,
                         0 /*SecurityDescriptor*/, 0 /*NamedPipeType*/,
                         0 /*NamedPipeMode*/);

    free(ubuf);
    return st;
}

/* ------------------------------------------------------------------ */
/* SYS-041 — NtSetInformationThread (T-048)                            */
/* ------------------------------------------------------------------ */

static long ntb0_test_set_thread_info(void *stub, uint64_t handle, uint32_t cls,
                                       void *info, uint32_t len)
{
    return ntb0_call_win64(stub, (long)handle, (long)cls, (long)info, (long)len,
                           0, 0, 0, 0, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* SYS-048 — NtSetSystemInformation (T-036)                           */
/* ------------------------------------------------------------------ */

static long ntb0_test_set_system_info(void *stub, uint32_t cls, void *info,
                                       uint32_t len)
{
    return ntb0_call_win64(stub, (long)cls, (long)info, (long)len, 0, 0, 0, 0,
                           0, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* SYS-046 — NtQueryFullAttributesFile (T-056)                        */
/* ------------------------------------------------------------------ */

static long ntb0_test_query_full_attrs(void *stub, uint64_t handle, void *buf,
                                        uint32_t len, uint32_t *ret)
{
    return ntb0_call_win64(stub, (long)handle, 0 /*IoStatusBlock*/, (long)buf,
                           (long)len, (long)ret, 0, 0, 0, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* SYS-049/050/051/052/053 — الريجستري (T-110 … T-114)                */
/* ------------------------------------------------------------------ */

/* NtOpenKey: a1=KeyHandle* a2=DesiredAccess a3=ObjectAttributes* */
static long ntb0_test_open_key(void *stub, uint64_t *out, const char *nt_path)
{
    uint16_t ulen = 0;
    uint16_t *ubuf = ntb0_test_utf16(nt_path, &ulen);
    ntb0_unicode_string us;
    ntb0_object_attributes oa;
    long st;

    if (!ubuf)
        return (long)NTB0_ST_NO_MEMORY;

    memset(&us, 0, sizeof(us));
    us.length = ulen;
    us.maximum_length = ulen;
    us.buffer = ubuf;

    memset(&oa, 0, sizeof(oa));
    oa.length = (uint32_t)sizeof(oa);
    oa.object_name = &us;

    st = ntb0_call_win64(stub, (long)out, (long)NTB0_GENERIC_READ, (long)&oa,
                         0, 0, 0, 0, 0, 0, 0, 0);

    free(ubuf);
    return st;
}

/* NtQueryValueKey: a1=handle a2=ValueName* a3=class a4=buf a5=len a6=ret* */
static long ntb0_test_query_value(void *stub, uint64_t handle, const char *vname,
                                  uint32_t cls, void *buf, uint32_t len,
                                  uint32_t *ret)
{
    uint16_t ulen = 0;
    uint16_t *ubuf = ntb0_test_utf16(vname, &ulen);
    ntb0_unicode_string us;
    long st;

    if (!ubuf)
        return (long)NTB0_ST_NO_MEMORY;

    memset(&us, 0, sizeof(us));
    us.length = ulen;
    us.maximum_length = ulen;
    us.buffer = ubuf;

    st = ntb0_call_win64(stub, (long)handle, (long)&us, (long)cls, (long)buf,
                         (long)len, (long)ret, 0, 0, 0, 0, 0);

    free(ubuf);
    return st;
}

/* NtEnumerateKey: a1=handle a2=class a3=index a4=buf a5=len a6=ret* */
static long ntb0_test_enum_key(void *stub, uint64_t handle, uint32_t cls,
                               uint32_t index, void *buf, uint32_t len,
                               uint32_t *ret)
{
    return ntb0_call_win64(stub, (long)handle, (long)cls, (long)index,
                           (long)buf, (long)len, (long)ret, 0, 0, 0, 0, 0);
}

/* NtEnumerateValueKey: نفس التوقيع مع class القيمة */
static long ntb0_test_enum_value(void *stub, uint64_t handle, uint32_t cls,
                                 uint32_t index, void *buf, uint32_t len,
                                 uint32_t *ret)
{
    return ntb0_call_win64(stub, (long)handle, (long)cls, (long)index,
                           (long)buf, (long)len, (long)ret, 0, 0, 0, 0, 0);
}

/* NtFlushKey: a1=handle */
static long ntb0_test_flush_key(void *stub, uint64_t handle)
{
    return ntb0_call_win64(stub, (long)handle, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

static int ntb0_u16_is(const uint16_t *u, const char *ascii)
{
    size_t i, len = strlen(ascii);

    if (!u)
        return 0;
    for (i = 0; i < len; i++) {
        if (u[i] != (uint16_t)(unsigned char)ascii[i])
            return 0;
    }
    return u[len] == 0;
}

/* يقارن اسم مدخل مجلد (UTF-16LE) مع نص ASCII */
static int ntb0_dir_name_is(const unsigned char *ent, const char *ascii)
{
    uint32_t n = *(uint32_t *)(ent + 0x3C);
    size_t i, len = strlen(ascii);

    if (n != len * 2)
        return 0;
    for (i = 0; i < len; i++) {
        uint16_t c = *(uint16_t *)(ent + 0x40 + i * 2);

        if (c != (uint16_t)(unsigned char)ascii[i])
            return 0;
    }
    return 1;
}

/* يفحص ترجمة مسار NT واحد */
static int ntb0_test_path(const char *nt_path, const char *expect)
{
    uint16_t ulen = 0;
    uint16_t *b = ntb0_test_utf16(nt_path, &ulen);
    ntb0_unicode_string us;
    char out[NTB0_PATH_MAX];
    long rc;
    int ok;

    if (!b)
        return 1;

    memset(&us, 0, sizeof(us));
    us.length = ulen;
    us.maximum_length = ulen;
    us.buffer = b;

    rc = ntb0_path_from_nt(&us, out, sizeof(out));
    ok = (rc == NTB0_ST_SUCCESS) && strcmp(out, expect) == 0;
    printf("%s: path '%s' -> '%s' (rc=0x%lx)\n", ok ? "PASS" : "FAIL",
           nt_path, ok ? out : "(unexpected)", rc);
    free(b);
    return ok ? 0 : 1;
}

/* يفحص أن مسار NT مرفوض */
static int ntb0_test_path_reject(const char *nt_path, const char *why)
{
    uint16_t ulen = 0;
    uint16_t *b = ntb0_test_utf16(nt_path, &ulen);
    ntb0_unicode_string us;
    char out[NTB0_PATH_MAX];
    long rc;
    int ok;

    if (!b)
        return 1;

    memset(&us, 0, sizeof(us));
    us.length = ulen;
    us.maximum_length = ulen;
    us.buffer = b;

    rc = ntb0_path_from_nt(&us, out, sizeof(out));
    ok = (rc != NTB0_ST_SUCCESS);
    printf("%s: path '%s' rejected (%s, rc=0x%lx)\n", ok ? "PASS" : "FAIL",
           nt_path, why, rc);
    free(b);
    return ok ? 0 : 1;
}

/* يقرأ بايتات من ملف على القرص مباشرة — للمقارنة مع ما يقرأه المترجم */
static int ntb0_disk_read(const char *path, off_t off, void *buf, size_t len)
{
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = pread(fd, buf, len, off);
    close(fd);
    return n == (ssize_t)len ? 0 : -1;
}

/* حجم الملف على القرص */
static long ntb0_disk_size(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0)
        return -1;
    return (long)st.st_size;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : NTB0_DEFAULT_NTDLL;
    const char *root = (argc > 2) ? argv[2] : NTB0_DEFAULT_ROOT;
    struct ntb0_image img;
    void *stub, *stub52, *stub50, *stub30, *stub03, *stub05;
    void *stub15, *stub39, *stub1b, *stub4d, *stub20, *stub0e;
    void *stub24, *stub48, *stub04, *stub45, *stub0b, *stub3b, *stub01;
    void *stub31, *stub43, *stub33, *stub32;
    void *stub47, *stub25, *stub27, *stub36, *stubE0, *stub188, *stub3a,
        *stub0d;
    void *stub4b, *stub4f, *stub23, *stub29, *stub16, *stubfe, *stub17b,
        *stub22;
    void *stub0a, *stub9b, *stub113, *stub16c;
    void *stub0f, *stub14, *stub2f, *stub10, *stubc3;
    int fd;
    int failures = 0;
    long status = 0;

    printf("== NT-Box syscall trap test ==\n");
    printf("ntdll: %s\n", path);
    printf("win7 root: %s\n", root);
    fflush(stdout);

    /* 1) الرسم */
    if (ntb0_map_pe(path, &img) < 0) {
        printf("FAIL: map\n");
        return 1;
    }

    /* 2) التعديل في المكان */
    if (ntb0_trap_patch(&img) < 0) {
        printf("FAIL: patch\n");
        return 1;
    }
    if (img.stubs_patched < 390 || img.stubs_patched > 410) {
        printf("FAIL: expected ~400 patched stubs, got %u\n",
               img.stubs_patched);
        failures++;
    } else {
        printf("PASS: %u syscall stubs patched in place\n",
               img.stubs_patched);
    }

    /* 3) التحقق من البايتات المعدَّلة */
    stub = ntb0_find_stub(img.base, img.size, 0x0c); /* NtClose */
    stub52 = ntb0_find_stub(img.base, img.size, 0x52); /* NtCreateFile */
    stub30 = ntb0_find_stub(img.base, img.size, 0x30); /* NtOpenFile */
    stub03 = ntb0_find_stub(img.base, img.size, 0x03); /* NtReadFile */
    stub05 = ntb0_find_stub(img.base, img.size, 0x05); /* NtWriteFile */
    stub15 = ntb0_find_stub(img.base, img.size, 0x15); /* NtAllocateVirtualMemory */
    stub39 = ntb0_find_stub(img.base, img.size, 0x39); /* NtDuplicateObject */
    stub1b = ntb0_find_stub(img.base, img.size, 0x1b); /* NtFreeVirtualMemory */
    stub4d = ntb0_find_stub(img.base, img.size, 0x4d); /* NtProtectVirtualMemory */
    stub20 = ntb0_find_stub(img.base, img.size, 0x20); /* NtQueryVirtualMemory */
    stub0e = ntb0_find_stub(img.base, img.size, 0x0e); /* NtQueryInformationFile */
    stub24 = ntb0_find_stub(img.base, img.size, 0x24); /* NtSetInformationFile */
    stub48 = ntb0_find_stub(img.base, img.size, 0x48); /* NtFlushBuffersFile */
    stub04 = ntb0_find_stub(img.base, img.size, 0x04); /* NtDeviceIoControlFile */
    stub45 = ntb0_find_stub(img.base, img.size, 0x45); /* NtCreateEvent */
    stub0b = ntb0_find_stub(img.base, img.size, 0x0b); /* NtSetEvent */
    stub3b = ntb0_find_stub(img.base, img.size, 0x3b); /* NtClearEvent */
    stub01 = ntb0_find_stub(img.base, img.size, 0x01); /* NtWaitForSingleObject */
    stub31 = ntb0_find_stub(img.base, img.size, 0x31); /* NtDelayExecution */
    stub43 = ntb0_find_stub(img.base, img.size, 0x43); /* NtYieldExecution */
    stub33 = ntb0_find_stub(img.base, img.size, 0x33); /* NtQuerySystemInformation */
    stub32 = ntb0_find_stub(img.base, img.size, 0x32); /* NtQueryDirectoryFile */
    stub47 = ntb0_find_stub(img.base, img.size, 0x47); /* NtCreateSection */
    stub25 = ntb0_find_stub(img.base, img.size, 0x25); /* NtMapViewOfSection */
    stub27 = ntb0_find_stub(img.base, img.size, 0x27); /* NtUnmapViewOfSection */
    stub36 = ntb0_find_stub(img.base, img.size, 0x36); /* NtFsControlFile */
    stubE0 = ntb0_find_stub(img.base, img.size, 0xe0); /* NtLockFile */
    stub188 = ntb0_find_stub(img.base, img.size, 0x188); /* NtUnlockFile */
    stub3a = ntb0_find_stub(img.base, img.size, 0x3a); /* NtQueryAttributesFile */
    stub0d = ntb0_find_stub(img.base, img.size, 0x0d); /* NtQueryObject */
    stub4b = ntb0_find_stub(img.base, img.size, 0x4b); /* NtCreateThread */
    stub4f = ntb0_find_stub(img.base, img.size, 0x4f); /* NtResumeThread */
    stub23 = ntb0_find_stub(img.base, img.size, 0x23); /* NtOpenProcess */
    stub29 = ntb0_find_stub(img.base, img.size, 0x29); /* NtTerminateProcess */
    stub16 = ntb0_find_stub(img.base, img.size, 0x16); /* NtQueryInformationProcess */
    stubfe = ntb0_find_stub(img.base, img.size, 0xfe); /* NtOpenThread */
    stub17b = ntb0_find_stub(img.base, img.size, 0x17b); /* NtSuspendThread */
    stub22 = ntb0_find_stub(img.base, img.size, 0x22); /* NtQueryInformationThread */
    stub0a = ntb0_find_stub(img.base, img.size, 0x0a); /* NtSetInformationThread */
    stub9b = ntb0_find_stub(img.base, img.size, 0x9b); /* NtCreateNamedPipeFile */
    stub113 = ntb0_find_stub(img.base, img.size, 0x113); /* NtQueryFullAttributesFile */
    stub16c = ntb0_find_stub(img.base, img.size, 0x16c); /* NtSetSystemInformation */
    stub0f = ntb0_find_stub(img.base, img.size, 0x0f); /* NtOpenKey */
    stub14 = ntb0_find_stub(img.base, img.size, 0x14); /* NtQueryValueKey */
    stub2f = ntb0_find_stub(img.base, img.size, 0x2f); /* NtEnumerateKey */
    stub10 = ntb0_find_stub(img.base, img.size, 0x10); /* NtEnumerateValueKey */
    stubc3 = ntb0_find_stub(img.base, img.size, 0xc3); /* NtFlushKey */
    if (!stub15 || !stub39 || !stub1b || !stub4d || !stub20 || !stub0e) {
        printf("FAIL: new syscall stubs not found (alloc=%p dup=%p free=%p "
               "prot=%p qvm=%p qif=%p)\n", stub15, stub39, stub1b, stub4d,
               stub20, stub0e);
        return 1;
    }
    if (!stub || !stub52 || !stub30 || !stub03 || !stub05) {
        printf("FAIL: stub not found (NtClose=%p NtCreateFile=%p "
               "NtOpenFile=%p NtReadFile=%p NtWriteFile=%p)\n", stub, stub52,
               stub30, stub03, stub05);
        return 1;
    }
    if (!stub24 || !stub48 || !stub04 || !stub45 || !stub0b || !stub3b ||
        !stub01 || !stub31 || !stub43 || !stub33 || !stub32 ||
        !stub47 || !stub25 || !stub27 || !stub36 || !stubE0 || !stub188 ||
        !stub3a || !stub0d) {
        printf("FAIL: second-round syscall stubs not found\n");
        return 1;
    }
    if (!stub4b || !stub4f || !stub23 || !stub29 || !stub16 || !stubfe ||
        !stub17b || !stub22) {
        printf("FAIL: fourth-round syscall stubs not found (thr=0x4b res=0x4f "
               "opn=0x23 term=0x29 qip=0x16 oth=0xfe sus=0x17b qit=0x22)\n");
        return 1;
    }
    if (!stub0a || !stub9b || !stub113 || !stub16c) {
        printf("FAIL: fifth-round syscall stubs not found (sit=0x0a pipe=0x9b "
               "qfa=0x113 ssi=0x16c)\n");
        return 1;
    }
    if (!stub0f || !stub14 || !stub2f || !stub10 || !stubc3) {
        printf("FAIL: registry syscall stubs not found (opk=0x0f qvk=0x14 "
               "enk=0x2f evk=0x10 flk=0xc3)\n");
        return 1;
    }
    {
        uint8_t *s = (uint8_t *)stub;
        printf("NtClose stub bytes:");
        for (int i = 0; i < 16; i++)
            printf(" %02x", s[i]);
        printf("\n");
        if (s[8] != 0xE9) {
            printf("FAIL: stub not patched (expected E9)\n");
            failures++;
        } else {
            printf("PASS: stub patched with E9 (rel32 jump)\n");
        }
    }
    {
        uint8_t *s = (uint8_t *)stub52;
        printf("NtCreateFile stub bytes:");
        for (int i = 0; i < 16; i++)
            printf(" %02x", s[i]);
        printf("\n");
        if (s[8] != 0xE9) {
            printf("FAIL: NtCreateFile stub not patched (expected E9)\n");
            failures++;
        } else {
            printf("PASS: NtCreateFile stub patched with E9 (rel32 jump)\n");
        }
    }

    /*
     * 4) وصول الرقم والـ arguments كاملةً
     *    نستخدم ستوب syscall **غير منفَّذ** (0x50 = NtTerminateThread):
     *    0x52 صار له handler حقيقي، فالمعاملات الوهمية كانت ستُفكَّر
     *    كـ pointers (segfault). المستخدَم هنا يعيد NOT_IMPLEMENTED فقط.
     */
    stub50 = ntb0_find_stub(img.base, img.size, 0x50);
    if (!stub50) {
        printf("FAIL: stub 0x50 not found\n");
        return 1;
    }
    ntb0_set_probe(probe_fn);
    /*
     * 0x50 = NtTerminateThread — له handler حقيقي الآن ⇒ يتوقّع فشلاً
     * (المقبض 0x1111 وهمي) ⇒ نلفّه بـ expect_fail (رفض صحيح = مصباح OK).
     */
    ntb0_set_expect_fail(1);
    status = ntb0_call_win64(stub50, 0x1111, 0x2222, 0x3333, 0x4444, 0x5555,
                             0x6666, 0x7777, 0x8888, 0x9999, 0xAAAA, 0xBBBB);
    ntb0_set_expect_fail(0);
    if (probe_hits != 1) {
        printf("FAIL: dispatcher not reached (hits=%d)\n", probe_hits);
        failures++;
    } else if (probe_num != 0x50) {
        printf("FAIL: syscall number 0x%lx (expected 0x50)\n", probe_num);
        failures++;
    } else if (probe_a1 != 0x1111 || probe_a2 != 0x2222 ||
               probe_a3 != 0x3333 || probe_a4 != 0x4444 ||
               probe_a5 != 0x5555 || probe_a6 != 0x6666 ||
               probe_a7 != 0x7777 || probe_a8 != 0x8888 ||
               probe_a9 != 0x9999 || probe_a10 != 0xAAAA ||
               probe_a11 != 0xBBBB) {
        printf("FAIL: args a1..a11 = 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx "
               "0x%lx 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx\n",
               probe_a1, probe_a2, probe_a3, probe_a4, probe_a5, probe_a6,
               probe_a7, probe_a8, probe_a9, probe_a10, probe_a11);
        failures++;
    } else {
        printf("PASS: dispatcher received num=0x%lx a1..a11 complete "
               "(a5..a11 from the Win64 stack)\n", probe_num);
    }
    ntb0_set_probe(NULL);

    /* 5) جولة وظيفية كاملة: NtClose على ملف حقيقي */
    fd = open("/tmp/opencode/trap_test_file.txt", O_RDONLY | O_CREAT, 0600);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    /*
     * بعد T-023: المقبض في NT-Box **ليس** رقم fd — بل كعكة من جدول
     * المقابض. فإغلاق fd خام يجب أن يُرفض: STATUS_INVALID_HANDLE.
     * (سلوك صحيح مقصود ⇒ لا يُحسب FAIL.)
     */
    ntb0_set_expect_fail(1);
    status = ntb0_call_win64(stub, (long)fd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    ntb0_set_expect_fail(0);
    if (status == NTB0_ST_INVALID_HANDLE) {
        printf("PASS: NtClose on a raw fd -> STATUS_INVALID_HANDLE (0x%lx)\n",
               status);
    } else {
        printf("FAIL: expected 0x%lx, got 0x%lx\n",
               (long)NTB0_ST_INVALID_HANDLE, status);
        failures++;
    }
    close(fd);

    /* 6) SYS-005 — NtCreateFile: فتح ملف حقيقي من جذر ويندوز 7 */
    printf("\n-- SYS-005: NtCreateFile (0x52) --\n");
    if (ntb0_path_set_root(root) < 0) {
        printf("FAIL: cannot set windows root '%s'\n", root);
        return 1;
    }

    {
        uint64_t handle = 0;
        ntb0_io_status_block iosb;
        char magic[2] = { 0, 0 };
        ssize_t n;

        memset(&iosb, 0, sizeof(iosb));

        ntb0_set_probe(probe_fn);
        status = ntb0_test_create_file(
            stub52, "\\??\\C:\\Windows\\System32\\ntdll.dll",
            NTB0_GENERIC_READ | NTB0_FILE_READ_ATTRIBUTES,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE |
                NTB0_FILE_SHARE_DELETE,
            NTB0_FILE_OPEN,
            NTB0_FILE_NON_DIRECTORY_FILE | NTB0_FILE_SYNCHRONOUS_IO_NONALERT,
            &handle, &iosb);
        ntb0_set_probe(NULL);

        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: NtCreateFile returned 0x%lx\n", status);
            failures++;
        } else if (handle == 0) {
            printf("FAIL: NtCreateFile did not return a handle\n");
            failures++;
        } else if (iosb.status != 0) {
            printf("FAIL: IO_STATUS_BLOCK.Status = 0x%x\n", iosb.status);
            failures++;
        } else if (probe_num != 0x52 || probe_a1 != (long)&handle ||
                   probe_a6 != 0x80 || probe_a8 != NTB0_FILE_OPEN) {
            printf("FAIL: NtCreateFile args wrong (num=0x%lx a8=0x%lx)\n",
                   probe_num, probe_a8);
            failures++;
        } else {
            printf("PASS: NtCreateFile returned STATUS_SUCCESS, handle=%llu\n",
                   (unsigned long long)handle);
        }

        /* إثبات أن الملف الحقيقي فُتح في مكانه (أول بايتين = MZ) */
        n = pread(ntb0_handle_fd(handle), magic, 2, 0);
        if (n == 2 && magic[0] == 'M' && magic[1] == 'Z') {
            printf("PASS: opened file is the real PE (magic MZ)\n");
        } else {
            printf("FAIL: opened file content = %02x %02x (n=%zd)\n",
                   (unsigned char)magic[0], (unsigned char)magic[1], n);
            failures++;
        }

        /* الإغلاق عبر NtClose — والـ fd الحقيقي يُغلق فعلياً */
        {
            int real_fd = ntb0_handle_fd(handle);

            status = ntb0_call_win64(stub, (long)handle, 0, 0, 0, 0, 0, 0, 0,
                                     0, 0, 0);
            if (status == 0 && real_fd >= 0 && close(real_fd) != 0) {
                printf("PASS: NtCreateFile handle closed via NtClose "
                       "(fd %d really closed)\n", real_fd);
            } else {
                printf("FAIL: NtClose on created handle failed (0x%lx)\n",
                       status);
                failures++;
            }
        }
    }

    /* 7) حالات الفشل المتوقّع (سلوك صحيح — لا FAIL في التقرير) */
    printf("\n-- SYS-005: expected failures --\n");
    ntb0_set_expect_fail(1);
    {
        uint64_t handle = 0;
        ntb0_io_status_block iosb;
        char tmp_rel[64];
        char tmp_abs[512];

        memset(&iosb, 0, sizeof(iosb));

        /* 7a) FILE_CREATE على ملف جديد */
        status = ntb0_test_create_file(
            stub52, "\\??\\C:\\ntb0_test_create.tmp", NTB0_GENERIC_WRITE,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE,
            NTB0_FILE_CREATE, NTB0_FILE_NON_DIRECTORY_FILE, &handle, &iosb);
        snprintf(tmp_rel, sizeof(tmp_rel), "ntb0_test_create.tmp");
        snprintf(tmp_abs, sizeof(tmp_abs), "%s/%s", root, tmp_rel);
        if (status == NTB0_ST_SUCCESS && handle != 0) {
            printf("PASS: FILE_CREATE created a new file (handle=%llu)\n",
                   (unsigned long long)handle);
            ntb0_call_win64(stub, (long)handle, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        } else {
            printf("FAIL: FILE_CREATE returned 0x%lx\n", status);
            failures++;
        }

        /* 7b) FILE_CREATE على ملف موجود → OBJECT_NAME_COLLISION */
        handle = 0;
        status = ntb0_test_create_file(
            stub52, "\\??\\C:\\ntb0_test_create.tmp", NTB0_GENERIC_WRITE,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE,
            NTB0_FILE_CREATE, NTB0_FILE_NON_DIRECTORY_FILE, &handle, &iosb);
        if (status == NTB0_ST_OBJECT_NAME_COLLISION) {
            printf("PASS: FILE_CREATE on existing file -> "
                   "STATUS_OBJECT_NAME_COLLISION (0x%lx)\n", status);
        } else {
            printf("FAIL: expected 0x%lx, got 0x%lx\n",
                   (long)NTB0_ST_OBJECT_NAME_COLLISION, status);
            failures++;
        }

        /* 7c) FILE_OPEN على ملف غير موجود → OBJECT_NAME_NOT_FOUND */
        handle = 0;
        status = ntb0_test_create_file(
            stub52, "\\??\\C:\\ntb0_test_missing.tmp", NTB0_GENERIC_READ,
            NTB0_FILE_SHARE_READ, NTB0_FILE_OPEN, NTB0_FILE_NON_DIRECTORY_FILE,
            &handle, &iosb);
        if (status == NTB0_ST_OBJECT_NAME_NOT_FOUND) {
            printf("PASS: FILE_OPEN on missing file -> "
                   "STATUS_OBJECT_NAME_NOT_FOUND (0x%lx)\n", status);
        } else {
            printf("FAIL: expected 0x%lx, got 0x%lx\n",
                   (long)NTB0_ST_OBJECT_NAME_NOT_FOUND, status);
            failures++;
        }

        /* 7d) قرص غير موجود → DEVICE_NOT_CONNECTED */
        handle = 0;
        status = ntb0_test_create_file(
            stub52, "\\??\\Z:\\whatever.txt", NTB0_GENERIC_READ,
            NTB0_FILE_SHARE_READ, NTB0_FILE_OPEN, NTB0_FILE_NON_DIRECTORY_FILE,
            &handle, &iosb);
        if (status == NTB0_ST_DEVICE_NOT_CONNECTED) {
            printf("PASS: unmounted drive -> "
                   "STATUS_DEVICE_NOT_CONNECTED (0x%lx)\n", status);
        } else {
            printf("FAIL: expected 0x%lx, got 0x%lx\n",
                   (long)NTB0_ST_DEVICE_NOT_CONNECTED, status);
            failures++;
        }

        unlink(tmp_abs);
    }
    ntb0_set_expect_fail(0);

    /* 8) جدول ترجمة المسار */
    printf("\n-- path translation --\n");
    failures += ntb0_test_path("\\??\\C:\\Windows\\System32\\ntdll.dll",
                               "Windows/System32/ntdll.dll");
    failures += ntb0_test_path(
        "\\Device\\HarddiskVolume1\\Windows\\System32\\ntdll.dll",
        "Windows/System32/ntdll.dll");
    failures += ntb0_test_path("\\Windows\\System32\\ntdll.dll",
                               "Windows/System32/ntdll.dll");
    failures += ntb0_test_path("C:\\Windows\\System32\\ntdll.dll",
                               "Windows/System32/ntdll.dll");
    failures += ntb0_test_path("\\??\\C:\\Windows\\System32\\",
                               "Windows/System32");
    failures += ntb0_test_path_reject(
        "\\??\\C:\\Windows\\..\\..\\etc\\passwd", "path traversal");
    failures += ntb0_test_path_reject("\\Registry\\Machine\\SYSTEM",
                                      "registry path");
    failures += ntb0_test_path_reject("\\Device\\Mup\\srv\\share\\f.txt",
                                      "UNC path");

    /* 9) SYS-006 — NtOpenFile (0x30) */
    printf("\n-- SYS-006: NtOpenFile (0x30) --\n");
    {
        uint64_t h_ntdll = 0;   /* يُستخدم في قسم القراءة */
        uint64_t h_dir = 0;
        ntb0_io_status_block iosb;

        memset(&iosb, 0, sizeof(iosb));

        /* 9a) فتح ملف قائم → SUCCESS */
        status = ntb0_test_open_file(
            stub30, "\\??\\C:\\Windows\\System32\\ntdll.dll",
            NTB0_GENERIC_READ | NTB0_FILE_READ_ATTRIBUTES,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE |
                NTB0_FILE_SHARE_DELETE,
            NTB0_FILE_OPEN,
            NTB0_FILE_NON_DIRECTORY_FILE | NTB0_FILE_SYNCHRONOUS_IO_NONALERT,
            &h_ntdll, &iosb);
        if (status == NTB0_ST_SUCCESS && h_ntdll != 0 && iosb.status == 0) {
            printf("PASS: NtOpenFile returned STATUS_SUCCESS, handle=%llu\n",
                   (unsigned long long)h_ntdll);
        } else {
            printf("FAIL: NtOpenFile returned 0x%lx (handle=%llu iosb=0x%x)\n",
                   status, (unsigned long long)h_ntdll, iosb.status);
            failures++;
        }

        /* 9b) فتح مجلد كملف → FILE_IS_A_DIRECTORY (فشل متوقّع) */
        ntb0_set_expect_fail(1);
        {
            uint64_t h = 0;

            memset(&iosb, 0, sizeof(iosb));
            status = ntb0_test_open_file(
                stub30, "\\??\\C:\\Windows\\System32",
                NTB0_GENERIC_READ, NTB0_FILE_SHARE_READ, NTB0_FILE_OPEN,
                NTB0_FILE_NON_DIRECTORY_FILE, &h, &iosb);
            if (status == NTB0_ST_FILE_IS_A_DIRECTORY) {
                printf("PASS: NtOpenFile on a directory -> "
                       "STATUS_FILE_IS_A_DIRECTORY (0x%lx)\n", status);
            } else {
                printf("FAIL: expected 0x%lx, got 0x%lx\n",
                       (long)NTB0_ST_FILE_IS_A_DIRECTORY, status);
                failures++;
            }
        }

        /* 9c) فتح مجلد كـ FILE_DIRECTORY_FILE → SUCCESS */
        {
            uint64_t h = 0;

            memset(&iosb, 0, sizeof(iosb));
            status = ntb0_test_open_file(
                stub30, "\\??\\C:\\Windows\\System32", NTB0_GENERIC_READ,
                NTB0_FILE_SHARE_READ, NTB0_FILE_OPEN,
                NTB0_FILE_DIRECTORY_FILE, &h, &iosb);
            if (status == NTB0_ST_SUCCESS && h != 0) {
                printf("PASS: NtOpenFile on a directory (FILE_DIRECTORY_FILE) "
                       "-> handle=%llu\n", (unsigned long long)h);
                h_dir = h;
            } else {
                printf("FAIL: NtOpenFile on directory returned 0x%lx\n", status);
                failures++;
            }
        }

        /* 9d) فتح ملف غير موجود → OBJECT_NAME_NOT_FOUND (فشل متوقّع) */
        {
            uint64_t h = 0;

            memset(&iosb, 0, sizeof(iosb));
            status = ntb0_test_open_file(
                stub30, "\\??\\C:\\ntb0_missing_open.tmp", NTB0_GENERIC_READ,
                NTB0_FILE_SHARE_READ, NTB0_FILE_OPEN,
                NTB0_FILE_NON_DIRECTORY_FILE, &h, &iosb);
            if (status == NTB0_ST_OBJECT_NAME_NOT_FOUND) {
                printf("PASS: NtOpenFile on missing file -> "
                       "STATUS_OBJECT_NAME_NOT_FOUND (0x%lx)\n", status);
            } else {
                printf("FAIL: expected 0x%lx, got 0x%lx\n",
                       (long)NTB0_ST_OBJECT_NAME_NOT_FOUND, status);
                failures++;
            }
        }
        ntb0_set_expect_fail(0);

        if (h_dir) {
            status = ntb0_call_win64(stub, (long)h_dir, 0, 0, 0, 0, 0, 0, 0, 0,
                                     0, 0);
            if (status != 0) {
                printf("FAIL: NtClose on the directory handle failed\n");
                failures++;
            }
        }

        /* 10) SYS-003 — NtReadFile (0x03) على المقبض المفتوح أعلاه */
        printf("\n-- SYS-003: NtReadFile (0x03) --\n");
        if (h_ntdll == 0) {
            printf("FAIL: no handle from NtOpenFile — read tests skipped\n");
            failures++;
        } else {
            char disk[16], via[16];
            char diskpath[512];
            int64_t off;
            long fsize;

            snprintf(diskpath, sizeof(diskpath), "%s/Windows/System32/ntdll.dll",
                     root);
            fsize = ntb0_disk_size(diskpath);

            /* 10a) قراءة عند إزاحة صفرية → MZ (ترويسة PE) */
            memset(&iosb, 0, sizeof(iosb));
            off = 0;
            memset(via, 0, sizeof(via));
            status = ntb0_test_io(stub03, h_ntdll, via, 2, &off, &iosb);
            if (status == NTB0_ST_SUCCESS && via[0] == 'M' && via[1] == 'Z' &&
                iosb.information == 2) {
                printf("PASS: NtReadFile at offset 0 -> MZ "
                       "(Information=%llu)\n",
                       (unsigned long long)iosb.information);
            } else {
                printf("FAIL: NtReadFile -> 0x%lx info=%llu bytes=%02x %02x\n",
                       status, (unsigned long long)iosb.information,
                       (unsigned char)via[0], (unsigned char)via[1]);
                failures++;
            }

            /* 10b) مقارنة 16 بايت عند إزاحة e_lfanew مع الملف على القرص */
            off = 0x3C;
            memset(via, 0, sizeof(via));
            status = ntb0_test_io(stub03, h_ntdll, via, 16, &off, &iosb);
            memset(disk, 0, sizeof(disk));
            if (status == NTB0_ST_SUCCESS &&
                ntb0_disk_read(diskpath, 0x3C, disk, 16) == 0 &&
                memcmp(via, disk, 16) == 0) {
                printf("PASS: NtReadFile bytes identical to the file on disk "
                       "(offset 0x3C, 16 bytes)\n");
            } else {
                printf("FAIL: NtReadFile != disk (status=0x%lx)\n", status);
                failures++;
            }

            /* 10c) قراءة متتابعة (ByteOffset = NULL) — الموضع يتقدّم */
            {
                unsigned char s1[2], s2[2];

                memset(&iosb, 0, sizeof(iosb));
                status = ntb0_test_io(stub03, h_ntdll, s1, 2, NULL, &iosb);
                if (status != NTB0_ST_SUCCESS) {
                    printf("FAIL: sequential read #1 -> 0x%lx\n", status);
                    failures++;
                }
                status = ntb0_test_io(stub03, h_ntdll, s2, 2, NULL, &iosb);
                memset(disk, 0, sizeof(disk));
                if (status == NTB0_ST_SUCCESS &&
                    ntb0_disk_read(diskpath, 0, disk, 4) == 0 &&
                    memcmp(s1, disk, 2) == 0 && memcmp(s2, disk + 2, 2) == 0) {
                    printf("PASS: sequential NtReadFile (NULL offset) advanced "
                           "the position correctly\n");
                } else {
                    printf("FAIL: sequential read mismatch (0x%lx)\n", status);
                    failures++;
                }
            }

            /* 10d) القراءة بعد نهاية الملف → END_OF_FILE (فشل متوقّع) */
            ntb0_set_expect_fail(1);
            {
                unsigned char eof[4];

                memset(&iosb, 0, sizeof(iosb));
                off = fsize;
                status = ntb0_test_io(stub03, h_ntdll, eof, 4, &off, &iosb);
                if (status == NTB0_ST_END_OF_FILE) {
                    printf("PASS: read past EOF -> STATUS_END_OF_FILE (0x%lx)\n",
                           status);
                } else {
                    printf("FAIL: expected 0x%lx, got 0x%lx\n",
                           (long)NTB0_ST_END_OF_FILE, status);
                    failures++;
                }
            }

            /* 10e) مقبض غير صالح → INVALID_HANDLE (فشل متوقّع) */
            {
                unsigned char junk[4];

                memset(&iosb, 0, sizeof(iosb));
                status = ntb0_test_io(stub03, 9999, junk, 4, NULL, &iosb);
                if (status == NTB0_ST_INVALID_HANDLE) {
                    printf("PASS: read on a bogus handle -> "
                           "STATUS_INVALID_HANDLE (0x%lx)\n", status);
                } else {
                    printf("FAIL: expected 0x%lx, got 0x%lx\n",
                           (long)NTB0_ST_INVALID_HANDLE, status);
                    failures++;
                }
            }
            ntb0_set_expect_fail(0);

            /* إغلاق المقبض */
            status = ntb0_call_win64(stub, (long)h_ntdll, 0, 0, 0, 0, 0, 0, 0,
                                     0, 0, 0);
            if (status != 0) {
                printf("FAIL: NtClose on the NtOpenFile handle failed\n");
                failures++;
            }
        }
    }

    /* 11) SYS-004 — NtWriteFile (0x05) */
    printf("\n-- SYS-004: NtWriteFile (0x05) --\n");
    {
        uint64_t h = 0;
        ntb0_io_status_block iosb;
        const char payload[] = "NTBOX-WRITE-PROOF-0123456789";
        const char payload2[] = "SECOND-BLOCK";
        char wpath[512], rpath[512];
        unsigned char back[64];
        int64_t off;

        memset(&iosb, 0, sizeof(iosb));

        snprintf(wpath, sizeof(wpath), "%s/ntb0_test_write.tmp", root);
        snprintf(rpath, sizeof(rpath), "\\??\\C:\\ntb0_test_write.tmp");

        /* 11a) إنشاء الملف (FILE_CREATE) */
        status = ntb0_test_create_file(
            stub52, rpath, NTB0_GENERIC_READ | NTB0_GENERIC_WRITE,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE, NTB0_FILE_CREATE,
            NTB0_FILE_NON_DIRECTORY_FILE | NTB0_FILE_SYNCHRONOUS_IO_NONALERT,
            &h, &iosb);
        if (status != NTB0_ST_SUCCESS || h == 0) {
            printf("FAIL: create for write returned 0x%lx\n", status);
            failures++;
        } else {
            /* 11b) كتابة عند الموضع الحالي */
            status = ntb0_test_io(stub05, h, (void *)payload,
                                  (uint32_t)strlen(payload), NULL, &iosb);
            if (status == NTB0_ST_SUCCESS &&
                iosb.information == strlen(payload)) {
                printf("PASS: NtWriteFile wrote %llu bytes\n",
                       (unsigned long long)iosb.information);
            } else {
                printf("FAIL: NtWriteFile -> 0x%lx info=%llu\n", status,
                       (unsigned long long)iosb.information);
                failures++;
            }

            /* 11c) كتابة عند إزاحة صريحة */
            off = 64;
            status = ntb0_test_io(stub05, h, (void *)payload2,
                                  (uint32_t)strlen(payload2), &off, &iosb);
            if (status == NTB0_ST_SUCCESS &&
                iosb.information == strlen(payload2)) {
                printf("PASS: NtWriteFile at offset 64 wrote %llu bytes\n",
                       (unsigned long long)iosb.information);
            } else {
                printf("FAIL: NtWriteFile at offset -> 0x%lx\n", status);
                failures++;
            }

            /* 11d) قراءة ما كُتب عبر NtReadFile */
            memset(&iosb, 0, sizeof(iosb));
            off = 0;
            memset(back, 0, sizeof(back));
            status = ntb0_test_io(stub03, h, back, (uint32_t)strlen(payload),
                                  &off, &iosb);
            if (status == NTB0_ST_SUCCESS &&
                memcmp(back, payload, strlen(payload)) == 0) {
                printf("PASS: read-back matches what was written "
                       "(round-trip)\n");
            } else {
                printf("FAIL: round-trip mismatch (0x%lx)\n", status);
                failures++;
            }

            /* 11e) إثبات أن الكتابة landed في الملف الحقيقي على القرص */
            {
                char disk[32], disk2[16];

                memset(disk, 0, sizeof(disk));
                memset(disk2, 0, sizeof(disk2));
                if (ntb0_disk_read(wpath, 0, disk, strlen(payload)) == 0 &&
                    ntb0_disk_read(wpath, 64, disk2, strlen(payload2)) == 0 &&
                    memcmp(disk, payload, strlen(payload)) == 0 &&
                    memcmp(disk2, payload2, strlen(payload2)) == 0) {
                    printf("PASS: the bytes are in the real file on disk "
                           "(in place)\n");
                } else {
                    printf("FAIL: disk content mismatch after NtWriteFile\n");
                    failures++;
                }
            }

            /* 11f) كتابة على مقبض للقراءة فقط → ACCESS_DENIED (فشل متوقّع) */
            ntb0_set_expect_fail(1);
            {
                uint64_t ro = 0;
                uint64_t h2 = 0;

                memset(&iosb, 0, sizeof(iosb));
                status = ntb0_test_open_file(
                    stub30, "\\??\\C:\\Windows\\System32\\ntdll.dll",
                    NTB0_GENERIC_READ, NTB0_FILE_SHARE_READ, NTB0_FILE_OPEN,
                    NTB0_FILE_NON_DIRECTORY_FILE, &ro, &iosb);
                if (status == NTB0_ST_SUCCESS && ro != 0) {
                    status = ntb0_test_io(stub05, ro, (void *)payload, 4, NULL,
                                          &iosb);
                    if (status == NTB0_ST_ACCESS_DENIED) {
                        printf("PASS: write on a read-only handle -> "
                               "STATUS_ACCESS_DENIED (0x%lx)\n", status);
                    } else {
                        printf("FAIL: expected 0x%lx, got 0x%lx\n",
                               (long)NTB0_ST_ACCESS_DENIED, status);
                        failures++;
                    }
                    ntb0_call_win64(stub, (long)ro, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                    0);
                } else {
                    printf("FAIL: cannot open the read-only handle (0x%lx)\n",
                           status);
                    failures++;
                }
                (void)h2;
            }
            ntb0_set_expect_fail(0);

            /* إغلاق + حذف الملف المؤقت */
            status = ntb0_call_win64(stub, (long)h, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                     0);
            if (status != 0) {
                printf("FAIL: NtClose after NtWriteFile failed\n");
                failures++;
            }
            unlink(wpath);
        }
    }

    /* 13) SYS-007 — جدول المقابض: المقبض ليس fd (T-023) */
    printf("\n-- SYS-007: handle table (handle != fd) --\n");
    {
        uint64_t h1 = 0, h2 = 0;
        ntb0_io_status_block iosb;
        int fd1 = -1, fd2 = -1;
        unsigned char magic[2] = { 0, 0 };
        int64_t off = 0;

        memset(&iosb, 0, sizeof(iosb));

        status = ntb0_test_open_file(
            stub30, "\\??\\C:\\Windows\\System32\\ntdll.dll",
            NTB0_GENERIC_READ,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE | NTB0_FILE_SHARE_DELETE,
            NTB0_FILE_OPEN, NTB0_FILE_NON_DIRECTORY_FILE, &h1, &iosb);
        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: first open failed (0x%lx)\n", status);
            failures++;
        }

        status = ntb0_test_open_file(
            stub30, "\\??\\C:\\Windows\\System32\\ntdll.dll",
            NTB0_GENERIC_READ,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE | NTB0_FILE_SHARE_DELETE,
            NTB0_FILE_OPEN, NTB0_FILE_NON_DIRECTORY_FILE, &h2, &iosb);
        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: second open failed (0x%lx)\n", status);
            failures++;
        }

        if (h1 && h2 && h1 != h2) {
            printf("PASS: two opens of the same file -> two different "
                   "handles (0x%llx / 0x%llx)\n",
                   (unsigned long long)h1, (unsigned long long)h2);
        } else {
            printf("FAIL: expected two different handles, got 0x%llx / 0x%llx\n",
                   (unsigned long long)h1, (unsigned long long)h2);
            failures++;
        }

        fd1 = ntb0_handle_fd(h1);
        fd2 = ntb0_handle_fd(h2);

        if (fd1 >= 0 && fd2 >= 0 && (uint64_t)fd1 != h1 &&
            (uint64_t)fd2 != h2) {
            printf("PASS: the handle is NOT the fd (handle 0x%llx -> fd %d, "
                   "handle 0x%llx -> fd %d)\n",
                   (unsigned long long)h1, fd1, (unsigned long long)h2, fd2);
        } else {
            printf("FAIL: handle/fd identity broken (fd1=%d fd2=%d)\n", fd1,
                   fd2);
            failures++;
        }

        /* إغلاق أحدهما لا يُغلق الآخر */
        status = ntb0_call_win64(stub, (long)h1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: NtClose(h1) failed (0x%lx)\n", status);
            failures++;
        } else if (ntb0_handle_fd(h1) != -1) {
            printf("FAIL: handle still valid after NtClose\n");
            failures++;
        } else {
            printf("PASS: closed handle is invalidated immediately\n");
        }

        memset(&iosb, 0, sizeof(iosb));
        status = ntb0_test_io(stub03, h2, magic, 2, &off, &iosb);
        if (status == NTB0_ST_SUCCESS && magic[0] == 'M' && magic[1] == 'Z') {
            printf("PASS: the second handle still works after closing the "
                   "first (MZ)\n");
        } else {
            printf("FAIL: second handle broke after closing the first "
                   "(0x%lx)\n", status);
            failures++;
        }

        ntb0_call_win64(stub, (long)h2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    }

    /* 14) T-024 — ShareAccess: تعارض المشاركة */
    printf("\n-- T-024: ShareAccess -> STATUS_SHARING_VIOLATION --\n");
    {
        uint64_t ha = 0, hb = 0;
        ntb0_io_status_block iosb;
        char spath[512];

        snprintf(spath, sizeof(spath), "%s/ntb0_share.tmp", root);
        unlink(spath);

        memset(&iosb, 0, sizeof(iosb));

        /* القائم: قراءة + كتابة، مشاركة = قراءة|كتابة */
        status = ntb0_test_create_file(
            stub52, "\\??\\C:\\ntb0_share.tmp",
            NTB0_GENERIC_READ | NTB0_GENERIC_WRITE,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE, NTB0_FILE_CREATE,
            NTB0_FILE_NON_DIRECTORY_FILE, &ha, &iosb);
        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: cannot create the share-test file (0x%lx)\n", status);
            failures++;
        } else {
            /* الجديد: كتابة، مشاركة = قراءة فقط ⇒ تعارض */
            ntb0_set_expect_fail(1);
            status = ntb0_test_open_file(
                stub30, "\\??\\C:\\ntb0_share.tmp", NTB0_GENERIC_WRITE,
                NTB0_FILE_SHARE_READ, NTB0_FILE_OPEN,
                NTB0_FILE_NON_DIRECTORY_FILE, &hb, &iosb);
            ntb0_set_expect_fail(0);

            if (status == NTB0_ST_SHARING_VIOLATION) {
                printf("PASS: conflicting share mode -> "
                       "STATUS_SHARING_VIOLATION (0x%lx)\n", status);
            } else {
                printf("FAIL: expected 0x%lx, got 0x%lx\n",
                       (long)NTB0_ST_SHARING_VIOLATION, status);
                failures++;
            }

            /* الجديد: مشاركة كاملة ⇒ مسموح */
            status = ntb0_test_open_file(
                stub30, "\\??\\C:\\ntb0_share.tmp", NTB0_GENERIC_WRITE,
                NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE, NTB0_FILE_OPEN,
                NTB0_FILE_NON_DIRECTORY_FILE, &hb, &iosb);
            if (status == NTB0_ST_SUCCESS && hb) {
                printf("PASS: compatible share mode -> opened (0x%llx)\n",
                       (unsigned long long)hb);
                ntb0_call_win64(stub, (long)hb, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            } else {
                printf("FAIL: compatible share mode rejected (0x%lx)\n", status);
                failures++;
            }

            ntb0_call_win64(stub, (long)ha, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            unlink(spath);
        }
    }

    /* 15) T-025 — RootDirectory: مسار نسبي داخل مجلد مفتوح */
    printf("\n-- T-025: RootDirectory (relative open) --\n");
    {
        uint64_t hdir = 0, hrel = 0;
        ntb0_io_status_block iosb;
        unsigned char magic[2] = { 0, 0 };
        int64_t off = 0;

        memset(&iosb, 0, sizeof(iosb));

        status = ntb0_test_open_file(
            stub30, "\\??\\C:\\Windows\\System32", NTB0_GENERIC_READ,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE | NTB0_FILE_SHARE_DELETE,
            NTB0_FILE_OPEN, NTB0_FILE_DIRECTORY_FILE, &hdir, &iosb);
        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: cannot open the directory handle (0x%lx)\n", status);
            failures++;
        } else {
            status = ntb0_test_open_rel(
                stub30, "ntdll.dll", hdir, NTB0_GENERIC_READ,
                NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE | NTB0_FILE_SHARE_DELETE,
                NTB0_FILE_OPEN, NTB0_FILE_NON_DIRECTORY_FILE, &hrel, &iosb);
            if (status != NTB0_ST_SUCCESS) {
                printf("FAIL: relative open through RootDirectory failed "
                       "(0x%lx)\n", status);
                failures++;
            } else {
                memset(&iosb, 0, sizeof(iosb));
                status = ntb0_test_io(stub03, hrel, magic, 2, &off, &iosb);
                if (status == NTB0_ST_SUCCESS && magic[0] == 'M' &&
                    magic[1] == 'Z') {
                    printf("PASS: relative open via RootDirectory -> the same "
                           "ntdll.dll (MZ)\n");
                } else {
                    printf("FAIL: wrong content through RootDirectory "
                           "(0x%lx)\n", status);
                    failures++;
                }
                ntb0_call_win64(stub, (long)hrel, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                0);
            }

            /* مسار نسبي مطلق → رفض */
            ntb0_set_expect_fail(1);
            status = ntb0_test_open_rel(
                stub30, "\\??\\C:\\Windows\\System32\\ntdll.dll", hdir,
                NTB0_GENERIC_READ, NTB0_FILE_SHARE_READ, NTB0_FILE_OPEN,
                NTB0_FILE_NON_DIRECTORY_FILE, &hrel, &iosb);
            ntb0_set_expect_fail(0);
            if (status == NTB0_ST_OBJECT_PATH_SYNTAX_BAD) {
                printf("PASS: absolute path with RootDirectory -> "
                       "STATUS_OBJECT_PATH_SYNTAX_BAD (0x%lx)\n", status);
            } else {
                printf("FAIL: expected 0x%lx, got 0x%lx\n",
                       (long)NTB0_ST_OBJECT_PATH_SYNTAX_BAD, status);
                failures++;
            }

            /* RootDirectory غير صالح → INVALID_HANDLE */
            ntb0_set_expect_fail(1);
            status = ntb0_test_open_rel(
                stub30, "ntdll.dll", 0xDEAD, NTB0_GENERIC_READ,
                NTB0_FILE_SHARE_READ, NTB0_FILE_OPEN,
                NTB0_FILE_NON_DIRECTORY_FILE, &hrel, &iosb);
            ntb0_set_expect_fail(0);
            if (status == NTB0_ST_INVALID_HANDLE) {
                printf("PASS: invalid RootDirectory -> "
                       "STATUS_INVALID_HANDLE (0x%lx)\n", status);
            } else {
                printf("FAIL: expected 0x%lx, got 0x%lx\n",
                       (long)NTB0_ST_INVALID_HANDLE, status);
                failures++;
            }

            ntb0_call_win64(stub, (long)hdir, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
    }

    /* 16) SYS-008 — NtDuplicateObject */
    printf("\n-- SYS-008: NtDuplicateObject (0x39) --\n");
    {
        uint64_t hs = 0, hd = 0;
        ntb0_io_status_block iosb;
        unsigned char m1[2] = { 0, 0 }, m2[2] = { 0, 0 };
        int64_t off = 0;

        memset(&iosb, 0, sizeof(iosb));

        status = ntb0_test_open_file(
            stub30, "\\??\\C:\\Windows\\System32\\ntdll.dll",
            NTB0_GENERIC_READ,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE | NTB0_FILE_SHARE_DELETE,
            NTB0_FILE_OPEN, NTB0_FILE_NON_DIRECTORY_FILE, &hs, &iosb);
        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: cannot open the source file (0x%lx)\n", status);
            failures++;
        } else {
            status = ntb0_test_duplicate(stub39, hs, 0, 0, &hd);
            if (status != NTB0_ST_SUCCESS || !hd) {
                printf("FAIL: NtDuplicateObject failed (0x%lx)\n", status);
                failures++;
            } else if (hd == hs) {
                printf("FAIL: the duplicate must be a new handle\n");
                failures++;
            } else {
                printf("PASS: duplicate -> a new handle (0x%llx -> 0x%llx)\n",
                       (unsigned long long)hs, (unsigned long long)hd);

                memset(&iosb, 0, sizeof(iosb));
                status = ntb0_test_io(stub03, hs, m1, 2, &off, &iosb);
                memset(&iosb, 0, sizeof(iosb));
                status = ntb0_test_io(stub03, hd, m2, 2, &off, &iosb);
                if (m1[0] == 'M' && m1[1] == 'Z' && m2[0] == 'M' &&
                    m2[1] == 'Z') {
                    printf("PASS: both handles read the same file (MZ/MZ)\n");
                } else {
                    printf("FAIL: the duplicate does not read the same file\n");
                    failures++;
                }

                /* إغلاق المصدر لا يُغلق النسخة */
                ntb0_call_win64(stub, (long)hs, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
                hs = 0;
                memset(&iosb, 0, sizeof(iosb));
                status = ntb0_test_io(stub03, hd, m2, 2, &off, &iosb);
                if (status == NTB0_ST_SUCCESS && m2[0] == 'M') {
                    printf("PASS: the duplicate survives closing the source\n");
                } else {
                    printf("FAIL: the duplicate died with the source "
                           "(0x%lx)\n", status);
                    failures++;
                }
            }

            if (hd)
                ntb0_call_win64(stub, (long)hd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            if (hs)
                ntb0_call_win64(stub, (long)hs, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
    }

    /* 17) SYS-009 / SYS-010 / SYS-011 — إدارة الذاكرة */
    printf("\n-- SYS-009/010/011: virtual memory --\n");
    {
        uint64_t base = 0, size = 0x10000;
        uint32_t old_prot = 0;
        unsigned char info[0x30];
        uint64_t qsize = 0x30;

        memset(info, 0, sizeof(info));

        status = ntb0_test_alloc_vm(stub15, &base, &size, 0x04 /*PAGE_READWRITE*/);
        if (status != NTB0_ST_SUCCESS || !base) {
            printf("FAIL: NtAllocateVirtualMemory failed (0x%lx)\n", status);
            failures++;
        } else {
            printf("PASS: NtAllocateVirtualMemory -> 0x%llx (0x%llx bytes)\n",
                   (unsigned long long)base, (unsigned long long)size);

            /* SYS-011: استعلام عن المنطقة */
            status = ntb0_test_query_vm(stub20, base, info, &qsize);
            if (status != NTB0_ST_SUCCESS) {
                printf("FAIL: NtQueryVirtualMemory failed (0x%lx)\n", status);
                failures++;
            } else {
                uint64_t *p_base = (uint64_t *)(info + 0x00);
                uint64_t *p_size = (uint64_t *)(info + 0x18);
                uint32_t *p_state = (uint32_t *)(info + 0x20);
                uint32_t *p_prot = (uint32_t *)(info + 0x24);
                uint32_t *p_type = (uint32_t *)(info + 0x28);

                if (*p_base == base && *p_size >= 0x10000 &&
                    *p_state == 0x1000 /*MEM_COMMIT*/ &&
                    *p_type == 0x20000 /*MEM_PRIVATE*/) {
                    printf("PASS: MemoryBasicInformation (base=0x%llx size=0x%llx "
                           "state=0x%x prot=0x%x type=0x%x)\n",
                           (unsigned long long)*p_base,
                           (unsigned long long)*p_size, *p_state, *p_prot,
                           *p_type);
                } else {
                    printf("FAIL: wrong MEMORY_BASIC_INFORMATION "
                           "(base=0x%llx size=0x%llx state=0x%x type=0x%x)\n",
                           (unsigned long long)*p_base,
                           (unsigned long long)*p_size, *p_state, *p_type);
                    failures++;
                }
            }

            /* SYS-010: تغيير الحماية */
            status = ntb0_test_protect_vm(stub4d, &base, &size,
                                          0x02 /*PAGE_READONLY*/, &old_prot);
            if (status == NTB0_ST_SUCCESS && old_prot == 0x04) {
                printf("PASS: NtProtectVirtualMemory -> PAGE_READONLY "
                       "(old=0x%x)\n", old_prot);
            } else {
                printf("FAIL: NtProtectVirtualMemory failed (0x%lx, old=0x%x)\n",
                       status, old_prot);
                failures++;
            }

            /* SYS-009: تحرير الذاكرة */
            status = ntb0_test_free_vm(stub1b, &base, &size);
            if (status == NTB0_ST_SUCCESS && base == 0 && size == 0) {
                printf("PASS: NtFreeVirtualMemory -> base=0 size=0\n");
            } else {
                printf("FAIL: NtFreeVirtualMemory failed (0x%lx base=0x%llx "
                       "size=0x%llx)\n", status, (unsigned long long)base,
                       (unsigned long long)size);
                failures++;
            }

            /* بعد التحرير: الاستعلام يجب أن يفشل */
            ntb0_set_expect_fail(1);
            qsize = 0x30;
            status = ntb0_test_query_vm(stub20, 0x1000, info, &qsize);
            ntb0_set_expect_fail(0);
            if (status != NTB0_ST_SUCCESS) {
                printf("PASS: query on a freed region -> 0x%lx\n", status);
            } else {
                printf("FAIL: query on a freed region succeeded\n");
                failures++;
            }
        }
    }

    /* 18) SYS-012 — NtQueryInformationFile */
    printf("\n-- SYS-012: NtQueryInformationFile (0x0e) --\n");
    {
        uint64_t hq = 0;
        ntb0_io_status_block iosb;
        unsigned char info[0x18];

        memset(&iosb, 0, sizeof(iosb));
        memset(info, 0, sizeof(info));

        status = ntb0_test_open_file(
            stub30, "\\??\\C:\\Windows\\System32\\ntdll.dll",
            NTB0_GENERIC_READ,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE | NTB0_FILE_SHARE_DELETE,
            NTB0_FILE_OPEN, NTB0_FILE_NON_DIRECTORY_FILE, &hq, &iosb);
        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: cannot open the file for query (0x%lx)\n", status);
            failures++;
        } else {
            status = ntb0_test_query_file(stub0e, hq, info,
                                          1 /*FileStandardInformation*/,
                                          0x18);
            if (status == NTB0_ST_SUCCESS) {
                uint64_t *eof = (uint64_t *)(info + 0x08);
                uint32_t *links = (uint32_t *)(info + 0x10);
                uint8_t *dirp = (uint8_t *)(info + 0x15);

                if (*eof > 0 && *links >= 1 && *dirp == 0) {
                    printf("PASS: FileStandardInformation (eof=0x%llx links=%u "
                           "dir=%u)\n", (unsigned long long)*eof, *links,
                           *dirp);
                } else {
                    printf("FAIL: wrong FileStandardInformation "
                           "(eof=0x%llx links=%u dir=%u)\n",
                           (unsigned long long)*eof, *links, *dirp);
                    failures++;
                }
            } else {
                printf("FAIL: NtQueryInformationFile failed (0x%lx)\n", status);
                failures++;
            }

            /* FilePositionInformation */
            memset(info, 0, sizeof(info));
            status = ntb0_test_query_file(stub0e, hq, info,
                                          2 /*FilePositionInformation*/, 0x10);
            if (status == NTB0_ST_SUCCESS) {
                printf("PASS: FilePositionInformation (pos=0x%llx)\n",
                       (unsigned long long)*(uint64_t *)(info + 0x00));
            } else {
                printf("FAIL: FilePositionInformation failed (0x%lx)\n",
                       status);
                failures++;
            }

            /* صنف غير مدعوم → NOT_SUPPORTED (فشل متوقّع) */
            ntb0_set_expect_fail(1);
            status = ntb0_test_query_file(stub0e, hq, info, 99, 0x10);
            ntb0_set_expect_fail(0);
            if (status == NTB0_ST_NOT_SUPPORTED) {
                printf("PASS: unsupported information class -> "
                       "STATUS_NOT_SUPPORTED (0x%lx)\n", status);
            } else {
                printf("FAIL: expected 0x%lx, got 0x%lx\n",
                       (long)NTB0_ST_NOT_SUPPORTED, status);
                failures++;
            }

            ntb0_call_win64(stub, (long)hq, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
    }

    /* 19) SYS-013 — NtSetInformationFile (truncate / position / rename) */
    printf("\n-- SYS-013: NtSetInformationFile (0x24) --\n");
    {
        uint64_t h = 0;
        ntb0_io_status_block iosb;
        char fpath[512];
        int64_t pos = 0;
        unsigned char ren[0x40 + 64];
        const char *newname = "ntb0_renamed.tmp";

        snprintf(fpath, sizeof(fpath), "%s/ntb0_setinfo.tmp", root);
        unlink(fpath);
        memset(&iosb, 0, sizeof(iosb));

        status = ntb0_test_create_file(
            stub52, "\\??\\C:\\ntb0_setinfo.tmp",
            NTB0_GENERIC_READ | NTB0_GENERIC_WRITE,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE, NTB0_FILE_CREATE,
            NTB0_FILE_NON_DIRECTORY_FILE, &h, &iosb);
        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: cannot create the temp file (0x%lx)\n", status);
            failures++;
        } else {
            /* كتابة 100 بايت ثم قطع الملف عند 50 */
            memset(&iosb, 0, sizeof(iosb));
            status = ntb0_test_io(stub05, h, (void *)"0123456789ABCDEF"
                                                "0123456789ABCDEF"
                                                "0123456789ABCDEF"
                                                "0123456789ABCDEF"
                                                "0123456789ABCDEF"
                                                "0123456789ABCDEF",
                                  100, NULL, &iosb);
            if (status != NTB0_ST_SUCCESS) {
                printf("FAIL: cannot write 100 bytes (0x%lx)\n", status);
                failures++;
            }

            /* الصنف 5: FileEndOfFileInformation ⇒ ftruncate */
            pos = 50;
            status = ntb0_test_set_info_file(stub24, h, &pos, 5, 8);
            if (status == NTB0_ST_SUCCESS) {
                struct stat st;

                if (stat(fpath, &st) == 0 && st.st_size == 50) {
                    printf("PASS: FileEndOfFileInformation -> the file is "
                           "50 bytes on disk\n");
                } else {
                    printf("FAIL: truncate did not reach the disk\n");
                    failures++;
                }
            } else {
                printf("FAIL: FileEndOfFileInformation failed (0x%lx)\n",
                       status);
                failures++;
            }

            /* الصنف 2: FilePositionInformation ⇒ lseek */
            pos = 10;
            status = ntb0_test_set_info_file(stub24, h, &pos, 2, 8);
            if (status == NTB0_ST_SUCCESS) {
                printf("PASS: FilePositionInformation -> the position is set\n");
            } else {
                printf("FAIL: FilePositionInformation failed (0x%lx)\n",
                       status);
                failures++;
            }

            /* الصنف 10: FileRenameInformation */
            memset(ren, 0, sizeof(ren));
            ren[0x00] = 0;                    /* ReplaceIfExists = FALSE */
            *(uint32_t *)(ren + 0x0C) = 32;   /* FileNameLength (16 chars) */
            {
                const char *nm = newname;
                size_t i;

                for (i = 0; nm[i]; i++)
                    *(uint16_t *)(ren + 0x10 + i * 2) = (uint16_t)nm[i];
            }
            status = ntb0_test_set_info_file(stub24, h, ren, 10,
                                             (uint32_t)sizeof(ren));
            if (status == NTB0_ST_SUCCESS) {
                char rpath[512];

                snprintf(rpath, sizeof(rpath), "%s/%s", root, newname);
                if (access(rpath, F_OK) == 0) {
                    printf("PASS: FileRenameInformation -> the file is now "
                           "'%s'\n", newname);
                } else {
                    printf("FAIL: the renamed file does not exist\n");
                    failures++;
                }
            } else {
                printf("FAIL: FileRenameInformation failed (0x%lx)\n", status);
                failures++;
            }

            /* الصنف 13: FileDispositionInformation ⇒ حذف (الاسم الجديد) */
            {
                unsigned char del = 1;
                char rpath[512];

                snprintf(rpath, sizeof(rpath), "%s/%s", root, newname);
                status = ntb0_test_set_info_file(stub24, h, &del, 13, 1);
                if (status == NTB0_ST_SUCCESS) {
                    if (access(rpath, F_OK) != 0) {
                        printf("PASS: FileDispositionInformation -> the file "
                               "is deleted (by its new name)\n");
                    } else {
                        printf("FAIL: the file was not deleted\n");
                        failures++;
                    }
                } else {
                    printf("FAIL: FileDispositionInformation failed "
                           "(0x%lx)\n", status);
                    failures++;
                }
            }
            ntb0_call_win64(stub, (long)h, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
        unlink(fpath);
    }

    /* 20) SYS-014 — NtFlushBuffersFile */
    printf("\n-- SYS-014: NtFlushBuffersFile (0x48) --\n");
    {
        uint64_t h = 0;
        ntb0_io_status_block iosb;

        memset(&iosb, 0, sizeof(iosb));
        status = ntb0_test_open_file(
            stub30, "\\??\\C:\\Windows\\System32\\ntdll.dll",
            NTB0_GENERIC_READ,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE | NTB0_FILE_SHARE_DELETE,
            NTB0_FILE_OPEN, NTB0_FILE_NON_DIRECTORY_FILE, &h, &iosb);
        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: cannot open for flush (0x%lx)\n", status);
            failures++;
        } else {
            status = ntb0_test_flush(stub48, h);
            if (status == NTB0_ST_SUCCESS) {
                printf("PASS: NtFlushBuffersFile -> fsync done\n");
            } else {
                printf("FAIL: NtFlushBuffersFile failed (0x%lx)\n", status);
                failures++;
            }
            ntb0_call_win64(stub, (long)h, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
    }

    /* 21) SYS-015 — NtDeviceIoControlFile (ioctl حقيقي) */
    printf("\n-- SYS-015: NtDeviceIoControlFile (0x04) --\n");
    {
        uint64_t h = 0;
        ntb0_io_status_block iosb;
        uint32_t avail = 0;
        uint32_t ioctl_ret = 0;
        char fpath[512];

        snprintf(fpath, sizeof(fpath), "%s/ntb0_ioctl.tmp", root);
        unlink(fpath);
        memset(&iosb, 0, sizeof(iosb));

        status = ntb0_test_create_file(
            stub52, "\\??\\C:\\ntb0_ioctl.tmp",
            NTB0_GENERIC_READ | NTB0_GENERIC_WRITE,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE, NTB0_FILE_CREATE,
            NTB0_FILE_NON_DIRECTORY_FILE, &h, &iosb);
        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: cannot create the ioctl-test file (0x%lx)\n", status);
            failures++;
        } else {
            memset(&iosb, 0, sizeof(iosb));
            ntb0_test_io(stub05, h, (void *)"0123456789", 10, NULL, &iosb);

            /* أعد المؤشّر إلى الصفر (NtSetInformationFile) ثم اقرأ
             * FIONREAD ⇒ عدد البايتات الجاهزة للقراءة */
            {
                int64_t zero = 0;

                ntb0_test_set_info_file(stub24, h, &zero, 2, 8);
            }

            /* FIONREAD (0x541B) ⇒ عدد البايتات الجاهزة للقراءة */
            status = ntb0_test_ioctl(stub04, h, NULL, 0, &avail,
                                     sizeof(avail), 0x541B, &ioctl_ret);
            if (status == NTB0_ST_SUCCESS && avail == 10) {
                printf("PASS: NtDeviceIoControlFile(FIONREAD) -> %u bytes "
                       "available\n", avail);
            } else {
                printf("FAIL: ioctl pass-through failed (0x%lx, avail=%u)\n",
                       status, avail);
                failures++;
            }

            ntb0_call_win64(stub, (long)h, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
        unlink(fpath);
    }

    /* 22) SYS-016..SYS-019 — الأحداث والانتظار */
    printf("\n-- SYS-016/017/018/019: events + wait --\n");
    {
        uint64_t he = 0;

        status = ntb0_test_create_event(stub45, &he, 1 /*Synchronization*/, 0);
        if (status != NTB0_ST_SUCCESS || !he) {
            printf("FAIL: NtCreateEvent failed (0x%lx)\n", status);
            failures++;
        } else {
            /* انتظار على حدث غير مُشير ⇒ انتهاء المهلة (فشل متوقّع) */
            ntb0_set_expect_fail(1);
            status = ntb0_test_wait(stub01, he, -500000 /*50ms*/);
            ntb0_set_expect_fail(0);
            if (status == NTB0_ST_TIMEOUT) {
                printf("PASS: wait on a non-signaled event -> "
                       "STATUS_TIMEOUT (0x%lx)\n", status);
            } else {
                printf("FAIL: expected 0x%lx, got 0x%lx\n",
                       (long)NTB0_ST_TIMEOUT, status);
                failures++;
            }

            /* الإشارة ثم الانتظار ⇒ نجاح */
            status = ntb0_test_set_event(stub0b, he);
            if (status == NTB0_ST_SUCCESS) {
                printf("PASS: NtSetEvent -> signaled\n");
            } else {
                printf("FAIL: NtSetEvent failed (0x%lx)\n", status);
                failures++;
            }

            status = ntb0_test_wait(stub01, he, -1000000 /*100ms*/);
            if (status == NTB0_ST_SUCCESS) {
                printf("PASS: wait after SetEvent -> STATUS_SUCCESS\n");
            } else {
                printf("FAIL: wait after SetEvent failed (0x%lx)\n", status);
                failures++;
            }

            /* التصفير ثم الانتظار ⇒ انتهاء المهلة */
            status = ntb0_test_clear_event(stub3b, he);
            if (status != NTB0_ST_SUCCESS) {
                printf("FAIL: NtClearEvent failed (0x%lx)\n", status);
                failures++;
            }

            ntb0_set_expect_fail(1);
            status = ntb0_test_wait(stub01, he, -500000);
            ntb0_set_expect_fail(0);
            if (status == NTB0_ST_TIMEOUT) {
                printf("PASS: wait after NtClearEvent -> STATUS_TIMEOUT\n");
            } else {
                printf("FAIL: expected 0x%lx, got 0x%lx\n",
                       (long)NTB0_ST_TIMEOUT, status);
                failures++;
            }

            ntb0_call_win64(stub, (long)he, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }

        /* حدث مُشير من الأصل (InitialState = TRUE) */
        {
            uint64_t h2 = 0;

            status = ntb0_test_create_event(stub45, &h2, 0 /*Notification*/,
                                            1);
            if (status == NTB0_ST_SUCCESS) {
                status = ntb0_test_wait(stub01, h2, -1000000);
                if (status == NTB0_ST_SUCCESS) {
                    printf("PASS: an event created signaled is immediately "
                           "waitable\n");
                } else {
                    printf("FAIL: initial-state event not signaled (0x%lx)\n",
                           status);
                    failures++;
                }
                ntb0_call_win64(stub, (long)h2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            } else {
                printf("FAIL: NtCreateEvent(initial) failed (0x%lx)\n",
                       status);
                failures++;
            }
        }
    }

    /* 23) SYS-020 / SYS-021 — التأخير وتسليم المعالج */
    printf("\n-- SYS-020/021: delay + yield --\n");
    {
        struct timespec t0, t1;
        long ms = 0;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        status = ntb0_test_delay(stub31, -500000 /*50ms*/);
        clock_gettime(CLOCK_MONOTONIC, &t1);

        ms = (t1.tv_sec - t0.tv_sec) * 1000 +
             (t1.tv_nsec - t0.tv_nsec) / 1000000;

        if (status == NTB0_ST_SUCCESS && ms >= 40) {
            printf("PASS: NtDelayExecution(50ms) -> really waited %ld ms\n",
                   ms);
        } else {
            printf("FAIL: NtDelayExecution (0x%lx) waited %ld ms\n", status,
                   ms);
            failures++;
        }

        status = ntb0_test_yield(stub43);
        if (status == NTB0_ST_SUCCESS) {
            printf("PASS: NtYieldExecution -> STATUS_SUCCESS\n");
        } else {
            printf("FAIL: NtYieldExecution failed (0x%lx)\n", status);
            failures++;
        }
    }

    /* 24) SYS-022 — NtQuerySystemInformation */
    printf("\n-- SYS-022: NtQuerySystemInformation (0x33) --\n");
    {
        unsigned char basic[0x40];
        uint32_t ret = 0;

        memset(basic, 0, sizeof(basic));

        status = ntb0_test_query_system(stub33, 0, basic, sizeof(basic), &ret);
        if (status == NTB0_ST_SUCCESS) {
            uint32_t page = *(uint32_t *)(basic + 0x08);
            uint32_t npages = *(uint32_t *)(basic + 0x0C);
            uint32_t gran = *(uint32_t *)(basic + 0x18);
            uint8_t ncpu = *(uint8_t *)(basic + 0x38);

            if (page == 4096 && npages > 0 && gran == 65536 && ncpu >= 1) {
                printf("PASS: SystemBasicInformation (page=%u pages=%u "
                       "gran=%u cpus=%u)\n", page, npages, gran, ncpu);
            } else {
                printf("FAIL: wrong SystemBasicInformation "
                       "(page=%u pages=%u gran=%u cpus=%u)\n", page, npages,
                       gran, ncpu);
                failures++;
            }
        } else {
            printf("FAIL: SystemBasicInformation failed (0x%lx)\n", status);
            failures++;
        }

        /* SystemProcessorInformation */
        {
            unsigned char proc[0x20 * 64];
            uint32_t n = 0;

            memset(proc, 0, sizeof(proc));
            status = ntb0_test_query_system(stub33, 1, proc, sizeof(proc), &n);
            if (status == NTB0_ST_SUCCESS && n >= 0x20 &&
                (n % 0x20) == 0) {
                printf("PASS: SystemProcessorInformation (%u entries)\n",
                       n / 0x20);
            } else {
                printf("FAIL: SystemProcessorInformation failed (0x%lx, "
                       "len=%u)\n", status, n);
                failures++;
            }
        }

        /* صنف غير مدعوم ⇒ NOT_SUPPORTED (فشل متوقّع) */
        ntb0_set_expect_fail(1);
        status = ntb0_test_query_system(stub33, 99, basic, sizeof(basic),
                                        &ret);
        ntb0_set_expect_fail(0);
        if (status == NTB0_ST_NOT_SUPPORTED) {
            printf("PASS: unsupported system class -> "
                   "STATUS_NOT_SUPPORTED (0x%lx)\n", status);
        } else {
            printf("FAIL: expected 0x%lx, got 0x%lx\n",
                   (long)NTB0_ST_NOT_SUPPORTED, status);
            failures++;
        }
    }

    /* 25) SYS-023 — NtQueryDirectoryFile */
    printf("\n-- SYS-023: NtQueryDirectoryFile (0x32) --\n");
    {
        uint64_t hdir = 0;
        ntb0_io_status_block iosb;
        unsigned char ent[0x40 + 512];
        uint32_t ret = 0;
        int count = 0, found_ntdll = 0;

        memset(&iosb, 0, sizeof(iosb));
        status = ntb0_test_open_file(
            stub30, "\\??\\C:\\Windows\\System32", NTB0_GENERIC_READ,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE | NTB0_FILE_SHARE_DELETE,
            NTB0_FILE_OPEN, NTB0_FILE_DIRECTORY_FILE, &hdir, &iosb);
        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: cannot open the directory (0x%lx)\n", status);
            failures++;
        } else {
            for (;;) {
                memset(ent, 0, sizeof(ent));
                ret = 0;

                /* نهاية التعداد (STATUS_NO_MORE_FILES) نتيجة صحيحة
                 * لا "فشل" ⇒ تُعلَّم كفشل متوقّع */
                ntb0_set_expect_fail(1);
                status = ntb0_test_query_dir(stub32, hdir, ent, sizeof(ent),
                                             &ret);
                if (status != NTB0_ST_NO_MORE_FILES)
                    ntb0_set_expect_fail(0);

                if (status == NTB0_ST_NO_MORE_FILES)
                    break;
                if (status != NTB0_ST_SUCCESS) {
                    printf("FAIL: NtQueryDirectoryFile failed (0x%lx)\n",
                           status);
                    failures++;
                    break;
                }

                count++;
                if (ntb0_dir_name_is(ent, "ntdll.dll"))
                    found_ntdll = 1;

                if (count > 4000)
                    break; /* حماية من حلقة لا نهائية */
            }

            if (count > 3 && found_ntdll) {
                printf("PASS: enumerated %d entries and found ntdll.dll\n",
                       count);
            } else {
                printf("FAIL: directory enumeration wrong (count=%d, "
                       "ntdll=%d)\n", count, found_ntdll);
                failures++;
            }

            ntb0_call_win64(stub, (long)hdir, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
    }

    /* 26) SYS-024/025/026 — المقاطع (T-034) */
    printf("\n-- SYS-024/025/026: sections (0x47/0x25/0x27) --\n");
    {
        uint64_t hsec = 0, base = 0, vsize = 0x1000;
        unsigned char *view = NULL;
        static const char payload[] = "NTBOX-SECTION-PAYLOAD";

        /* (أ) مقطع مجهول */
        status = ntb0_test_create_section(stub47, &hsec, 0x10000, 0);
        if (status != NTB0_ST_SUCCESS || !hsec) {
            printf("FAIL: NtCreateSection failed (0x%lx)\n", status);
            failures++;
        } else {
            printf("PASS: NtCreateSection (anonymous) -> handle 0x%llx\n",
                   (unsigned long long)hsec);

            status = ntb0_test_map_view(stub25, hsec, &base, &vsize, 0x04);
            if (status == NTB0_ST_SUCCESS && base) {
                printf("PASS: NtMapViewOfSection -> 0x%llx (%llu bytes)\n",
                       (unsigned long long)base,
                       (unsigned long long)vsize);

                /* الكتابة في العرض ثم القراءة منه */
                view = (unsigned char *)(uintptr_t)base;
                memcpy(view, payload, sizeof(payload));
                if (memcmp(view, payload, sizeof(payload)) == 0) {
                    printf("PASS: the view is writable and readable\n");
                } else {
                    printf("FAIL: the view content is wrong\n");
                    failures++;
                }

                status = ntb0_test_unmap_view(stub27, base);
                if (status == NTB0_ST_SUCCESS) {
                    printf("PASS: NtUnmapViewOfSection -> the view is released\n");
                } else {
                    printf("FAIL: NtUnmapViewOfSection failed (0x%lx)\n",
                           status);
                    failures++;
                }
            } else {
                printf("FAIL: NtMapViewOfSection failed (0x%lx)\n", status);
                failures++;
            }

            ntb0_call_win64(stub, (long)hsec, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }

        /* (ب) مقطع ملفّي: القراءة من الملف مباشرة (بلا نسخ) */
        {
            uint64_t hfile = 0, hsec2 = 0, base2 = 0, size2 = 0x1000;
            ntb0_io_status_block iosb;
            char fpath[512];

            snprintf(fpath, sizeof(fpath), "%s/ntb0_section.tmp", root);
            unlink(fpath);
            memset(&iosb, 0, sizeof(iosb));

            status = ntb0_test_create_file(
                stub52, "\\??\\C:\\ntb0_section.tmp",
                NTB0_GENERIC_READ | NTB0_GENERIC_WRITE,
                NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE, NTB0_FILE_CREATE,
                NTB0_FILE_NON_DIRECTORY_FILE, &hfile, &iosb);
            if (status != NTB0_ST_SUCCESS) {
                printf("FAIL: cannot create the section-test file (0x%lx)\n",
                       status);
                failures++;
            } else {
                memset(&iosb, 0, sizeof(iosb));
                ntb0_test_io(stub05, hfile, (void *)payload,
                             (uint32_t)strlen(payload), NULL, &iosb);

                status = ntb0_test_create_section(stub47, &hsec2, 0, hfile);
                if (status != NTB0_ST_SUCCESS) {
                    printf("FAIL: file-backed NtCreateSection failed (0x%lx)\n",
                           status);
                    failures++;
                } else {
                    status = ntb0_test_map_view(stub25, hsec2, &base2, &size2,
                                                0x04);
                    if (status == NTB0_ST_SUCCESS && base2) {
                        unsigned char *v2 = (unsigned char *)(uintptr_t)base2;

                        if (memcmp(v2, payload, strlen(payload)) == 0) {
                            printf("PASS: a file-backed section maps the file "
                                   "itself (no copy)\n");
                        } else {
                            printf("FAIL: the file-backed view has wrong "
                                   "content\n");
                            failures++;
                        }
                        ntb0_test_unmap_view(stub27, base2);
                    } else {
                        printf("FAIL: mapping the file-backed section failed "
                               "(0x%lx)\n", status);
                        failures++;
                    }
                    ntb0_call_win64(stub, (long)hsec2, 0, 0, 0, 0, 0, 0, 0, 0,
                                    0, 0);
                }
                ntb0_call_win64(stub, (long)hfile, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            }
            unlink(fpath);
        }
    }

    /* 27) SYS-027 — NtFsControlFile (T-052) */
    printf("\n-- SYS-027: NtFsControlFile (0x36) --\n");
    {
        uint64_t h = 0;
        ntb0_io_status_block iosb;
        uint32_t avail = 0, ret = 0;
        char fpath[512];

        snprintf(fpath, sizeof(fpath), "%s/ntb0_fsctl.tmp", root);
        unlink(fpath);
        memset(&iosb, 0, sizeof(iosb));

        status = ntb0_test_create_file(
            stub52, "\\??\\C:\\ntb0_fsctl.tmp",
            NTB0_GENERIC_READ | NTB0_GENERIC_WRITE,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE, NTB0_FILE_CREATE,
            NTB0_FILE_NON_DIRECTORY_FILE, &h, &iosb);
        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: cannot create the fsctl-test file (0x%lx)\n",
                   status);
            failures++;
        } else {
            memset(&iosb, 0, sizeof(iosb));
            ntb0_test_io(stub05, h, (void *)"0123456789", 10, NULL, &iosb);
            {
                int64_t zero = 0;

                ntb0_test_set_info_file(stub24, h, &zero, 2, 8);
            }

            status = ntb0_test_fs_control(stub36, h, NULL, 0, &avail,
                                          sizeof(avail), 0x541B, &ret);
            if (status == NTB0_ST_SUCCESS && avail == 10) {
                printf("PASS: NtFsControlFile(FIONREAD) -> %u bytes\n", avail);
            } else {
                printf("FAIL: NtFsControlFile failed (0x%lx, avail=%u)\n",
                       status, avail);
                failures++;
            }

            ntb0_call_win64(stub, (long)h, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
        unlink(fpath);
    }

    /* 28) SYS-028/029 — أقفال الملفات (T-053) */
    printf("\n-- SYS-028/029: lock / unlock (0xe0/0x188) --\n");
    {
        uint64_t h = 0;
        ntb0_io_status_block iosb;
        char fpath[512];

        snprintf(fpath, sizeof(fpath), "%s/ntb0_lock.tmp", root);
        unlink(fpath);
        memset(&iosb, 0, sizeof(iosb));

        status = ntb0_test_create_file(
            stub52, "\\??\\C:\\ntb0_lock.tmp",
            NTB0_GENERIC_READ | NTB0_GENERIC_WRITE,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE, NTB0_FILE_CREATE,
            NTB0_FILE_NON_DIRECTORY_FILE, &h, &iosb);
        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: cannot create the lock-test file (0x%lx)\n",
                   status);
            failures++;
        } else {
            status = ntb0_test_lock_file(stubE0, h, 0, 64,
                                         0x3 /*EXCLUSIVE|FAIL_IMMEDIATELY*/);
            if (status == NTB0_ST_SUCCESS) {
                printf("PASS: NtLockFile (exclusive, 64 bytes) -> locked\n");
            } else {
                printf("FAIL: NtLockFile failed (0x%lx)\n", status);
                failures++;
            }

            status = ntb0_test_unlock_file(stub188, h, 0, 64);
            if (status == NTB0_ST_SUCCESS) {
                printf("PASS: NtUnlockFile -> unlocked\n");
            } else {
                printf("FAIL: NtUnlockFile failed (0x%lx)\n", status);
                failures++;
            }

            /* مقبض غير صالح ⇒ INVALID_HANDLE */
            ntb0_set_expect_fail(1);
            status = ntb0_test_lock_file(stubE0, 0xDEAD, 0, 64, 0x3);
            ntb0_set_expect_fail(0);
            if (status == NTB0_ST_INVALID_HANDLE) {
                printf("PASS: NtLockFile on a bad handle -> "
                       "STATUS_INVALID_HANDLE\n");
            } else {
                printf("FAIL: expected 0x%lx, got 0x%lx\n",
                       (long)NTB0_ST_INVALID_HANDLE, status);
                failures++;
            }

            ntb0_call_win64(stub, (long)h, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
        unlink(fpath);
    }

    /* 29) SYS-030 — NtQueryAttributesFile (T-055) */
    printf("\n-- SYS-030: NtQueryAttributesFile (0x3a) --\n");
    {
        uint64_t hdir = 0, hfile = 0;
        ntb0_io_status_block iosb;
        uint32_t attrs = 0;

        memset(&iosb, 0, sizeof(iosb));
        status = ntb0_test_open_file(
            stub30, "\\??\\C:\\Windows\\System32", NTB0_GENERIC_READ,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE | NTB0_FILE_SHARE_DELETE,
            NTB0_FILE_OPEN, NTB0_FILE_DIRECTORY_FILE, &hdir, &iosb);
        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: cannot open the directory (0x%lx)\n", status);
            failures++;
        } else {
            status = ntb0_test_query_attrs(stub3a, hdir, &attrs,
                                           sizeof(attrs));
            if (status == NTB0_ST_SUCCESS && (attrs & 0x10u)) {
                printf("PASS: a directory reports "
                       "FILE_ATTRIBUTE_DIRECTORY (0x%x)\n", attrs);
            } else {
                printf("FAIL: wrong directory attributes (0x%lx, 0x%x)\n",
                       status, attrs);
                failures++;
            }
            ntb0_call_win64(stub, (long)hdir, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }

        memset(&iosb, 0, sizeof(iosb));
        status = ntb0_test_open_file(
            stub30, "\\??\\C:\\Windows\\System32\\ntdll.dll",
            NTB0_GENERIC_READ,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE | NTB0_FILE_SHARE_DELETE,
            NTB0_FILE_OPEN, NTB0_FILE_NON_DIRECTORY_FILE, &hfile, &iosb);
        if (status == NTB0_ST_SUCCESS) {
            attrs = 0;
            status = ntb0_test_query_attrs(stub3a, hfile, &attrs,
                                           sizeof(attrs));
            if (status == NTB0_ST_SUCCESS && (attrs & 0x80u)) {
                printf("PASS: a file reports FILE_ATTRIBUTE_NORMAL (0x%x)\n",
                       attrs);
            } else {
                printf("FAIL: wrong file attributes (0x%lx, 0x%x)\n", status,
                       attrs);
                failures++;
            }
            ntb0_call_win64(stub, (long)hfile, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
    }

    /* 30) SYS-031 — NtQueryObject (T-047) */
    printf("\n-- SYS-031: NtQueryObject (0x0d) --\n");
    {
        uint64_t h = 0;
        ntb0_io_status_block iosb;
        unsigned char info[0x20];
        uint32_t ret = 0;
        uint16_t *name16;

        memset(&iosb, 0, sizeof(iosb));
        status = ntb0_test_open_file(
            stub30, "\\??\\C:\\Windows\\System32\\ntdll.dll",
            NTB0_GENERIC_READ,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE | NTB0_FILE_SHARE_DELETE,
            NTB0_FILE_OPEN, NTB0_FILE_NON_DIRECTORY_FILE, &h, &iosb);
        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: cannot open the file for NtQueryObject (0x%lx)\n",
                   status);
            failures++;
        } else {
            /* الصنف 5: ObjectNameInformation */
            memset(info, 0, sizeof(info));
            status = ntb0_test_query_object(stub0d, h, 5, info, sizeof(info),
                                            &ret);
            if (status == NTB0_ST_SUCCESS) {
                uint16_t nlen = *(uint16_t *)(info + 0x00);

                name16 = *(uint16_t **)(info + 0x08);
                if (nlen >= 10 && name16) {
                    char nm[256];
                    size_t k;

                    for (k = 0; k < nlen / 2 && k + 1 < sizeof(nm); k++)
                        nm[k] = (char)(name16[k] < 128 ? name16[k] : '?');
                    nm[nlen / 2 < sizeof(nm) ? nlen / 2 : sizeof(nm) - 1] = 0;

                    printf("PASS: ObjectNameInformation -> \"%s\" (%u bytes)\n",
                           nm, nlen);
                } else {
                    printf("FAIL: wrong object name (len=%u)\n", nlen);
                    failures++;
                }
            } else {
                printf("FAIL: ObjectNameInformation failed (0x%lx)\n", status);
                failures++;
            }

            /* الصنف 2: ObjectTypeInformation */
            memset(info, 0, sizeof(info));
            status = ntb0_test_query_object(stub0d, h, 2, info, sizeof(info),
                                            &ret);
            if (status == NTB0_ST_SUCCESS) {
                name16 = *(uint16_t **)(info + 0x08);
                if (ntb0_u16_is(name16, "File")) {
                    printf("PASS: ObjectTypeInformation -> \"File\"\n");
                } else {
                    printf("FAIL: wrong object type\n");
                    failures++;
                }
            } else {
                printf("FAIL: ObjectTypeInformation failed (0x%lx)\n", status);
                failures++;
            }

            /* صنف غير مدعوم ⇒ NOT_SUPPORTED (فشل متوقّع) */
            ntb0_set_expect_fail(1);
            status = ntb0_test_query_object(stub0d, h, 99, info, sizeof(info),
                                            &ret);
            ntb0_set_expect_fail(0);
            if (status == NTB0_ST_NOT_SUPPORTED) {
                printf("PASS: unsupported object class -> "
                       "STATUS_NOT_SUPPORTED\n");
            } else {
                printf("FAIL: expected 0x%lx, got 0x%lx\n",
                       (long)NTB0_ST_NOT_SUPPORTED, status);
                failures++;
            }

            ntb0_call_win64(stub, (long)h, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
    }

    /* 31) SYS-032/033/034/038/039/040 — الخيوط (T-040 · T-042) */
    printf("\n-- SYS-032/033/034/038/039/040: threads --\n");
    {
        uint64_t hthr = 0;
        uint32_t cid[2] = { 0, 0 };
        uint32_t prev = 0;
        uint32_t code = 0;
        unsigned char probe_buf[4] = { 0, 0, 0, 0 };
        int64_t timeout_5s = 500000000LL; /* 100-نانية ⇒ 5 ثوانٍ */

        /* 31a) إنشاء خيط معلق (THREAD_CREATE_SUSPENDED) */
        status = ntb0_test_create_thread(stub4b, &hthr,
                                         (uint64_t)(uintptr_t)ntb0_thread_probe,
                                         (uint64_t)(uintptr_t)probe_buf,
                                         1 /*suspended*/, cid);
        if (status != NTB0_ST_SUCCESS || hthr == 0) {
            printf("FAIL: NtCreateThread failed (0x%lx)\n", status);
            failures++;
        } else {
            printf("PASS: NtCreateThread(suspended) -> handle=%llu\n",
                   (unsigned long long)hthr);

            if (cid[0] == (uint32_t)getpid() && cid[1] != 0) {
                printf("PASS: CLIENT_ID = pid %u / tid %u\n", cid[0], cid[1]);
            } else {
                printf("FAIL: bad CLIENT_ID (pid=%u tid=%u)\n", cid[0], cid[1]);
                failures++;
            }

            /* الخيط معلّق ⇒ لم تُنفَّذ نقطة الدخول بعد */
            if (probe_buf[0] == 0) {
                printf("PASS: suspended thread did not run yet\n");
            } else {
                printf("FAIL: suspended thread ran anyway\n");
                failures++;
            }

            /* 31b) SYS-033 — الاستئناف: PreviousSuspendCount = 1 */
            status = ntb0_test_resume_thread(stub4f, hthr, &prev);
            if (status == NTB0_ST_SUCCESS && prev == 1) {
                printf("PASS: NtResumeThread -> previous suspend count %u\n",
                       prev);
            } else {
                printf("FAIL: NtResumeThread (0x%lx prev=%u)\n", status, prev);
                failures++;
            }

            /* 31c) SYS-019 — انتظار انتهاء الخيط عبر مقبضه */
            status = ntb0_test_wait(stub01, hthr, timeout_5s);
            if (status == NTB0_ST_SUCCESS) {
                printf("PASS: NtWaitForSingleObject(thread handle) -> "
                       "signaled\n");
            } else {
                printf("FAIL: waiting on the thread handle (0x%lx)\n", status);
                failures++;
            }

            /* 31d) جسر الاصطلاح: البايتات + رمز الخروج */
            ntb0_thread_join(hthr, &code);
            if (probe_buf[0] == 0xA5 && probe_buf[1] == 0x5A &&
                probe_buf[2] == 0xC3 && probe_buf[3] == 0x3C) {
                printf("PASS: thread wrote A5 5A C3 3C through the ABI bridge\n");
            } else {
                printf("FAIL: thread payload = %02x %02x %02x %02x\n",
                       probe_buf[0], probe_buf[1], probe_buf[2], probe_buf[3]);
                failures++;
            }
            if (code == 0x1234) {
                printf("PASS: thread exit status = 0x%x\n", code);
            } else {
                printf("FAIL: thread exit status = 0x%x (expected 0x1234)\n",
                       code);
                failures++;
            }

            /* 31e) SYS-040 — NtQueryInformationThread (صنف 0) */
            {
                unsigned char info[0x28];
                uint32_t ret = 0;
                int32_t exit_status;
                uint32_t tid;

                memset(info, 0, sizeof(info));
                status = ntb0_test_query_thread(stub22, hthr, 0, info,
                                                sizeof(info), &ret);
                if (status != NTB0_ST_SUCCESS) {
                    printf("FAIL: NtQueryInformationThread (0x%lx)\n", status);
                    failures++;
                } else {
                    exit_status = *(int32_t *)(info + 0x00);
                    tid = *(uint32_t *)(info + 0x14);
                    if (exit_status == 0x1234 && tid == cid[1] &&
                        ret == 0x28) {
                        printf("PASS: ThreadBasicInformation -> exit=0x%x "
                               "tid=%u (size 0x%x)\n", exit_status, tid, ret);
                    } else {
                        printf("FAIL: ThreadBasicInformation exit=0x%x tid=%u "
                               "size=0x%x\n", exit_status, tid, ret);
                        failures++;
                    }
                }
            }

            /* 31f) SYS-038 — NtOpenThread بمعرّف الخيط */
            {
                uint64_t h2 = 0;

                status = ntb0_test_open_thread(stubfe, &h2, cid[1]);
                if (status == NTB0_ST_SUCCESS && h2 != 0 && h2 != hthr) {
                    printf("PASS: NtOpenThread(tid=%u) -> handle=%llu\n", cid[1],
                           (unsigned long long)h2);
                    ntb0_call_win64(stub, (long)h2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                     0);
                } else {
                    printf("FAIL: NtOpenThread(tid=%u) (0x%lx)\n", cid[1],
                           status);
                    failures++;
                }
            }

            /* صنف غير مدعوم ⇒ NOT_SUPPORTED (فشل متوقّع) */
            {
                unsigned char info[0x28];
                uint32_t ret = 0;

                ntb0_set_expect_fail(1);
                status = ntb0_test_query_thread(stub22, hthr, 99, info,
                                                sizeof(info), &ret);
                ntb0_set_expect_fail(0);
                if (status == NTB0_ST_NOT_SUPPORTED) {
                    printf("PASS: unsupported thread class -> "
                           "STATUS_NOT_SUPPORTED\n");
                } else {
                    printf("FAIL: expected 0x%lx, got 0x%lx\n",
                           (long)NTB0_ST_NOT_SUPPORTED, status);
                    failures++;
                }
            }

            ntb0_call_win64(stub, (long)hthr, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
    }

    /* 31g) SYS-039/034 — تعليق خيط يدور ثم إنهاؤه */
    {
        uint64_t hloop = 0;
        uint32_t prev2 = 0;
        uint32_t code2 = 0;
        uint32_t cid2[2] = { 0, 0 };
        int64_t timeout_5s = 500000000LL;

        status = ntb0_test_create_thread(stub4b, &hloop,
                                         (uint64_t)(uintptr_t)ntb0_thread_loop,
                                         0, 0 /*not suspended*/, cid2);
        if (status != NTB0_ST_SUCCESS || hloop == 0) {
            printf("FAIL: NtCreateThread(loop) failed (0x%lx)\n", status);
            failures++;
        } else {
            printf("PASS: NtCreateThread(loop) -> handle=%llu tid=%u\n",
                   (unsigned long long)hloop, cid2[1]);

            /* SYS-039 — تعليق */
            status = ntb0_test_suspend_thread(stub17b, hloop, &prev2);
            if (status == NTB0_ST_SUCCESS && prev2 == 0) {
                printf("PASS: NtSuspendThread -> previous suspend count %u\n",
                       prev2);
            } else {
                printf("FAIL: NtSuspendThread (0x%lx prev=%u)\n", status, prev2);
                failures++;
            }

            /* SYS-033 — استئناف (1 ⇒ 0) */
            status = ntb0_test_resume_thread(stub4f, hloop, &prev2);
            if (status == NTB0_ST_SUCCESS && prev2 == 1) {
                printf("PASS: NtResumeThread -> previous suspend count %u\n",
                       prev2);
            } else {
                printf("FAIL: NtResumeThread (0x%lx prev=%u)\n", status, prev2);
                failures++;
            }

            /* SYS-034 — إنهاء */
            status = ntb0_test_terminate_thread(stub50, hloop, 0xDEAD);
            if (status == NTB0_ST_SUCCESS) {
                printf("PASS: NtTerminateThread -> exit status 0xDEAD\n");
            } else {
                printf("FAIL: NtTerminateThread (0x%lx)\n", status);
                failures++;
            }

            /* انتظار انتهاء الخيط (المقبض يُنبَّه عند الانتهاء) */
            status = ntb0_test_wait(stub01, hloop, timeout_5s);
            if (status == NTB0_ST_SUCCESS) {
                printf("PASS: terminated thread signaled its handle\n");
            } else {
                printf("FAIL: terminated thread did not signal (0x%lx)\n",
                       status);
                failures++;
            }

            ntb0_thread_join(hloop, &code2);
            if (code2 == 0xDEAD) {
                printf("PASS: terminated thread exit status = 0x%x\n", code2);
            } else {
                printf("FAIL: terminated thread exit status = 0x%x\n", code2);
                failures++;
            }

            ntb0_call_win64(stub, (long)hloop, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
    }

    /* 32) SYS-035/036/037 — العمليات (T-041 · T-046) */
    printf("\n-- SYS-035/036/037: processes --\n");
    {
        uint64_t hproc = 0;
        unsigned char info[0x30];
        uint32_t ret = 0;

        /* 32a) فتح العملية الحالية */
        status = ntb0_test_open_process(stub23, &hproc, (uint32_t)getpid());
        if (status == NTB0_ST_SUCCESS && hproc != 0) {
            printf("PASS: NtOpenProcess(self) -> handle=%llu\n",
                   (unsigned long long)hproc);
        } else {
            printf("FAIL: NtOpenProcess(self) (0x%lx)\n", status);
            failures++;
        }

        /* 32b) SYS-037 — ProcessBasicInformation */
        if (hproc) {
            memset(info, 0, sizeof(info));
            status = ntb0_test_query_process(stub16, hproc, 0, info,
                                             sizeof(info), &ret);
            if (status == NTB0_ST_SUCCESS && ret == 0x30) {
                uint64_t pid = *(uint64_t *)(info + 0x20);
                uint64_t ppid = *(uint64_t *)(info + 0x28);

                if (pid == (uint64_t)getpid() && ppid == (uint64_t)getppid()) {
                    printf("PASS: ProcessBasicInformation -> pid=%llu "
                           "ppid=%llu (size 0x%x)\n",
                           (unsigned long long)pid, (unsigned long long)ppid,
                           ret);
                } else {
                    printf("FAIL: ProcessBasicInformation pid=%llu "
                           "ppid=%llu\n", (unsigned long long)pid,
                           (unsigned long long)ppid);
                    failures++;
                }
            } else {
                printf("FAIL: NtQueryInformationProcess(0) (0x%lx size=0x%x)\n",
                       status, ret);
                failures++;
            }

            /* 32c) الصنف 27 — ImageFileName (UNICODE_STRING) */
            memset(info, 0, sizeof(info));
            status = ntb0_test_query_process(stub16, hproc, 27, info,
                                             sizeof(info), &ret);
            if (status == NTB0_ST_SUCCESS) {
                uint16_t nlen = *(uint16_t *)(info + 0x00);
                uint16_t *name16 = *(uint16_t **)(info + 0x08);

                if (nlen > 0 && name16) {
                    char nm[256];
                    size_t k;

                    for (k = 0; k < nlen / 2 && k + 1 < sizeof(nm); k++)
                        nm[k] = (char)(name16[k] < 128 ? name16[k] : '?');
                    nm[nlen / 2 < sizeof(nm) ? nlen / 2 : sizeof(nm) - 1] = 0;
                    printf("PASS: ProcessImageFileName -> \"%s\"\n", nm);
                } else {
                    printf("FAIL: ProcessImageFileName empty\n");
                    failures++;
                }
            } else {
                printf("FAIL: NtQueryInformationProcess(27) (0x%lx)\n", status);
                failures++;
            }

            /* 32d) الصنف 40 — Wow64Information (لسنا WoW64) */
            memset(info, 0, sizeof(info));
            status = ntb0_test_query_process(stub16, hproc, 40, info,
                                             sizeof(info), &ret);
            if (status == NTB0_ST_SUCCESS && *(uint32_t *)info == 0) {
                printf("PASS: ProcessWow64Information -> 0 (64-bit)\n");
            } else {
                printf("FAIL: ProcessWow64Information (0x%lx)\n", status);
                failures++;
            }

            /* صنف غير مدعوم ⇒ NOT_SUPPORTED (فشل متوقّع) */
            ntb0_set_expect_fail(1);
            status = ntb0_test_query_process(stub16, hproc, 99, info,
                                             sizeof(info), &ret);
            ntb0_set_expect_fail(0);
            if (status == NTB0_ST_NOT_SUPPORTED) {
                printf("PASS: unsupported process class -> "
                       "STATUS_NOT_SUPPORTED\n");
            } else {
                printf("FAIL: expected 0x%lx, got 0x%lx\n",
                       (long)NTB0_ST_NOT_SUPPORTED, status);
                failures++;
            }

            /* 32e) SYS-036 — إنهاء العملية الحالية ⇒ ACCESS_DENIED */
            ntb0_set_expect_fail(1);
            status = ntb0_test_terminate_process(stub29, hproc, 1);
            ntb0_set_expect_fail(0);
            if (status == NTB0_ST_ACCESS_DENIED) {
                printf("PASS: NtTerminateProcess(self) -> STATUS_ACCESS_DENIED\n");
            } else {
                printf("FAIL: expected 0x%lx, got 0x%lx\n",
                       (long)NTB0_ST_ACCESS_DENIED, status);
                failures++;
            }

            ntb0_call_win64(stub, (long)hproc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }

        /* 32f) فتح عملية غير موجودة ⇒ رفض صحيح */
        {
            uint64_t hbad = 0;

            ntb0_set_expect_fail(1);
            status = ntb0_test_open_process(stub23, &hbad, 0x7FFFFFF0u);
            ntb0_set_expect_fail(0);
            if (status != NTB0_ST_SUCCESS && hbad == 0) {
                printf("PASS: NtOpenProcess(bogus pid) -> 0x%lx (refused)\n",
                       status);
            } else {
                printf("FAIL: NtOpenProcess(bogus pid) succeeded\n");
                failures++;
            }
        }

        /* 32g) SYS-036 — إنهاء عملية ابنة حقيقية ⇒ SIGKILL */
        {
            pid_t child = fork();

            if (child == 0) {
                pause();    /* ينتظر الإ killing */
                _exit(0);
            } else if (child < 0) {
                printf("FAIL: fork() failed\n");
                failures++;
            } else {
                uint64_t hc = 0;
                int wst = 0;

                status = ntb0_test_open_process(stub23, &hc,
                                                (uint32_t)child);
                if (status != NTB0_ST_SUCCESS) {
                    printf("FAIL: NtOpenProcess(child) (0x%lx)\n", status);
                    failures++;
                } else {
                    status = ntb0_test_terminate_process(stub29, hc, 0x55);
                    if (status != NTB0_ST_SUCCESS) {
                        printf("FAIL: NtTerminateProcess(child) (0x%lx)\n",
                               status);
                        failures++;
                    } else if (waitpid(child, &wst, 0) == child &&
                               WIFSIGNALED(wst) && WTERMSIG(wst) == SIGKILL) {
                        printf("PASS: NtTerminateProcess(child) -> SIGKILL\n");
                    } else {
                        printf("FAIL: child was not killed by SIGKILL "
                               "(status=0x%x)\n", wst);
                        failures++;
                    }
                    ntb0_call_win64(stub, (long)hc, 0, 0, 0, 0, 0, 0, 0, 0,
                                     0, 0);
                }
            }
        }
    }

    /* 33) SYS-045 — NtCreateNamedPipeFile (T-051) */
    printf("\n-- SYS-045: NtCreateNamedPipeFile (0x9b) --\n");
    {
        uint64_t hpipe = 0, hpipe2 = 0;
        ntb0_io_status_block iosb;

        memset(&iosb, 0, sizeof(iosb));
        status = ntb0_test_create_pipe(stub9b, &hpipe,
                                        "\\Device\\NamedPipe\\ntb0test",
                                        NTB0_FILE_OPEN_IF, &iosb);
        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: cannot create the named pipe (0x%lx)\n", status);
            failures++;
        } else {
            printf("PASS: named pipe created -> handle 0x%llx\n",
                   (unsigned long long)hpipe);

            if (iosb.status == NTB0_ST_SUCCESS) {
                printf("PASS: IO_STATUS_BLOCK filled (status=0)\n");
            } else {
                printf("FAIL: IO_STATUS_BLOCK status = 0x%x\n", iosb.status);
                failures++;
            }

            /* ذهاب وإياب حقيقي عبر الأنبوب (FIFO) */
            {
                char wbuf[16], rbuf[16];

                memset(wbuf, 0, sizeof(wbuf));
                memset(rbuf, 0, sizeof(rbuf));
                memcpy(wbuf, "PIPE-OK", 7);

                status = ntb0_test_io(stub05, hpipe, wbuf, 7, 0, &iosb);
                if (status != NTB0_ST_SUCCESS) {
                    printf("FAIL: write to the pipe (0x%lx)\n", status);
                    failures++;
                } else {
                    status = ntb0_test_io(stub03, hpipe, rbuf, 7, 0, &iosb);
                    if (status == NTB0_ST_SUCCESS &&
                        memcmp(rbuf, "PIPE-OK", 7) == 0) {
                        printf("PASS: pipe round trip -> \"%s\"\n", rbuf);
                    } else {
                        printf("FAIL: pipe read back (0x%lx) \"%s\"\n", status,
                               rbuf);
                        failures++;
                    }
                }
            }

            /* إنشاء ثانٍ ⇒ تعارض أسماء (رفض صحيح) */
            ntb0_set_expect_fail(1);
            status = ntb0_test_create_pipe(stub9b, &hpipe2,
                                            "\\Device\\NamedPipe\\ntb0test",
                                            NTB0_FILE_CREATE, NULL);
            ntb0_set_expect_fail(0);
            if (status == NTB0_ST_OBJECT_NAME_COLLISION) {
                printf("PASS: second create -> STATUS_OBJECT_NAME_COLLISION\n");
            } else {
                printf("FAIL: expected 0x%lx, got 0x%lx\n",
                       (long)NTB0_ST_OBJECT_NAME_COLLISION, status);
                failures++;
            }

            /* فتح أنبوب غير موجود ⇒ رفض صحيح */
            ntb0_set_expect_fail(1);
            status = ntb0_test_create_pipe(stub9b, &hpipe2,
                                            "\\Device\\NamedPipe\\ntb0missing",
                                            NTB0_FILE_OPEN, NULL);
            ntb0_set_expect_fail(0);
            if (status == NTB0_ST_OBJECT_NAME_NOT_FOUND) {
                printf("PASS: open a missing pipe -> "
                       "STATUS_OBJECT_NAME_NOT_FOUND\n");
            } else {
                printf("FAIL: expected 0x%lx, got 0x%lx\n",
                       (long)NTB0_ST_OBJECT_NAME_NOT_FOUND, status);
                failures++;
            }

            ntb0_call_win64(stub, (long)hpipe, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
    }

    /* 34) SYS-041 — NtSetInformationThread (T-048) */
    printf("\n-- SYS-041: NtSetInformationThread (0x0a) --\n");
    {
        const uint64_t pseudo = 0xFFFFFFFFFFFFFFFEull; /* NtCurrentThread */
        uint64_t teb = 0x7FFDEADBEEF0000ull;
        uint32_t prio = 0;
        uint64_t mask = 1;
        cpu_set_t orig, now;

        /* 34a) مؤشر TEB — ذهاب وإياب */
        status = ntb0_test_set_thread_info(stub0a, pseudo,
                                            NTB0_TIC_TEB_POINTER, &teb,
                                            sizeof(teb));
        if (status == NTB0_ST_SUCCESS &&
            ntb0_thread_get_current_teb() == teb) {
            printf("PASS: TEB pointer stored -> 0x%llx\n",
                   (unsigned long long)teb);
        } else {
            printf("FAIL: TEB pointer (0x%lx, stored 0x%llx)\n", status,
                   (unsigned long long)ntb0_thread_get_current_teb());
            failures++;
        }

        /* 34b) أولوية أدنى ⇒ مسموحة دائماً */
        prio = (uint32_t)(int32_t)-1; /* NT: -1 ⇒ أدنى قليلاً (nice +1) */
        status = ntb0_test_set_thread_info(stub0a, pseudo,
                                            NTB0_TIC_PRIORITY, &prio,
                                            sizeof(prio));
        if (status == NTB0_ST_SUCCESS) {
            printf("PASS: lower priority accepted (nice +1)\n");
        } else {
            printf("FAIL: lower priority (0x%lx)\n", status);
            failures++;
        }
        /*
         * إعادة الأولوية الأصلية = **رفع** الأولوية ⇒ يحتاج امتياز
         * (وهو سلوك صحيح ⇒ مصباح OK لا FAIL).
         */
        prio = 0;
        ntb0_set_expect_fail(1);
        ntb0_test_set_thread_info(stub0a, pseudo, NTB0_TIC_PRIORITY, &prio,
                                  sizeof(prio));
        ntb0_set_expect_fail(0);

        /* 34c) أولوية أعلى ⇒ تحتاج امتياز (كما في NT) */
        prio = 10;
        ntb0_set_expect_fail(1);
        status = ntb0_test_set_thread_info(stub0a, pseudo,
                                            NTB0_TIC_PRIORITY, &prio,
                                            sizeof(prio));
        ntb0_set_expect_fail(0);
        if (status == NTB0_ST_SUCCESS ||
            status == NTB0_ST_PRIVILEGE_NOT_HELD) {
            printf("PASS: higher priority -> 0x%lx (SUCCESS أو "
                   "PRIVILEGE_NOT_HELD)\n", status);
        } else {
            printf("FAIL: higher priority (0x%lx)\n", status);
            failures++;
        }

        /* 34d) ألفة المعالجات — تُقاس على لينكس */
        CPU_ZERO(&orig);
        sched_getaffinity(0, sizeof(orig), &orig);
        status = ntb0_test_set_thread_info(stub0a, pseudo,
                                            NTB0_TIC_AFFINITY_MASK, &mask,
                                            sizeof(mask));
        CPU_ZERO(&now);
        sched_getaffinity(0, sizeof(now), &now);
        if (status == NTB0_ST_SUCCESS && CPU_ISSET(0, &now)) {
            printf("PASS: affinity mask applied (CPU 0 pinned)\n");
        } else {
            printf("FAIL: affinity (0x%lx)\n", status);
            failures++;
        }
        sched_setaffinity(0, sizeof(orig), &orig); /* استعادة */

        /* 34e) صنف للقراءة فقط ⇒ INVALID_INFO_CLASS */
        ntb0_set_expect_fail(1);
        status = ntb0_test_set_thread_info(stub0a, pseudo, NTB0_TIC_BASIC,
                                            &prio, sizeof(prio));
        ntb0_set_expect_fail(0);
        if (status == NTB0_ST_INVALID_INFO_CLASS) {
            printf("PASS: read-only class -> STATUS_INVALID_INFO_CLASS\n");
        } else {
            printf("FAIL: expected 0x%lx, got 0x%lx\n",
                   (long)NTB0_ST_INVALID_INFO_CLASS, status);
            failures++;
        }

        /* 34f) مقبض غير صالح ⇒ رفض صحيح */
        ntb0_set_expect_fail(1);
        status = ntb0_test_set_thread_info(stub0a, 0x1234,
                                            NTB0_TIC_HIDE_DEBUGGER, &prio,
                                            sizeof(prio));
        ntb0_set_expect_fail(0);
        if (status == NTB0_ST_INVALID_HANDLE) {
            printf("PASS: bad thread handle -> STATUS_INVALID_HANDLE\n");
        } else {
            printf("FAIL: expected 0x%lx, got 0x%lx\n",
                   (long)NTB0_ST_INVALID_HANDLE, status);
            failures++;
        }
    }

    /* 35) SYS-048 — NtSetSystemInformation (T-036) */
    printf("\n-- SYS-048: NtSetSystemInformation (0x16c) --\n");
    {
        int64_t tod[3];      /* BootTime · CurrentTime · TimeZoneBias */
        unsigned char qbuf[0x20];
        uint32_t ret = 0;
        uint64_t before = 0, after = 0;

        /* 35a) ضبط وقت النظام ثم قراءته (دورة كاملة) */
        memset(tod, 0, sizeof(tod));
        tod[1] = 0; /* نضبطه على "الآن" من جهة الاختبار */

        {
            struct timespec ts;

            clock_gettime(CLOCK_REALTIME, &ts);
            tod[1] = (int64_t)ts.tv_sec * 10000000LL + ts.tv_nsec / 100;
            tod[1] += 3600LL * 10000000LL; /* ساعة مقدّمة */
            tod[0] = tod[1] - 86400LL * 10000000LL;
        }

        status = ntb0_test_set_system_info(stub16c, NTB0_SIC_TIMEOFDAY, tod,
                                            sizeof(tod));
        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: set the time of day (0x%lx)\n", status);
            failures++;
        } else {
            memset(qbuf, 0, sizeof(qbuf));
            status = ntb0_call_win64(stub33, (long)NTB0_SIC_TIMEOFDAY,
                                     (long)qbuf, (long)sizeof(qbuf),
                                     (long)&ret, 0, 0, 0, 0, 0, 0, 0);
            if (status == NTB0_ST_SUCCESS && ret == 0x18) {
                memcpy(&before, qbuf + 8, sizeof(before));
                memcpy(&after, tod + 1, sizeof(after));
                if (before == after) {
                    printf("PASS: time of day set and read back exactly "
                           "(%lld)\n", (long long)before);
                } else {
                    int64_t diff = before - after;

                    if (diff < 0)
                        diff = -diff;
                    if (diff < 10000000LL) { /* < 1 ثانية */
                        printf("PASS: time of day within 1s (%lld)\n",
                               (long long)diff);
                    } else {
                        printf("FAIL: time of day mismatch (%lld vs %lld)\n",
                               (long long)before, (long long)after);
                        failures++;
                    }
                }
            } else {
                printf("FAIL: query the time of day (0x%lx, ret=%u)\n",
                       status, ret);
                failures++;
            }

            /* إعادة الوقت الأصلي */
            tod[1] -= 3600LL * 10000000LL;
            ntb0_test_set_system_info(stub16c, NTB0_SIC_TIMEOFDAY, tod,
                                       sizeof(tod));
        }

        /* 35b) إطفاء المضيف ⇒ مرفوض دائماً (أمان) */
        ntb0_set_expect_fail(1);
        status = ntb0_test_set_system_info(stub16c, NTB0_SIC_SHUTDOWN, NULL, 0);
        ntb0_set_expect_fail(0);
        if (status == NTB0_ST_PRIVILEGE_NOT_HELD) {
            printf("PASS: SystemShutdownInformation refused "
                   "(the host is never powered off)\n");
        } else {
            printf("FAIL: expected 0x%lx, got 0x%lx\n",
                   (long)NTB0_ST_PRIVILEGE_NOT_HELD, status);
            failures++;
        }

        /* 35c) CrashDump ⇒ مرفوض (لا تُعطِّل النواة) */
        ntb0_set_expect_fail(1);
        status = ntb0_test_set_system_info(stub16c, NTB0_SIC_CRASH_DUMP, NULL,
                                            0);
        ntb0_set_expect_fail(0);
        if (status == NTB0_ST_PRIVILEGE_NOT_HELD) {
            printf("PASS: SystemCrashDumpInformation refused\n");
        } else {
            printf("FAIL: expected 0x%lx, got 0x%lx\n",
                   (long)NTB0_ST_PRIVILEGE_NOT_HELD, status);
            failures++;
        }

        /* 35d) ExtendServiceTable ⇒ مرفوض (لا جداول خدمات في نواة المضيف) */
        ntb0_set_expect_fail(1);
        status = ntb0_test_set_system_info(stub16c,
                                            NTB0_SIC_EXTEND_SERVICE_TABLE,
                                            NULL, 0);
        ntb0_set_expect_fail(0);
        if (status == NTB0_ST_PRIVILEGE_NOT_HELD) {
            printf("PASS: SystemExtendServiceTableInformation refused\n");
        } else {
            printf("FAIL: expected 0x%lx, got 0x%lx\n",
                   (long)NTB0_ST_PRIVILEGE_NOT_HELD, status);
            failures++;
        }

        /* 35e) صنف غير مدعوم ⇒ NOT_SUPPORTED */
        ntb0_set_expect_fail(1);
        status = ntb0_test_set_system_info(stub16c, 99, NULL, 0);
        ntb0_set_expect_fail(0);
        if (status == NTB0_ST_NOT_SUPPORTED) {
            printf("PASS: unsupported system class -> "
                   "STATUS_NOT_SUPPORTED\n");
        } else {
            printf("FAIL: expected 0x%lx, got 0x%lx\n",
                   (long)NTB0_ST_NOT_SUPPORTED, status);
            failures++;
        }
    }

    /* 36) SYS-046 — NtQueryFullAttributesFile (T-056) */
    printf("\n-- SYS-046: NtQueryFullAttributesFile (0x113) --\n");
    {
        uint64_t hfile = 0;
        ntb0_io_status_block iosb;
        unsigned char info[0x48];
        uint32_t ret = 0;
        char diskpath[512];
        long disk_size;

        memset(&iosb, 0, sizeof(iosb));
        status = ntb0_test_open_file(
            stub30, "\\??\\C:\\Windows\\System32\\ntdll.dll",
            NTB0_GENERIC_READ,
            NTB0_FILE_SHARE_READ | NTB0_FILE_SHARE_WRITE | NTB0_FILE_SHARE_DELETE,
            NTB0_FILE_OPEN, NTB0_FILE_NON_DIRECTORY_FILE, &hfile, &iosb);
        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: cannot open the file (0x%lx)\n", status);
            failures++;
        } else {
            memset(info, 0, sizeof(info));
            status = ntb0_test_query_full_attrs(stub113, hfile, info,
                                                 sizeof(info), &ret);
            if (status == NTB0_ST_SUCCESS && ret == sizeof(info)) {
                uint32_t attrs = *(uint32_t *)(info + 0x04);
                uint64_t eof = *(uint64_t *)(info + 0x30);
                uint32_t links = *(uint32_t *)(info + 0x40);

                snprintf(diskpath, sizeof(diskpath), "%s/Windows/System32/ntdll.dll",
                         root);
                disk_size = ntb0_disk_size(diskpath);

                if (attrs != 0 && (attrs & NTB0_FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                    links >= 1) {
                    printf("PASS: full attributes (attrs=0x%x links=%u)\n",
                           attrs, links);
                } else {
                    printf("FAIL: wrong attributes (0x%x, links=%u)\n", attrs,
                           links);
                    failures++;
                }

                if (disk_size > 0 && (uint64_t)disk_size == eof) {
                    printf("PASS: EndOfFileLarge matches the disk (%lld)\n",
                           (long long)eof);
                } else {
                    printf("FAIL: EndOfFileLarge=%lld disk=%ld\n",
                           (long long)eof, disk_size);
                    failures++;
                }
            } else {
                printf("FAIL: NtQueryFullAttributesFile (0x%lx, ret=%u)\n",
                       status, ret);
                failures++;
            }

            /* مخزن صغير ⇒ رفض صحيح */
            ntb0_set_expect_fail(1);
            status = ntb0_test_query_full_attrs(stub113, hfile, info, 8, &ret);
            ntb0_set_expect_fail(0);
            if (status == NTB0_ST_STATUS_LOGICAL_ERROR) {
                printf("PASS: small buffer -> STATUS_BUFFER_TOO_SMALL\n");
            } else {
                printf("FAIL: expected 0x%lx, got 0x%lx\n",
                       (long)NTB0_ST_STATUS_LOGICAL_ERROR, status);
                failures++;
            }

            ntb0_call_win64(stub, (long)hfile, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }
    }

    /* 37) SYS-049/050/051/052/053 — الريجستري (T-110 … T-114) */
    printf("\n-- SYS-049/050/051/052/053: registry --\n");
    {
        uint64_t hkey = 0, hsvc = 0, hbad = 0;
        unsigned char vbuf[512];
        uint32_t ret = 0;
        int i;

        /* 37a) فتح مفتاح خدمة حقيقي من خلية SYSTEM الحقيقية */
        status = ntb0_test_open_key(
            stub0f, &hkey,
            "\\Registry\\Machine\\SYSTEM\\ControlSet001\\Services\\RpcSs");
        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: cannot open the registry key (0x%lx)\n", status);
            failures++;
        } else {
            printf("PASS: NtOpenKey(SYSTEM\\...\\RpcSs) -> handle 0x%llx\n",
                   (unsigned long long)hkey);

            /* 37b) قراءة قيمة نصية (ImagePath) */
            memset(vbuf, 0, sizeof(vbuf));
            status = ntb0_test_query_value(stub14, hkey, "ImagePath",
                                           NTB0_KVI_PARTIAL, vbuf,
                                           sizeof(vbuf), &ret);
            if (status == NTB0_ST_SUCCESS) {
                uint32_t dlen = *(uint32_t *)(vbuf + 0x08);
                uint32_t vtype = *(uint32_t *)(vbuf + 0x04);
                char txt[256];
                size_t k, o = 0;

                for (k = 0; k + 1 < dlen / 2 && o + 1 < sizeof(txt); k++) {
                    uint16_t c = *(uint16_t *)(vbuf + 0x0C + k * 2);

                    txt[o++] = (char)(c < 128 ? c : '?');
                }
                txt[o] = 0;

                if (vtype == NTB0_REG_EXPAND_SZ && strstr(txt, "svchost.exe")) {
                    printf("PASS: ImagePath -> \"%s\" (type %u, %u bytes)\n",
                           txt, vtype, dlen);
                } else {
                    printf("FAIL: wrong ImagePath (type %u) \"%s\"\n", vtype, txt);
                    failures++;
                }
            } else {
                printf("FAIL: NtQueryValueKey(ImagePath) (0x%lx)\n", status);
                failures++;
            }

            /* 37c) قيمة DWORD (Start = 2) */
            memset(vbuf, 0, sizeof(vbuf));
            status = ntb0_test_query_value(stub14, hkey, "Start", NTB0_KVI_PARTIAL,
                                           vbuf, sizeof(vbuf), &ret);
            if (status == NTB0_ST_SUCCESS) {
                uint32_t vtype = *(uint32_t *)(vbuf + 0x04);
                uint32_t dlen = *(uint32_t *)(vbuf + 0x08);
                uint32_t val = (dlen >= 4) ? *(uint32_t *)(vbuf + 0x0C) : 0;

                if (vtype == NTB0_REG_DWORD && val == 2) {
                    printf("PASS: Start = %u (REG_DWORD)\n", val);
                } else {
                    printf("FAIL: Start = %u (type %u)\n", val, vtype);
                    failures++;
                }
            } else {
                printf("FAIL: NtQueryValueKey(Start) (0x%lx)\n", status);
                failures++;
            }

            /* 37d) تعداد قيم المفتاح (12 قيمة) */
            {
                int count = 0;

                for (i = 0; i < 32; i++) {
                    unsigned char vb[256];
                    uint32_t rl = 0;

                    ntb0_set_expect_fail(1);
                    status = ntb0_test_enum_value(stub10, hkey,
                                                   NTB0_KVI_BASIC, (uint32_t)i,
                                                   vb, sizeof(vb), &rl);
                    ntb0_set_expect_fail(0);
                    if (status == NTB0_ST_SUCCESS)
                        count++;
                    else
                        break;
                }
                if (count >= 12) {
                    printf("PASS: enumerated %d value(s) of RpcSs\n", count);
                } else {
                    printf("FAIL: enumerated only %d value(s)\n", count);
                    failures++;
                }
            }

            /* 37e) NtFlushKey (خلية قراءة فقط ⇒ نجاح بلا عمل) */
            status = ntb0_test_flush_key(stubc3, hkey);
            if (status == NTB0_ST_SUCCESS) {
                printf("PASS: NtFlushKey -> STATUS_SUCCESS (read-only hive)\n");
            } else {
                printf("FAIL: NtFlushKey (0x%lx)\n", status);
                failures++;
            }

            ntb0_call_win64(stub, (long)hkey, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }

        /* 37f) تعداد المفاتيح الفرعية لمجلد الخدمات */
        status = ntb0_test_open_key(
            stub0f, &hsvc,
            "\\Registry\\Machine\\SYSTEM\\ControlSet001\\Services");
        if (status != NTB0_ST_SUCCESS) {
            printf("FAIL: cannot open the services key (0x%lx)\n", status);
            failures++;
        } else {
            int count = 0;

            for (i = 0; i < 600; i++) {
                unsigned char kb[512];
                uint32_t rl = 0;

                ntb0_set_expect_fail(1);
                status = ntb0_test_enum_key(stub2f, hsvc, NTB0_KEI_BASIC,
                                            (uint32_t)i, kb, sizeof(kb), &rl);
                ntb0_set_expect_fail(0);
                if (status == NTB0_ST_SUCCESS)
                    count++;
                else
                    break;
            }
            if (count >= 100) {
                printf("PASS: enumerated %d service key(s)\n", count);
            } else {
                printf("FAIL: enumerated only %d service key(s)\n", count);
                failures++;
            }
            ntb0_call_win64(stub, (long)hsvc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }

        /* 37g) مفتاح غير موجود ⇒ رفض صحيح */
        ntb0_set_expect_fail(1);
        status = ntb0_test_open_key(
            stub0f, &hbad,
            "\\Registry\\Machine\\SYSTEM\\ControlSet001\\Services\\NoSuchService");
        ntb0_set_expect_fail(0);
        if (status == NTB0_ST_OBJECT_NAME_NOT_FOUND) {
            printf("PASS: missing key -> STATUS_OBJECT_NAME_NOT_FOUND\n");
        } else {
            printf("FAIL: expected 0x%lx, got 0x%lx\n",
                   (long)NTB0_ST_OBJECT_NAME_NOT_FOUND, status);
            failures++;
        }
    }

    /* 12) جدول الـ syscalls المستخرج + إحصاءات الترجمة */
    printf("\nsyscall table: %u entries (NtCreateFile=0x%lx, NtClose=0x%lx)\n",
           ntb0_syscall_count,
           (unsigned long)ntb0_syscall_number("NtCreateFile"),
           (unsigned long)ntb0_syscall_number("NtClose"));

    {
        struct ntb0_stat_pub st[16];
        int n = ntb0_stats_get(st, 16);
        int i;

        printf("\n-- translation stats --\n");
        printf("%-9s %-24s %6s %6s\n", "ID", "NAME", "CALLS", "FAILS");
        for (i = 0; i < n; i++)
            printf("%-9s %-24s %6lu %6lu\n", st[i].id, st[i].name, st[i].calls,
                   st[i].fails);
    }

    printf("\n== %s (%d failure(s)) ==\n", failures ? "FAILED" : "PASSED",
           failures);
    return failures ? 1 : 0;
}
