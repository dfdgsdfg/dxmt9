#pragma once

#include <stdint.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#else
#include <cstddef>

#ifndef WINAPI
#define WINAPI
#endif

using DWORD = uint32_t;
using WCHAR = wchar_t;
using UINT64 = uint64_t;
using LONG = int32_t;
using NTSTATUS = LONG;
typedef struct _UNICODE_STRING {
  uint16_t Length;
  uint16_t MaximumLength;
  WCHAR* Buffer;
} UNICODE_STRING;

#ifndef DECLSPEC_EXPORT
#define DECLSPEC_EXPORT __attribute__((visibility("default")))
#endif
#endif

typedef UINT64 unixlib_handle_t;
typedef UINT64 unixlib_module_t;

#ifdef __cplusplus
constexpr NTSTATUS DXMT9_STATUS_SUCCESS = 0;
constexpr NTSTATUS DXMT9_STATUS_INVALID_PARAMETER = static_cast<NTSTATUS>(0xC000000Du);
constexpr NTSTATUS DXMT9_STATUS_DLL_NOT_FOUND = static_cast<NTSTATUS>(0xC0000135u);
constexpr NTSTATUS DXMT9_STATUS_NOT_SUPPORTED = static_cast<NTSTATUS>(0xC00000BBu);
#else
#define DXMT9_STATUS_SUCCESS ((NTSTATUS)0)
#define DXMT9_STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000Du)
#define DXMT9_STATUS_DLL_NOT_FOUND ((NTSTATUS)0xC0000135u)
#define DXMT9_STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBu)
#endif

#ifdef WINE_UNIX_LIB

typedef NTSTATUS (*unixlib_entry_t)(void *args);

#ifdef __cplusplus
extern "C" {
#endif
extern DECLSPEC_EXPORT NTSTATUS __wine_unix_lib_init(void);
extern DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_funcs[];
extern DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_wow64_funcs[];
#ifdef __cplusplus
}
#endif

#else

#ifdef __cplusplus
extern "C" {
#endif
extern NTSTATUS (WINAPI *__wine_unix_call_dispatcher)(unixlib_handle_t handle,
                                                      unsigned int code,
                                                      void *args);
extern unixlib_handle_t __wine_unixlib_handle;
extern NTSTATUS WINAPI __wine_unix_call(unixlib_handle_t handle,
                                        unsigned int code,
                                        void *args);
extern NTSTATUS WINAPI __wine_init_unix_call(void);
extern NTSTATUS WINAPI __wine_load_unix_lib(const UNICODE_STRING *name,
                                            unixlib_module_t *lib,
                                            unixlib_handle_t *handle);
extern NTSTATUS WINAPI __wine_unload_unix_lib(unixlib_module_t lib);
#ifdef __cplusplus
}
#endif

#ifndef WINE_UNIX_CALL
#define WINE_UNIX_CALL(code, args) __wine_unix_call_dispatcher(__wine_unixlib_handle, (code), (args))
#endif

#endif
