/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Pure TELNET option-negotiation state machine (RFC 854 / RFC 1143 "Q Method"),
 * extracted from telnet.c so it has no dependency on the ambient ts/cv globals,
 * the comm layer, or Win32 — which makes it host-unit-testable. The owner
 * (telnet.c) supplies the read-only settings it needs (TelNegConfig), a sink for
 * bytes the machine wants to send, and mirrors the decided output flags back to
 * ts/cv after driving each received byte. Logging, the keepalive thread, and the
 * actual socket I/O stay in telnet.c.
 */
#pragma once

#include "telnet_defs.h" /* option + status constants, MaxTelOpt */

#ifdef __cplusplus
extern "C" {
#endif

/* Per-option negotiation record. Status is No/Yes/WantNo/WantYes; Que is
 * Empty/Opposite (see the enums in telnet_neg.c). Kept as ints so the struct is
 * plain and copyable in tests. */
typedef struct {
	int Accept;
	int Status;
	int Que;
} TelNegOpt;

/* Read-only settings the machine consults while negotiating. */
typedef struct {
	int TelEcho;              /* ts.TelEcho: honour remote ECHO by flipping LocalEcho */
	const char *TermType;     /* ts.TermType, for the TERMINAL-TYPE sub-negotiation */
	int TerminalInputSpeed;   /* ts.TerminalInputSpeed, for TERMINAL-SPEED */
	int TerminalOutputSpeed;  /* ts.TerminalOutputSpeed */
} TelNegConfig;

/* Bytes the machine wants to transmit (an IAC command or a sub-option reply). */
typedef void (*TelNegSink)(const unsigned char *buf, int len, void *ctx);

typedef struct {
	int Status; /* TelIdle..TelNop */
	TelNegOpt MyOpt[MaxTelOpt + 1];
	TelNegOpt HisOpt[MaxTelOpt + 1];
	unsigned char SubOptBuff[51];
	int SubOptCount;
	int SubOptIAC;

	int ChangeWinSize; /* set when the peer's NAWS updated the window size */
	int WinSizeX;
	int WinSizeY;

	/* Output flags: mirror ts.LocalEcho / cv.TelLineMode / cv.TelBinRecv /
	 * cv.TelBinSend. The owner seeds these from the live values before driving a
	 * byte and copies them back afterward, so an unchanged flag round-trips. */
	int LocalEcho;
	int TelLineMode;
	int TelBinRecv;
	int TelBinSend;
} TelNegState;

/* Reset the machine and set the option-acceptance policy Tera Term uses; seeds
 * the advertised window size. */
void TelNegInit(TelNegState *st, int win_x, int win_y);

/* Drive one received byte through the machine (dispatch on st->Status). */
void TelNegFeed(TelNegState *st, const TelNegConfig *cfg, unsigned char b, TelNegSink sink, void *ctx);

/* Local-side option control (the "want" transitions), used by telnet.c to
 * initiate or tear down options. */
void TelNegEnableHisOpt(TelNegState *st, unsigned char opt, TelNegSink sink, void *ctx);
void TelNegDisableHisOpt(TelNegState *st, unsigned char opt, TelNegSink sink, void *ctx);
void TelNegEnableMyOpt(TelNegState *st, unsigned char opt, TelNegSink sink, void *ctx);
void TelNegDisableMyOpt(TelNegState *st, unsigned char opt, TelNegSink sink, void *ctx);

/* If NAWS is active and the size changed, record it and emit a NAWS sub-option. */
void TelNegInformWinSize(TelNegState *st, int nx, int ny, TelNegSink sink, void *ctx);

#ifdef __cplusplus
}
#endif
