/*
 * ntb0_nt.h — أنواع NT الحقيقية (Windows x64) التي يحتاجها مترجم الـ syscalls.
 *
 * لماذا هذا الملف:
 *   المترجم (ntb0_dispatch) يستقبل معاملات syscall خاماً من كود ويندوز.
 *   هذه المعاملات (OBJECT_ATTRIBUTES / UNICODE_STRING / IO_STATUS_BLOCK)
 *   يجب أن تُقرأ بتخطيط **ويندوز الحقيقي** — أي نفس إزاحات Win64.
 *   لا ترجمة مبنية على تخمين.
 *
 * التخطيطات مستقيمة من Win64 (مستخدمة في Win7 SP1 x64):
 *
 *   UNICODE_STRING      0x00 Length (USHORT)   0x02 MaximumLength (USHORT)
 *                       0x08 Buffer (PWSTR)     — الحجم 16
 *
 *   OBJECT_ATTRIBUTES   0x00 Length (ULONG)     0x08 RootDirectory (HANDLE)
 *                       0x10 ObjectName (PUNICODE_STRING)
 *                       0x18 Attributes (ULONG) 0x20 SecurityDescriptor
 *                       0x28 SecurityQualityOfService  — الحجم 0x30
 *
 *   IO_STATUS_BLOCK     0x00 Status (NTSTATUS)  0x08 Information (ULONG_PTR)
 *                       — الحجم 0x10
 *
 * القاعدة (NTBOX.md §9): كل ثابت هنا مُقيس من بنية Win64، والتأكيدات
 * (_Static_assert) تمنع أي انزياح في التخطيط.
 */
#ifndef NTB0_NT_H
#define NTB0_NT_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* UNICODE_STRING                                                      */
/* ------------------------------------------------------------------ */

typedef struct ntb0_unicode_string {
    uint16_t length;          /* 0x00 — الطول بالبايت (بدون NUL) */
    uint16_t maximum_length;  /* 0x02 */
    uint16_t reserved[2];     /* 0x04 — حشو المحاذاة إلى 8 */
    uint16_t *buffer;         /* 0x08 — UTF-16LE */
} ntb0_unicode_string;

/* ------------------------------------------------------------------ */
/* OBJECT_ATTRIBUTES                                                   */
/* ------------------------------------------------------------------ */

typedef struct ntb0_object_attributes {
    uint32_t              length;        /* 0x00 — sizeof(OBJECT_ATTRIBUTES) */
    uint32_t              reserved0;     /* 0x04 */
    void                 *root_directory;/* 0x08 — HANDLE */
    ntb0_unicode_string  *object_name;   /* 0x10 — PUNICODE_STRING */
    uint32_t              attributes;    /* 0x18 */
    uint32_t              reserved1;     /* 0x1C */
    void                 *security_descriptor;        /* 0x20 */
    void                 *security_quality_of_service; /* 0x28 */
} ntb0_object_attributes;

/* ------------------------------------------------------------------ */
/* IO_STATUS_BLOCK                                                     */
/* ------------------------------------------------------------------ */

typedef struct ntb0_io_status_block {
    int32_t  status;      /* 0x00 — NTSTATUS */
    uint32_t reserved;    /* 0x04 */
    uint64_t information; /* 0x08 — ULONG_PTR */
} ntb0_io_status_block;

/* تأكيد التخطيط — يمنع أي انزياح صامت */
_Static_assert(sizeof(ntb0_unicode_string) == 16, "UNICODE_STRING size");
_Static_assert(offsetof(ntb0_unicode_string, buffer) == 8, "UNICODE_STRING.Buffer");
_Static_assert(sizeof(ntb0_object_attributes) == 0x30, "OBJECT_ATTRIBUTES size");
_Static_assert(offsetof(ntb0_object_attributes, object_name) == 0x10,
               "OBJECT_ATTRIBUTES.ObjectName");
_Static_assert(sizeof(ntb0_io_status_block) == 0x10, "IO_STATUS_BLOCK size");
_Static_assert(offsetof(ntb0_io_status_block, information) == 0x08,
               "IO_STATUS_BLOCK.Information");

/* ------------------------------------------------------------------ */
/* ACCESS_MASK (Win32)                                                 */
/* ------------------------------------------------------------------ */

#define NTB0_FILE_READ_DATA         0x00000001u
#define NTB0_FILE_WRITE_DATA        0x00000002u
#define NTB0_FILE_APPEND_DATA       0x00000004u
#define NTB0_FILE_READ_EA           0x00000008u
#define NTB0_FILE_WRITE_EA          0x00000010u
#define NTB0_FILE_EXECUTE           0x00000020u
#define NTB0_FILE_DELETE_CHILD      0x00000040u
#define NTB0_FILE_READ_ATTRIBUTES   0x00000080u
#define NTB0_FILE_WRITE_ATTRIBUTES  0x00000100u
#define NTB0_DELETE                 0x00010000u
#define NTB0_READ_CONTROL           0x00020000u
#define NTB0_WRITE_DAC              0x00040000u
#define NTB0_WRITE_OWNER            0x00080000u
#define NTB0_SYNCHRONIZE            0x00100000u
#define NTB0_GENERIC_EXECUTE        0x20000000u
#define NTB0_GENERIC_WRITE          0x40000000u
#define NTB0_GENERIC_READ           0x80000000u

/* ------------------------------------------------------------------ */
/* ShareAccess                                                         */
/* ------------------------------------------------------------------ */

#define NTB0_FILE_SHARE_READ   0x00000001u
#define NTB0_FILE_SHARE_WRITE  0x00000002u
#define NTB0_FILE_SHARE_DELETE 0x00000004u

/* ------------------------------------------------------------------ */
/* CreateDisposition                                                   */
/* ------------------------------------------------------------------ */

#define NTB0_FILE_SUPERSEDE     0u
#define NTB0_FILE_OPEN          1u
#define NTB0_FILE_CREATE        2u
#define NTB0_FILE_OPEN_IF       3u
#define NTB0_FILE_OVERWRITE     4u
#define NTB0_FILE_OVERWRITE_IF  5u

/* ------------------------------------------------------------------ */
/* CreateOptions                                                       */
/* ------------------------------------------------------------------ */

#define NTB0_FILE_DIRECTORY_FILE          0x00000001u
#define NTB0_FILE_WRITE_THROUGH           0x00000002u
#define NTB0_FILE_SEQUENTIAL_ONLY         0x00000004u
#define NTB0_FILE_SYNCHRONOUS_IO_NONALERT 0x00000020u
#define NTB0_FILE_NON_DIRECTORY_FILE      0x00000040u
#define NTB0_FILE_OPEN_FOR_BACKUP_INTENT  0x00004000u
#define NTB0_FILE_OPEN_REPARSE_POINT      0x20000000u

/* ------------------------------------------------------------------ */
/* NTSTATUS                                                            */
/* ------------------------------------------------------------------ */

#define NTB0_ST_SUCCESS                0x00000000L
#define NTB0_ST_UNSUCCESSFUL           0xC0000001L
#define NTB0_ST_NOT_IMPLEMENTED        0xC0000002L
#define NTB0_ST_INVALID_HANDLE         0xC0000008L
#define NTB0_ST_INVALID_PARAMETER      0xC000000DL
#define NTB0_ST_END_OF_FILE            0xC0000011L
#define NTB0_ST_NO_MEMORY              0xC0000017L
#define NTB0_ST_ACCESS_DENIED          0xC0000022L
#define NTB0_ST_STATUS_LOGICAL_ERROR   0xC0000023L
#define NTB0_ST_OBJECT_NAME_NOT_FOUND  0xC0000034L
#define NTB0_ST_OBJECT_NAME_COLLISION  0xC0000035L
#define NTB0_ST_OBJECT_PATH_NOT_FOUND  0xC000003AL
#define NTB0_ST_OBJECT_PATH_SYNTAX_BAD 0xC000003BL
#define NTB0_ST_SHARING_VIOLATION      0xC0000043L
#define NTB0_ST_NOT_SUPPORTED          0xC00000BBL
#define NTB0_ST_FILE_IS_A_DIRECTORY    0xC00000BAL
#define NTB0_ST_INSUFFICIENT_RESOURCES 0xC000009AL
#define NTB0_ST_DEVICE_NOT_CONNECTED   0xC000009DL
#define NTB0_ST_NOT_A_DIRECTORY        0xC0000103L
#define NTB0_ST_OBJECT_NAME_INVALID    0xC0000033L
#define NTB0_ST_TIMEOUT                0x00000102L
#define NTB0_ST_NO_MORE_FILES          0x80000006L
#define NTB0_ST_INVALID_INFO_CLASS     0xC0000003L
#define NTB0_ST_INVALID_DEVICE_REQUEST 0xC0000010L
#define NTB0_ST_OBJECT_TYPE_MISMATCH   0xC0000024L
#define NTB0_ST_PRIVILEGE_NOT_HELD     0xC0000061L
#define NTB0_ST_PIPE_BUSY              0xC00000AEL
#define NTB0_ST_PIPE_CONNECTED         0xC00000B0L
#define NTB0_ST_PIPE_DISCONNECTED      0xC00000B1L
#define NTB0_ST_BAD_IMPERSONATION_LEVEL 0xC00000B6L

/* ------------------------------------------------------------------ */
/* File attributes (Windows)                                           */
/* ------------------------------------------------------------------ */

#define NTB0_FILE_ATTRIBUTE_READONLY      0x00000001u
#define NTB0_FILE_ATTRIBUTE_DIRECTORY     0x00000010u
#define NTB0_FILE_ATTRIBUTE_NORMAL        0x00000080u
#define NTB0_FILE_ATTRIBUTE_REPARSE_POINT 0x00000400u

/* ------------------------------------------------------------------ */
/* Thread information classes (THREADINFOCLASS)                         */
/* ------------------------------------------------------------------ */

#define NTB0_TIC_BASIC          0u
#define NTB0_TIC_TIMES          1u
#define NTB0_TIC_PRIORITY       2u
#define NTB0_TIC_BASE_PRIORITY  3u
#define NTB0_TIC_DEBUG_OBJECT   4u
#define NTB0_TIC_TEB_POINTER    5u
#define NTB0_TIC_AFFINITY_MASK  6u
#define NTB0_TIC_IMPERSONATION  7u
#define NTB0_TIC_WEEKEND_MODE   8u
#define NTB0_TIC_HIDE_DEBUGGER  9u
#define NTB0_TIC_IDEAL_PROCESSOR 10u
#define NTB0_TIC_EXTENDED_CID   11u

/* ------------------------------------------------------------------ */
/* System information classes (SYSTEM_INFORMATION_CLASS)               */
/* ------------------------------------------------------------------ */

#define NTB0_SIC_BASIC                0u
#define NTB0_SIC_PROCESSOR            1u
#define NTB0_SIC_PERFORMANCE          2u
#define NTB0_SIC_TIMEOFDAY            3u
#define NTB0_SIC_TIME_ADJUSTMENT      4u
#define NTB0_SIC_SHUTDOWN             5u
#define NTB0_SIC_MAX_SIZE             6u
#define NTB0_SIC_NATIVE               7u
#define NTB0_SIC_BOOT_TIME            8u
#define NTB0_SIC_PERF_COUNTER         9u
#define NTB0_SIC_EXCEPTION            10u
#define NTB0_SIC_CRASH_DUMP           11u
#define NTB0_SIC_KERNEL_DEBUGGER      12u
#define NTB0_SIC_CONTEXT_SWITCH       13u
#define NTB0_SIC_REGISTRY_QUOTA       14u
#define NTB0_SIC_EXTEND_SERVICE_TABLE 15u
#define NTB0_SIC_PRIORITY_SEPARATION  16u
#define NTB0_SIC_THREAD_PRIORITY_CID  22u
#define NTB0_SIC_DEBUGGER             35u

#endif /* NTB0_NT_H */
