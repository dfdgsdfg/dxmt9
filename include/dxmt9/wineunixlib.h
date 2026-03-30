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

#ifndef DECLSPEC_EXPORT
#define DECLSPEC_EXPORT __attribute__((visibility("default")))
#endif
#endif

typedef UINT64 unixlib_handle_t;
typedef UINT64 unixlib_module_t;

constexpr NTSTATUS DXMT9_STATUS_SUCCESS = 0;
constexpr NTSTATUS DXMT9_STATUS_INVALID_PARAMETER = static_cast<NTSTATUS>(0xC000000Du);
constexpr NTSTATUS DXMT9_STATUS_DLL_NOT_FOUND = static_cast<NTSTATUS>(0xC0000135u);
constexpr NTSTATUS DXMT9_STATUS_NOT_SUPPORTED = static_cast<NTSTATUS>(0xC00000BBu);

#ifdef WINE_UNIX_LIB

typedef NTSTATUS (*unixlib_entry_t)(void *args);

extern "C" DECLSPEC_EXPORT NTSTATUS __wine_unix_lib_init(void);
extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_funcs[];
extern "C" DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_wow64_funcs[];

#else

extern "C" NTSTATUS (WINAPI *__wine_unix_call_dispatcher)(unixlib_handle_t handle,
                                                          unsigned int code,
                                                          void *args);
extern "C" NTSTATUS WINAPI __wine_load_unix_lib(const UNICODE_STRING *name,
                                                unixlib_module_t *lib,
                                                unixlib_handle_t *handle);
extern "C" NTSTATUS WINAPI __wine_unload_unix_lib(unixlib_module_t lib);

#endif
