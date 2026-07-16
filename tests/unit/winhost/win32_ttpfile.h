/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Host-lane Win32 vocabulary for the file-transfer protocol TUs (ttpfile:
 * xmodem/ymodem/zmodem/kermit/bplus/quickvan and ftlib). It supplements the
 * minimal <windows.h> shim (winhost/windows.h) with the additional handle
 * types, GDI/message plumbing, and MSVC secure-CRT spellings those TUs pull in
 * transitively, so the protocol state machines compile and run on the host lane
 * against a fake TFileIO/TComm. It is force-included (-include) ahead of each
 * protocol TU; it defines a set disjoint from windows.h, so include order is
 * immaterial. None of the Win32 entry points below are real — the protocol
 * logic under test only touches the vtable seams, never these stubs.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>

#define __declspec(x)
#define WINAPI
#define CALLBACK
#define PASCAL
#define far
#define near

typedef void *HANDLE;
typedef HANDLE HWND, HMENU, HINSTANCE, HFONT, HDC, HICON, HCURSOR, HBRUSH, HMODULE, HGDIOBJ, HBITMAP, HRGN, HKEY, HGLOBAL, HLOCAL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef long LONG;
typedef unsigned long ULONG;
typedef unsigned long COLORREF;
typedef int INT;
typedef short SHORT;
typedef unsigned short USHORT;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG, DWORD_PTR, ULONG_PTR;
typedef long long LONG_PTR, INT_PTR;
typedef unsigned long long UINT_PTR;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef BYTE *LPBYTE;
typedef WORD *LPWORD;
typedef unsigned long *LPDWORD;
typedef char *PCHAR;
typedef unsigned short *PWORD;
typedef long *PLONG;
typedef WORD ATOM;
typedef unsigned long LRESULT;
typedef unsigned long WPARAM;
typedef long LPARAM;

typedef struct tagPOINT { LONG x, y; } POINT, *PPOINT, *LPPOINT;
typedef struct tagRECT { LONG left, top, right, bottom; } RECT, *PRECT, *LPRECT;
typedef struct tagSIZE { LONG cx, cy; } SIZE, *PSIZE, *LPSIZE;

#define LF_FACESIZE 32
typedef struct tagLOGFONTA {
	LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
	BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet;
	BYTE lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily;
	char lfFaceName[LF_FACESIZE];
} LOGFONTA, *PLOGFONTA, *LPLOGFONTA;
typedef struct tagLOGFONTW {
	LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
	BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet;
	BYTE lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily;
	wchar_t lfFaceName[LF_FACESIZE];
} LOGFONTW, *PLOGFONTW, *LPLOGFONTW;
typedef LOGFONTA LOGFONT, *PLOGFONT;

typedef struct _SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct _FILETIME { unsigned long dwLowDateTime, dwHighDateTime; } FILETIME;
typedef unsigned long UINT_TIMER;

#define MAX_PATH 260
#define MAXBYTE 0xff
#define _S_IFREG 0100000

#define HIBYTE(w) ((BYTE)(((WORD)(w) >> 8) & 0xFF))
#define LOBYTE(w) ((BYTE)((WORD)(w) & 0xFF))
#define HIWORD(l) ((WORD)((((ULONG_PTR)(l)) >> 16) & 0xFFFF))
#define LOWORD(l) ((WORD)(((ULONG_PTR)(l)) & 0xFFFF))
#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | ((ULONG)((WORD)(b))) << 16))

/* MSVC's _stati64 spelling maps onto a host-defined struct of the same shape;
 * the stat path is never exercised on the host lane. */
typedef struct _stati64_shim { long long st_size; long _shim_mtime; unsigned short st_mode; struct { long tv_sec; long tv_nsec; } st_mtim; } _stati64_t;
#define _stati64 _stati64_shim

#define _TRUNCATE ((size_t)-1)
#define MB_OK 0u
#define MB_ICONEXCLAMATION 0x30u

static inline int _sh_snprintf_s(char *d, size_t n, size_t c, const char *f, ...) { va_list a; va_start(a, f); int r = vsnprintf(d, n, f, a); va_end(a); (void)c; return r; }
#define _snprintf_s _sh_snprintf_s
static inline int _sh_vsnprintf_s(char *d, size_t n, size_t c, const char *f, va_list a) { (void)c; return vsnprintf(d, n, f, a); }
#define _vsnprintf_s _sh_vsnprintf_s
#define strncpy_s(d, ds, s, n) ((void)strncpy((d), (s), ((n) == _TRUNCATE ? (ds) - 1 : (n))))
#define strncat_s(d, ds, s, n) ((void)strncat((d), (s), (ds) - strlen(d) - 1))
#define memmove_s(d, ds, s, n) ((void)memmove((d), (s), (n)))
#define sscanf_s sscanf
#define _atoi64(s) strtoll((s), NULL, 10)
#define localtime_s(tmv, tv) (*(tmv) = *localtime(tv), 0)
#define ctime_s(b, n, t) (strncpy((b), ctime(t), (n)), 0)

unsigned long GetTickCount(void);
int MessageBox(HWND h, const char *t, const char *c, unsigned u);
ULONG_PTR SetTimer(HWND h, ULONG_PTR id, unsigned e, void *fn);
int KillTimer(HWND h, ULONG_PTR id);
