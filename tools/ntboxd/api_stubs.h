/*
 * api_stubs.h — Declarations for comprehensive Win32 API stubs
 */

#ifndef API_STUBS_H
#define API_STUBS_H

#include <stdint.h>

/* Process Management */
void     *__attribute__((ms_abi)) ntboxd_GetCurrentProcess(void);
uint32_t __attribute__((ms_abi)) ntboxd_GetCurrentProcessId(void);
int      __attribute__((ms_abi)) ntboxd_GetExitCodeProcess(void *hProcess, uint32_t *lpExitCode);
int      __attribute__((ms_abi)) ntboxd_CreateProcessA(const char *lpApplicationName, char *lpCommandLine, void *lpProcessAttributes, void *lpThreadAttributes, int bInheritHandles, uint32_t dwCreationFlags, void *lpEnvironment, const char *lpCurrentDirectory, void *lpStartupInfo, void *lpProcessInformation);
int      __attribute__((ms_abi)) ntboxd_CreateProcessW(const uint16_t *lpApplicationName, uint16_t *lpCommandLine, void *lpProcessAttributes, void *lpThreadAttributes, int bInheritHandles, uint32_t dwCreationFlags, void *lpEnvironment, const uint16_t *lpCurrentDirectory, void *lpStartupInfo, void *lpProcessInformation);
int      __attribute__((ms_abi)) ntboxd_TerminateProcess(void *hProcess, uint32_t uExitCode);
void     *__attribute__((ms_abi)) ntboxd_OpenProcess(uint32_t dwDesiredAccess, int bInheritHandle, uint32_t dwProcessId);

/* Thread Management */
void     *__attribute__((ms_abi)) ntboxd_GetCurrentThread(void);
uint32_t __attribute__((ms_abi)) ntboxd_GetCurrentThreadId(void);
int      __attribute__((ms_abi)) ntboxd_GetExitCodeThread(void *hThread, uint32_t *lpExitCode);
uint32_t __attribute__((ms_abi)) ntboxd_ExitThread(uint32_t dwExitCode);
int      __attribute__((ms_abi)) ntboxd_SetThreadPriority(void *hThread, int nPriority);
uint32_t __attribute__((ms_abi)) ntboxd_ResumeThread(void *hThread);
uint32_t __attribute__((ms_abi)) ntboxd_SuspendThread(void *hThread);

/* Memory Management */
void     *__attribute__((ms_abi)) ntboxd_HeapCreate(uint32_t flOptions, size_t dwInitialSize, size_t dwMaximumSize);
int      __attribute__((ms_abi)) ntboxd_HeapDestroy(void *hHeap);
size_t   __attribute__((ms_abi)) ntboxd_HeapSize(void *hHeap, uint32_t dwFlags, void *lpMem);
int      __attribute__((ms_abi)) ntboxd_HeapValidate(void *hHeap, uint32_t dwFlags, void *lpMem);
size_t   __attribute__((ms_abi)) ntboxd_HeapCompact(void *hHeap, uint32_t dwFlags);
int      __attribute__((ms_abi)) ntboxd_HeapWalk(void *hHeap, void *lpEntry);
void     *__attribute__((ms_abi)) ntboxd_LocalAlloc(uint32_t uFlags, size_t uBytes);
void     *__attribute__((ms_abi)) ntboxd_LocalFree(void *hMem);
void     *__attribute__((ms_abi)) ntboxd_LocalReAlloc(void *hMem, size_t uBytes, uint32_t uFlags);
size_t   __attribute__((ms_abi)) ntboxd_LocalSize(void *hMem);
void     *__attribute__((ms_abi)) ntboxd_GlobalAlloc(uint32_t uFlags, size_t dwBytes);
void     *__attribute__((ms_abi)) ntboxd_GlobalFree(void *hMem);
void     *__attribute__((ms_abi)) ntboxd_GlobalReAlloc(void *hMem, size_t dwBytes, uint32_t uFlags);
size_t   __attribute__((ms_abi)) ntboxd_GlobalSize(void *hMem);

/* Handle Management */
int      __attribute__((ms_abi)) ntboxd_DuplicateHandle(void *hSourceProcessHandle, void *hSourceHandle, void *hTargetProcessHandle, void *lpTargetHandle, uint32_t dwDesiredAccess, int bInheritHandle, uint32_t dwOptions);

/* Environment & Registry */
uint32_t __attribute__((ms_abi)) ntboxd_GetEnvironmentVariableA(const char *lpName, char *lpBuffer, uint32_t nSize);
uint32_t __attribute__((ms_abi)) ntboxd_GetEnvironmentVariableW(const uint16_t *lpName, uint16_t *lpBuffer, uint32_t nSize);
int      __attribute__((ms_abi)) ntboxd_SetEnvironmentVariableA(const char *lpName, const char *lpValue);
int      __attribute__((ms_abi)) ntboxd_SetEnvironmentVariableW(const uint16_t *lpName, const uint16_t *lpValue);
uint32_t __attribute__((ms_abi)) ntboxd_ExpandEnvironmentStringsA(const char *lpSrc, char *lpDst, uint32_t nSize);
uint32_t __attribute__((ms_abi)) ntboxd_ExpandEnvironmentStringsW(const uint16_t *lpSrc, uint16_t *lpDst, uint32_t nSize);
uint32_t __attribute__((ms_abi)) ntboxd_GetCurrentDirectoryA(uint32_t nBufferLength, char *lpBuffer);
uint32_t __attribute__((ms_abi)) ntboxd_GetCurrentDirectoryW(uint32_t nBufferLength, uint16_t *lpBuffer);
int      __attribute__((ms_abi)) ntboxd_SetCurrentDirectoryA(const char *lpPathName);
int      __attribute__((ms_abi)) ntboxd_SetCurrentDirectoryW(const uint16_t *lpPathName);

/* Error Handling */
uint32_t __attribute__((ms_abi)) ntboxd_GetLastError(void);
void     __attribute__((ms_abi)) ntboxd_SetLastError(uint32_t dwErrCode);
int32_t  __attribute__((ms_abi)) ntboxd_UnhandledExceptionFilter(void *lpTopLevelExceptionInfo);
void     *__attribute__((ms_abi)) ntboxd_SetUnhandledExceptionFilter(void *lpTopLevelExceptionHandler);
uint32_t __attribute__((ms_abi)) ntboxd_SetErrorMode(uint32_t uMode);

/* String Conversion */
int      __attribute__((ms_abi)) ntboxd_MultiByteToWideChar(uint32_t CodePage, uint32_t dwFlags, const char *lpMultiByteStr, int cbMultiByte, uint16_t *lpWideCharStr, int cchWideChar);
int      __attribute__((ms_abi)) ntboxd_WideCharToMultiByte(uint32_t CodePage, uint32_t dwFlags, const uint16_t *lpWideCharStr, int cchWideChar, char *lpMultiByteStr, int cbMultiByte, const char *lpDefaultChar, unsigned char *lpUsedDefaultChar);

/* System Information */
uint32_t __attribute__((ms_abi)) ntboxd_GetTickCount(void);
void     __attribute__((ms_abi)) ntboxd_GetSystemTimeAsFileTime(uint64_t *lpSystemTimeAsFileTime);
int      __attribute__((ms_abi)) ntboxd_QueryPerformanceCounter(uint64_t *lpPerformanceCount);
int      __attribute__((ms_abi)) ntboxd_QueryPerformanceFrequency(uint64_t *lpFrequency);
void     __attribute__((ms_abi)) ntboxd_GetSystemInfo(void *lpSystemInfo);
void     __attribute__((ms_abi)) ntboxd_GetLocalTime(void *lpSystemTime);
int      __attribute__((ms_abi)) ntboxd_SetLocalTime(const void *lpSystemTime);
int      __attribute__((ms_abi)) ntboxd_GetVersionExA(void *lpVersionInformation);
int      __attribute__((ms_abi)) ntboxd_GetVersionExW(void *lpVersionInformation);

/* Interlocked Operations */
int32_t  __attribute__((ms_abi)) ntboxd_InterlockedIncrement(int32_t *lpAddend);
int32_t  __attribute__((ms_abi)) ntboxd_InterlockedDecrement(int32_t *lpAddend);
int32_t  __attribute__((ms_abi)) ntboxd_InterlockedExchange(int32_t *Target, int32_t Value);
int32_t  __attribute__((ms_abi)) ntboxd_InterlockedCompareExchange(int32_t *Destination, int32_t Exchange, int32_t Comperand);
uint64_t __attribute__((ms_abi)) ntboxd_InterlockedCompareExchange64(uint64_t *Destination, uint64_t Exchange, uint64_t Comperand);

/* Directory/Path Operations */
uint32_t __attribute__((ms_abi)) ntboxd_GetSystemDirectoryA(char *lpBuffer, uint32_t uSize);
uint32_t __attribute__((ms_abi)) ntboxd_GetSystemDirectoryW(uint16_t *lpBuffer, uint32_t uSize);
uint32_t __attribute__((ms_abi)) ntboxd_GetWindowsDirectoryA(char *lpBuffer, uint32_t uSize);
uint32_t __attribute__((ms_abi)) ntboxd_GetWindowsDirectoryW(uint16_t *lpBuffer, uint32_t uSize);

/* DLL Loading & Module Management */
uint32_t __attribute__((ms_abi)) ntboxd_GetModuleFileNameA(void *hModule, char *lpFilename, uint32_t nSize);
uint32_t __attribute__((ms_abi)) ntboxd_GetModuleFileNameW(void *hModule, uint16_t *lpFilename, uint32_t nSize);
int      __attribute__((ms_abi)) ntboxd_GetModuleHandleExA(uint32_t dwFlags, const char *lpModuleName, void **phModule);
int      __attribute__((ms_abi)) ntboxd_GetModuleHandleExW(uint32_t dwFlags, const uint16_t *lpModuleName, void **phModule);
void     __attribute__((ms_abi)) ntboxd_FreeLibraryAndExitThread(void *hLibModule, uint32_t dwExitCode);
int      __attribute__((ms_abi)) ntboxd_DisableThreadLibraryCalls(void *hLibModule);
void     *__attribute__((ms_abi)) ntboxd_LoadLibraryExA(const char *lpLibFileName, void *hFile, uint32_t dwFlags);
void     *__attribute__((ms_abi)) ntboxd_LoadLibraryExW(const uint16_t *lpLibFileName, void *hFile, uint32_t dwFlags);
void     *__attribute__((ms_abi)) ntboxd_LoadLibraryW(const uint16_t *lpLibFileName);

/* Misc Utilities */
int      __attribute__((ms_abi)) ntboxd_MulDiv(int nNumber, int nNumerator, int nDenominator);
int      __attribute__((ms_abi)) ntboxd_IsDBCSLeadByte(unsigned char TestChar);
uint32_t __attribute__((ms_abi)) ntboxd_GetACP(void);
uint32_t __attribute__((ms_abi)) ntboxd_GetOEMCP(void);
uint32_t __attribute__((ms_abi)) ntboxd_GetUserDefaultLCID(void);
int      __attribute__((ms_abi)) ntboxd_IsValidLocale(uint32_t Locale, uint32_t dwFlags);
uint32_t __attribute__((ms_abi)) ntboxd_ConvertDefaultLocale(uint32_t Locale);

/* Registry Operations */
int32_t  __attribute__((ms_abi)) ntboxd_RegQueryValueExA(void *hKey, const char *lpValueName, uint32_t *lpReserved, uint32_t *lpType, unsigned char *lpData, uint32_t *lpcbData);
int32_t  __attribute__((ms_abi)) ntboxd_RegQueryValueExW(void *hKey, const uint16_t *lpValueName, uint32_t *lpReserved, uint32_t *lpType, unsigned char *lpData, uint32_t *lpcbData);
int32_t  __attribute__((ms_abi)) ntboxd_RegEnumValueA(void *hKey, uint32_t dwIndex, char *lpValueName, uint32_t *lpcchValueName, uint32_t *lpReserved, uint32_t *lpType, unsigned char *lpData, uint32_t *lpcbData);
int32_t  __attribute__((ms_abi)) ntboxd_RegEnumValueW(void *hKey, uint32_t dwIndex, uint16_t *lpValueName, uint32_t *lpcchValueName, uint32_t *lpReserved, uint32_t *lpType, unsigned char *lpData, uint32_t *lpcbData);
int32_t  __attribute__((ms_abi)) ntboxd_RegQueryInfoKeyA(void *hKey, char *lpClass, uint32_t *lpcchClass, uint32_t *lpReserved, uint32_t *lpcSubKeys, uint32_t *lpcbMaxSubKeyLen, uint32_t *lpcbMaxClassLen, uint32_t *lpcValues, uint32_t *lpcbMaxValueNameLen, uint32_t *lpcbMaxValueLen, uint32_t *lpcbSecurityDescriptor, void *lpftLastWriteTime);
int32_t  __attribute__((ms_abi)) ntboxd_RegQueryInfoKeyW(void *hKey, uint16_t *lpClass, uint32_t *lpcchClass, uint32_t *lpReserved, uint32_t *lpcSubKeys, uint32_t *lpcbMaxSubKeyLen, uint32_t *lpcbMaxClassLen, uint32_t *lpcValues, uint32_t *lpcbMaxValueNameLen, uint32_t *lpcbMaxValueLen, uint32_t *lpcbSecurityDescriptor, void *lpftLastWriteTime);

#endif /* API_STUBS_H */
