/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Plain-C driving seam for the file-transfer protocol host tests. The Win32
 * vocabulary shim and the protocol headers live entirely behind this boundary
 * (ttpfile_harness.c); the Catch2 side includes only this header and asserts on
 * TtpResult, so the test translation unit never sees <windows.h> or the shim.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { TTP_TX_MAX = 65536 };

typedef struct {
	int rc;                        /* 0 = session drove to completion; <0 = harness guard tripped */
	int tx_len;                    /* number of bytes the protocol put on the wire */
	unsigned char tx[TTP_TX_MAX];  /* captured wire bytes */
	int success;                   /* fv.Success reported at end of session */
	int io_error;                  /* whether the fake TFileIO's error-read path fired */
} TtpResult;

/*
 * Drive an XMODEM (checksum mode) send of a `file_len`-byte synthetic file
 * against a fake TFileIO + fake TComm. When `fail_at == (size_t)-1` the file
 * reads cleanly to EOF; otherwise the fake returns an error-shaped read (0 bytes
 * the way a failed Win32 ReadFile does today) once the read offset reaches
 * `fail_at`, exercising the EOF/error conflation under test.
 */
void ttp_xmodem_send(size_t file_len, size_t fail_at, TtpResult *out);

#ifdef __cplusplus
}
#endif
