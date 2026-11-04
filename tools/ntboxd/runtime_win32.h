/*
 * runtime_win32.h — بيئة تشغيل Win32 الدنيا لتشغيل cmd.exe/notepad.exe.
 *
 * المصابيح: RTW-001..RTW-010
 */
#ifndef NTB0_RUNTIME_WIN32_H
#define NTB0_RUNTIME_WIN32_H

#include <stddef.h>
#include <stdint.h>

#include "peb.h"

struct ntb0_dll_loader;
struct ntb0_dll_module;

/* ------------------------------------------------------------------ */
/* أنواع Win32 الأساسية                                               */
/* ------------------------------------------------------------------ */
typedef void      *HANDLE;
typedef uint32_t   DWORD;
typedef uint32_t   ULONG;
typedef int32_t    LONG;
typedef uint16_t   WORD;
typedef uint16_t   USHORT;
typedef int        BOOL;
typedef uintptr_t  UINT_PTR;
typedef intptr_t   LONG_PTR;
typedef unsigned char BYTE;
typedef size_t     SIZE_T;
typedef void      *LPVOID;
typedef const void *LPCVOID;
typedef uint64_t   ULONGLONG;

#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE  ((DWORD)-12)

#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)

/* CONSOLE_SCREEN_BUFFER_INFO */
typedef struct _CONSOLE_SCREEN_BUFFER_INFO {
    DWORD dwSizeX;
    DWORD dwSizeY;
    DWORD dwCursorPositionX;
    DWORD dwCursorPositionY;
    WORD  wAttributes;
    DWORD srWindowLeft;
    DWORD srWindowTop;
    DWORD srWindowRight;
    DWORD srWindowBottom;
    DWORD dwMaximumWindowSizeX;
    DWORD dwMaximumWindowSizeY;
} CONSOLE_SCREEN_BUFFER_INFO;

/* ------------------------------------------------------------------ */
/* Console API declarations                                           */
/* ------------------------------------------------------------------ */
HANDLE __attribute__((ms_abi)) ntb0_GetStdHandle(DWORD std_handle);
BOOL   __attribute__((ms_abi)) ntb0_WriteFile(HANDLE hFile, LPCVOID lpBuffer,
                                               DWORD nNumberOfBytesToWrite,
                                               DWORD *lpNumberOfBytesWritten,
                                               void *lpOverlapped);
BOOL   __attribute__((ms_abi)) ntb0_ReadFile(HANDLE hFile, LPVOID lpBuffer,
                                              DWORD nNumberOfBytesToRead,
                                              DWORD *lpNumberOfBytesRead,
                                              void *lpOverlapped);
BOOL   __attribute__((ms_abi)) ntb0_WriteConsoleA(HANDLE hConsoleOutput,
                                                   const void *lpBuffer,
                                                   DWORD nNumberOfCharsToWrite,
                                                   DWORD *lpNumberOfCharsWritten,
                                                   void *lpReserved);
BOOL   __attribute__((ms_abi)) ntb0_WriteConsoleW(HANDLE hConsoleOutput,
                                                   const void *lpBuffer,
                                                   DWORD nNumberOfCharsToWrite,
                                                   DWORD *lpNumberOfCharsWritten,
                                                   void *lpReserved);
BOOL   __attribute__((ms_abi)) ntb0_ReadConsoleA(HANDLE hConsoleInput,
                                                  void *lpBuffer,
                                                  DWORD nNumberOfCharsToRead,
                                                  DWORD *lpNumberOfCharsRead,
                                                  void *lpReserved);
BOOL   __attribute__((ms_abi)) ntb0_ReadConsoleW(HANDLE hConsoleInput,
                                                  void *lpBuffer,
                                                  DWORD nNumberOfCharsToRead,
                                                  DWORD *lpNumberOfCharsRead,
                                                  void *lpReserved);
BOOL   __attribute__((ms_abi)) ntb0_GetConsoleMode(HANDLE hConsoleHandle,
                                                    DWORD *lpMode);
BOOL   __attribute__((ms_abi)) ntb0_SetConsoleMode(HANDLE hConsoleHandle,
                                                    DWORD dwMode);
BOOL   __attribute__((ms_abi)) ntb0_AllocConsole(void);
BOOL   __attribute__((ms_abi)) ntb0_FreeConsole(void);
BOOL   __attribute__((ms_abi)) ntb0_AttachConsole(DWORD dwProcessId);
BOOL   __attribute__((ms_abi)) ntb0_GetConsoleScreenBufferInfo(HANDLE hConsoleOutput,
                                                                CONSOLE_SCREEN_BUFFER_INFO *lpConsoleScreenBufferInfo);
BOOL   __attribute__((ms_abi)) ntb0_SetConsoleCursorPosition(HANDLE hConsoleOutput,
                                                              DWORD dwCursorPosition);

/* ------------------------------------------------------------------ */
/* واجهة Runtime                                                       */
/* ------------------------------------------------------------------ */

/*
 * ntb0_runtime_win32_init — يهيئ بيئة Win32:
 *   - PEB: ImageBaseAddress, Ldr, ProcessParameters (CommandLine,
 *     CurrentDirectory, Environment), NtGlobalFlag
 *   - TEB: NtTib.Self, ThreadLocalStoragePointer, StackBase/Limit,
 *     ProcessEnvironmentBlock, LastErrorValue, ClientId
 *   - يستدعي DllMain للكرّرة (kernel32.dll) مع DLL_PROCESS_ATTACH
 *   - يشغّل TLS callbacks للوحدات المحمّلة
 *
 * المعاملات:
 *   ld       : المحمّل (يحتوي kernel32.dll مُحمّلاً)
 *   argc     : عدد المعاملات (لـ CommandLine)
 *   argv     : معاملات سطر الأوامر
 *   envp     : متغيرات البيئة (NULL ⇒ البيئة الحالية)
 *
 * يُعيد 0 عند النجاح، أو رمز خطأ سالب.
 */
int ntb0_runtime_win32_init(struct ntb0_dll_loader *ld,
                            int argc, char **argv, char **envp);

/*
 * ntb0_runtime_win32_console_init — ينفّذ واجهات الكونسول:
 *   GetStdHandle, WriteFile, ReadFile, WriteConsoleA/W,
 *   ReadConsoleA/W, GetConsoleMode, SetConsoleMode,
 *   AllocConsole, FreeConsole, AttachConsole,
 *   GetConsoleScreenBufferInfo, SetConsoleCursorPosition
 *
 * هذه الدوال تُربط كـ "stubs" حقيقية توجه إلى stdin/stdout/stderr
 * للمضيف.
 *
 * يُعيد 0 عند النجاح.
 */
int ntb0_runtime_win32_console_init(void);

/*
 * ntb0_runtime_win32_seh_init — يسجّل معالج SEH على مستوى
 * العملية (SetUnhandledExceptionFilter).
 *
 * يُعيد المُرشّح السابق (قد يكون NULL).
 */
void *ntb0_runtime_win32_seh_init(void);

/*
 * ntb0_runtime_win32_loader_fixups — يطبق تصحيحات المحمّل:
 *   - دليل TLS (TLS directory): ينفّذ الـ callbacks
 *   - delay-load imports
 *   - bound imports
 *
 * يجب استدعاؤه بعد تحميل جميع الوحدات وقبل الدخول إلى نقطة
 * بداية البرنامج الرئيسي (exe).
 *
 * يُعيد 0 عند النجاح.
 */
int ntb0_runtime_win32_loader_fixups(struct ntb0_dll_loader *ld);

/* ------------------------------------------------------------------ */
/* دوال مساعدة للاختبار                                                */
/* ------------------------------------------------------------------ */

/*
 * ntb0_runtime_win32_get_kernel32 — يعيد وحدة kernel32 المحمّلة
 * من المحمّل (NULL إن لم تُحمّل).
 */
struct ntb0_dll_module *ntb0_runtime_win32_get_kernel32(struct ntb0_dll_loader *ld);

/*
 * ntb0_runtime_win32_get_peb — يعيد الـ PEB الحالي (بعد التهيئة).
 */
PEB *ntb0_runtime_win32_get_peb(void);

/*
 * ntb0_runtime_win32_get_teb — يعيد الـ TEB الحالي (بعد التهيئة).
 */
TEB *ntb0_runtime_win32_get_teb(void);

/*
 * ntb0_runtime_win32_call_entry — يستدعي نقطة بداية exe
 * (عنوان الدخول من PE) مع المعاملات القياسية.
 *
 * لا يعود عادةً (يُستدعي ExitProcess داخلياً).
 * يُعيد كود الخروج إن عاد.
 */
int ntb0_runtime_win32_call_entry(struct ntb0_dll_module *exe_mod,
                                  int argc, char **argv);

/* ------------------------------------------------------------------ */
/* سجل الأخطاء (للاختبار)                                             */
/* ------------------------------------------------------------------ */
const char *ntb0_runtime_win32_last_error(void);

#endif /* NTB0_RUNTIME_WIN32_H */