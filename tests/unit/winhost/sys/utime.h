/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Host-lane shim for MSVC's <sys/utime.h>. The file-transfer I/O layer
 * (filesys_io.h) references struct _utimbuf in a vtable signature, and some
 * protocol TUs (quickvan) touch the POSIX-named struct utimbuf / utime() on the
 * receive path; the host compiler has <utime.h> but not the MSVC <sys/utime.h>
 * spelling. Only the shapes those headers need are provided here — the utime
 * path is never exercised on the host lane (it belongs to the native-Windows lane).
 */
#pragma once

#include <time.h>

struct _utimbuf {
	time_t actime;
	time_t modtime;
};

struct utimbuf {
	time_t actime;
	time_t modtime;
};

int utime(const char *filename, const struct utimbuf *times);
