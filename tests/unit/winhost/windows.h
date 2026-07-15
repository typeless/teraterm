/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Minimal <windows.h> shim for the host (Linux) unit-test lane. It provides ONLY
 * the handful of Win32 types, constants, and codepage entry points that pure
 * core translation units reference, so those TUs compile with the system
 * compiler and their non-Win32 logic can be exercised on the host.
 *
 * The codepage functions are stubs (see winhost.c): pure tests never call them,
 * and a stub that is called aborts loudly rather than returning a wrong answer.
 * Anything needing real MultiByteToWideChar behaviour belongs in a [win]-tagged
 * test that runs on the native-Windows lane, not here.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>

typedef int BOOL;
typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef char CHAR;
typedef wchar_t WCHAR;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef wchar_t *LPWSTR;
typedef const wchar_t *LPCWSTR;
typedef BOOL *LPBOOL;

#define CP_ACP 0u
#define CP_UTF8 65001u
#define MB_ERR_INVALID_CHARS 0x00000008u
#define WC_ERR_INVALID_CHARS 0x00000080u
#define WC_NO_BEST_FIT_CHARS 0x00000400u
#define ERROR_INSUFFICIENT_BUFFER 122u

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* MSVC spellings of the POSIX duplicators, defined locally so the shim carries
 * no link dependency on host feature-test macros. */
static inline char *_ttshim_strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = (char *)malloc(n);
	if (p != NULL) {
		memcpy(p, s, n);
	}
	return p;
}
static inline wchar_t *_ttshim_wcsdup(const wchar_t *s)
{
	size_t n = wcslen(s) + 1;
	wchar_t *p = (wchar_t *)malloc(n * sizeof(wchar_t));
	if (p != NULL) {
		memcpy(p, s, n * sizeof(wchar_t));
	}
	return p;
}
#define _strdup _ttshim_strdup
#define _wcsdup _ttshim_wcsdup

#ifdef __cplusplus
extern "C" {
#endif

int MultiByteToWideChar(UINT CodePage, DWORD dwFlags, const char *lpMultiByteStr,
						int cbMultiByte, wchar_t *lpWideCharStr, int cchWideChar);
int WideCharToMultiByte(UINT CodePage, DWORD dwFlags, const wchar_t *lpWideCharStr,
						int cchWideChar, char *lpMultiByteStr, int cbMultiByte,
						const char *lpDefaultChar, LPBOOL lpUsedDefaultChar);
UINT GetACP(void);
DWORD GetLastError(void);

#ifdef __cplusplus
}
#endif
