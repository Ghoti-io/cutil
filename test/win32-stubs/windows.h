/*
 * Minimal stand-ins for the Win32 declarations this library's headers use.
 *
 * NOT a Windows SDK and not used by any Windows build.  Its only job is to
 * let a Linux compiler *parse* the `#ifdef _WIN32` branches, which are
 * otherwise never tokenised here -- a syntax error in one of them survives
 * indefinitely, which is exactly how GCU_MAYBE_UNUSED's Windows branch stayed
 * a syntax error from the initial commit until 2026-09-22.
 *
 * This catches the syntax-error class and nothing else.  It does not check
 * that the real API has these signatures, and it cannot check semantics.  A
 * stub whose signature drifts from the SDK's would parse and still be wrong,
 * so keep these declarations copied from the documented prototypes.
 */

#ifndef GHOTI_IO_GCU_WIN32_STUBS_WINDOWS_H
#define GHOTI_IO_GCU_WIN32_STUBS_WINDOWS_H

#ifndef _WIN32
#error "win32-stubs/windows.h included without _WIN32; this is a parse check only"
#endif

typedef struct _GCU_STUB_SRWLOCK { void * Ptr; } SRWLOCK;

void InitializeSRWLock(SRWLOCK * SRWLock);
void AcquireSRWLockExclusive(SRWLOCK * SRWLock);
void ReleaseSRWLockExclusive(SRWLOCK * SRWLock);
unsigned char TryAcquireSRWLockExclusive(SRWLOCK * SRWLock);

#endif // GHOTI_IO_GCU_WIN32_STUBS_WINDOWS_H

/* --- appended for cond.h / cond.c --- */
#ifndef GHOTI_IO_GCU_WIN32_STUBS_COND
#define GHOTI_IO_GCU_WIN32_STUBS_COND

typedef unsigned long DWORD;
typedef int BOOL;

#define TRUE  1
#define FALSE 0

#define INFINITE      0xFFFFFFFFUL
#define ERROR_TIMEOUT 1460UL

DWORD GetLastError(void);

typedef struct _GCU_STUB_CONDITION_VARIABLE { void * Ptr; } CONDITION_VARIABLE;

void InitializeConditionVariable(CONDITION_VARIABLE * ConditionVariable);
void WakeConditionVariable(CONDITION_VARIABLE * ConditionVariable);
void WakeAllConditionVariable(CONDITION_VARIABLE * ConditionVariable);
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE * ConditionVariable,
    SRWLOCK * SRWLock, DWORD dwMilliseconds, unsigned long Flags);

#endif

/* --- appended for once.h / once.c --- */
#ifndef GHOTI_IO_GCU_WIN32_STUBS_ONCE
#define GHOTI_IO_GCU_WIN32_STUBS_ONCE

#define CALLBACK
#define INIT_ONCE_STATIC_INIT { 0 }

typedef void * PVOID;
typedef struct _GCU_STUB_INIT_ONCE { void * Ptr; } INIT_ONCE, * PINIT_ONCE;

typedef BOOL (CALLBACK * PINIT_ONCE_FN)(PINIT_ONCE, PVOID, PVOID *);

BOOL InitOnceExecuteOnce(PINIT_ONCE InitOnce, PINIT_ONCE_FN InitFn,
    PVOID Parameter, PVOID * Context);

#endif

/* --- appended for rwlock.h / rwlock.c --- */
#ifndef GHOTI_IO_GCU_WIN32_STUBS_RWLOCK
#define GHOTI_IO_GCU_WIN32_STUBS_RWLOCK
void AcquireSRWLockShared(SRWLOCK * SRWLock);
void ReleaseSRWLockShared(SRWLOCK * SRWLock);
unsigned char TryAcquireSRWLockShared(SRWLOCK * SRWLock);
#endif

/* --- appended for error.c --- */
#ifndef GHOTI_IO_GCU_WIN32_STUBS_ERROR
#define GHOTI_IO_GCU_WIN32_STUBS_ERROR
#define FORMAT_MESSAGE_FROM_SYSTEM     0x00001000UL
#define FORMAT_MESSAGE_IGNORE_INSERTS  0x00000200UL
typedef void * LPVOID;
typedef char * LPSTR;
DWORD FormatMessageA(DWORD dwFlags, const void * lpSource, DWORD dwMessageId,
    DWORD dwLanguageId, LPSTR lpBuffer, DWORD nSize, void * Arguments);
#endif

/* --- appended for tls.c --- */
#ifndef GHOTI_IO_GCU_WIN32_STUBS_TLS
#define GHOTI_IO_GCU_WIN32_STUBS_TLS
#define FLS_OUT_OF_INDEXES 0xFFFFFFFFUL
typedef void (* PFLS_CALLBACK_FUNCTION)(PVOID);
DWORD FlsAlloc(PFLS_CALLBACK_FUNCTION lpCallback);
BOOL  FlsFree(DWORD dwFlsIndex);
PVOID FlsGetValue(DWORD dwFlsIndex);
BOOL  FlsSetValue(DWORD dwFlsIndex, PVOID lpFlsData);
#endif

/* --- appended for env.c --- */
#ifndef GHOTI_IO_GCU_WIN32_STUBS_ENV
#define GHOTI_IO_GCU_WIN32_STUBS_ENV
#define ERROR_SUCCESS            0UL
#define ERROR_ENVVAR_NOT_FOUND 203UL
typedef unsigned short WCHAR;
typedef const WCHAR * LPCWSTR;
typedef WCHAR * LPWSTR;
void  SetLastError(DWORD dwErrCode);
DWORD GetEnvironmentVariableW(LPCWSTR lpName, LPWSTR lpBuffer, DWORD nSize);
BOOL  SetEnvironmentVariableW(LPCWSTR lpName, LPCWSTR lpValue);
// _wputenv_s is the C runtime's, not Win32's -- it comes from <stdlib.h> on
// Windows, and is declared here because this file is the only stand-in the
// parse check has.  It returns an errno value, zero on success; set_raw()
// compares against zero for that reason and not because it is a BOOL.
typedef int errno_t;
errno_t _wputenv_s(const wchar_t * varname, const wchar_t * value_string);
#endif

/* --- appended for library.c --- */
#ifndef GHOTI_IO_GCU_WIN32_STUBS_LIBRARY
#define GHOTI_IO_GCU_WIN32_STUBS_LIBRARY
#define ERROR_INVALID_NAME 123UL
typedef struct _GCU_STUB_HINSTANCE * HMODULE;
typedef int (* FARPROC)(void);
HMODULE LoadLibraryW(LPCWSTR lpLibFileName);
BOOL    FreeLibrary(HMODULE hLibModule);
FARPROC GetProcAddress(HMODULE hModule, const char * lpProcName);
#endif

/* --- appended for filelock.c --- */
#ifndef GHOTI_IO_GCU_WIN32_STUBS_FILELOCK
#define GHOTI_IO_GCU_WIN32_STUBS_FILELOCK
#define GENERIC_READ              0x80000000UL
#define GENERIC_WRITE             0x40000000UL
#define FILE_SHARE_READ           0x00000001UL
#define FILE_SHARE_WRITE          0x00000002UL
#define OPEN_ALWAYS               4UL
#define FILE_ATTRIBUTE_NORMAL     0x80UL
#define LOCKFILE_FAIL_IMMEDIATELY 0x00000001UL
#define LOCKFILE_EXCLUSIVE_LOCK   0x00000002UL
#define ERROR_LOCK_VIOLATION      33UL
#define MAXDWORD                  0xFFFFFFFFUL
#define INVALID_HANDLE_VALUE      ((HANDLE)(long)-1)
#define ZeroMemory(d, l)          gcu_stub_zero((d), (l))
typedef void * HANDLE;
typedef struct _GCU_STUB_OVERLAPPED { unsigned long Internal; } OVERLAPPED;
void gcu_stub_zero(void * destination, unsigned long length);
HANDLE CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess,
    DWORD dwShareMode, void * lpSecurityAttributes, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
BOOL CloseHandle(HANDLE hObject);
BOOL LockFileEx(HANDLE hFile, DWORD dwFlags, DWORD dwReserved,
    DWORD nNumberOfBytesToLockLow, DWORD nNumberOfBytesToLockHigh,
    OVERLAPPED * lpOverlapped);
BOOL UnlockFileEx(HANDLE hFile, DWORD dwReserved,
    DWORD nNumberOfBytesToUnlockLow, DWORD nNumberOfBytesToUnlockHigh,
    OVERLAPPED * lpOverlapped);
#endif

/* --- appended for mmap.c --- */
#ifndef GHOTI_IO_GCU_WIN32_STUBS_MMAP
#define GHOTI_IO_GCU_WIN32_STUBS_MMAP
#define OPEN_EXISTING   3UL
#define ERROR_ACCESS_DENIED 5UL
#define PAGE_READONLY   0x02UL
#define PAGE_READWRITE  0x04UL
#define FILE_MAP_READ   0x0004UL
#define FILE_MAP_WRITE  0x0002UL
typedef union _GCU_STUB_LARGE_INTEGER { long long QuadPart; } LARGE_INTEGER;
BOOL   GetFileSizeEx(HANDLE hFile, LARGE_INTEGER * lpFileSize);
HANDLE CreateFileMappingW(HANDLE hFile, void * lpAttributes,
    DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow,
    LPCWSTR lpName);
LPVOID MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess,
    DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow, size_t dwNumberOfBytesToMap);
BOOL   UnmapViewOfFile(LPVOID lpBaseAddress);
BOOL   FlushViewOfFile(LPVOID lpBaseAddress, size_t dwNumberOfBytesToFlush);
BOOL   FlushFileBuffers(HANDLE hFile);
#endif

/* --- appended for subprocess.c --- */
#ifndef GHOTI_IO_GCU_WIN32_STUBS_SUBPROCESS
#define GHOTI_IO_GCU_WIN32_STUBS_SUBPROCESS
#define WINAPI
#define ERROR_INVALID_PARAMETER     87UL
#define HANDLE_FLAG_INHERIT         0x00000001UL
#define STARTF_USESTDHANDLES        0x00000100UL
#define CREATE_UNICODE_ENVIRONMENT  0x00000400UL
#define WAIT_OBJECT_0               0x00000000UL
#define WAIT_TIMEOUT                258UL
typedef long LONG;
typedef unsigned long long ULONGLONG;
typedef HANDLE * PHANDLE;

typedef struct _GCU_STUB_SECURITY_ATTRIBUTES {
  DWORD nLength;
  LPVOID lpSecurityDescriptor;
  BOOL bInheritHandle;
} SECURITY_ATTRIBUTES, * LPSECURITY_ATTRIBUTES;

typedef struct _GCU_STUB_STARTUPINFOW {
  DWORD cb;
  DWORD dwFlags;
  HANDLE hStdInput;
  HANDLE hStdOutput;
  HANDLE hStdError;
} STARTUPINFOW;

typedef struct _GCU_STUB_PROCESS_INFORMATION {
  HANDLE hProcess;
  HANDLE hThread;
  DWORD dwProcessId;
  DWORD dwThreadId;
} PROCESS_INFORMATION;

typedef DWORD (WINAPI * LPTHREAD_START_ROUTINE)(LPVOID lpParameter);

BOOL CreatePipe(PHANDLE hReadPipe, PHANDLE hWritePipe,
    LPSECURITY_ATTRIBUTES lpPipeAttributes, DWORD nSize);
BOOL SetHandleInformation(HANDLE hObject, DWORD dwMask, DWORD dwFlags);
BOOL CreateProcessW(LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles,
    DWORD dwCreationFlags, LPVOID lpEnvironment,
    LPCWSTR lpCurrentDirectory, STARTUPINFOW * lpStartupInfo,
    PROCESS_INFORMATION * lpProcessInformation);
BOOL ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
    DWORD * lpNumberOfBytesRead, void * lpOverlapped);
BOOL WriteFile(HANDLE hFile, const void * lpBuffer,
    DWORD nNumberOfBytesToWrite, DWORD * lpNumberOfBytesWritten,
    void * lpOverlapped);
HANDLE CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes,
    size_t dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress,
    LPVOID lpParameter, DWORD dwCreationFlags, DWORD * lpThreadId);
DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
BOOL TerminateProcess(HANDLE hProcess, unsigned int uExitCode);
BOOL GetExitCodeProcess(HANDLE hProcess, DWORD * lpExitCode);
ULONGLONG GetTickCount64(void);
LONG InterlockedExchangeAdd(LONG volatile * Addend, LONG Value);
LONG InterlockedExchange(LONG volatile * Target, LONG Value);
BOOL CancelSynchronousIo(HANDLE hThread);
#endif

/* --- appended for fiber.h / fiber.c --- */
#ifndef GHOTI_IO_GCU_WIN32_STUBS_FIBER
#define GHOTI_IO_GCU_WIN32_STUBS_FIBER

typedef void VOID;
typedef void * LPVOID;

#define WINAPI
#define ERROR_ALREADY_FIBER 1280UL
#define FIBER_FLAG_FLOAT_SWITCH 0x1UL

typedef VOID (WINAPI * LPFIBER_START_ROUTINE)(LPVOID lpFiberParameter);

LPVOID CreateFiberEx(unsigned long long dwStackCommitSize,
    unsigned long long dwStackReserveSize, DWORD dwFlags,
    LPFIBER_START_ROUTINE lpStartAddress, LPVOID lpParameter);
LPVOID ConvertThreadToFiberEx(LPVOID lpParameter, DWORD dwFlags);
LPVOID GetCurrentFiber(void);
VOID SwitchToFiber(LPVOID lpFiber);
VOID DeleteFiber(LPVOID lpFiber);
DWORD GetCurrentThreadId(void);

#endif
