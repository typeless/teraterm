/*
 * Copyright (C) 1994-1998 T. Teranishi
 * (C) 2007- TeraTerm Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* TERATERM.EXE, TELNET routines */

#include "teraterm.h"
#include "tttypes.h"
#include <stdio.h>
#include <string.h>
#include "ttcommon.h"
#include "ttwinman.h"
#include "commlib.h"
#include <time.h>
#include <process.h>

#include "telnet.h"
#include "telnet_neg.h"
#include "asprintf.h"
#include "tt_res.h"

int TelStatus;

static TelNegState tn;
static HANDLE telnet_log;

static HANDLE keepalive_thread = INVALID_HANDLE_VALUE;
static HWND keepalive_dialog = NULL;
static int nop_interval = 0;

static void TelSendNOP();
static void TelStopKeepAliveThread();

/**
 *	@retval 書き込みバイト数
 */
static UINT win16_lwrite(HANDLE hFile, const char*buf, UINT length)
{
	DWORD NumberOfBytesWritten;
	BOOL result = WriteFile(hFile, buf, length, &NumberOfBytesWritten, NULL);
	if (result == FALSE) {
		return 0;
	}
	return NumberOfBytesWritten;
}

void InitTelnet(void)
{
	TelStatus = TelIdle;

	TelNegInit(&tn, ts.TerminalWidth, ts.TerminalHeight);

	if ((ts.LogFlag & LOG_TEL) != 0) {
		wchar_t *full_path = NULL;
		awcscats(&full_path, ts.LogDirW, L"\\", L"TELNET.LOG", NULL);
		telnet_log = CreateFileW(full_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
								 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		free(full_path);
	} else
		telnet_log = 0;
}

void EndTelnet(void)
{
	if (telnet_log) {
		CloseHandle(telnet_log);
		telnet_log = 0;
	}

	TelStopKeepAliveThread();
}

static void TelWriteLog1(BYTE b)
{
	BYTE Temp[3];
	BYTE Ch;

	Temp[0] = 0x20;
	Ch = b / 16;
	if (Ch <= 9)
		Ch = Ch + 0x30;
	else
		Ch = Ch + 0x37;
	Temp[1] = Ch;

	Ch = b & 15;
	if (Ch <= 9)
		Ch = Ch + 0x30;
	else
		Ch = Ch + 0x37;
	Temp[2] = Ch;
	win16_lwrite(telnet_log, Temp, 3);
}

static void TelWriteLog(PCHAR Buf, int C)
{
	int i;

	win16_lwrite(telnet_log, "\015\012>", 3);
	for (i = 0 ; i<= C-1 ; i++)
		TelWriteLog1(Buf[i]);
}

static void tel_sink(const unsigned char *buf, int len, void *ctx)
{
	(void)ctx;
	CommRawOut(&cv, (PCHAR)buf, len);
	if (telnet_log)
		TelWriteLog((PCHAR)buf, len);
}

void ParseTel(BOOL *Size, int *nx, int *ny)
{
	BYTE b;
	int c;
	TelNegConfig cfg;

	cfg.TelEcho = ts.TelEcho;
	cfg.TermType = ts.TermType;
	cfg.TerminalInputSpeed = ts.TerminalInputSpeed;
	cfg.TerminalOutputSpeed = ts.TerminalOutputSpeed;

	tn.Status = TelStatus;
	tn.LocalEcho = ts.LocalEcho;
	tn.TelLineMode = cv.TelLineMode;
	tn.TelBinRecv = cv.TelBinRecv;
	tn.TelBinSend = cv.TelBinSend;

	c = CommReadRawByte(&cv, &b);

	while ((c>0) && (cv.TelMode)) {
		if (telnet_log) {
			if (tn.Status==TelIAC) {
				win16_lwrite(telnet_log, "\015\012<", 3);
				TelWriteLog1(0xff);
			}
			TelWriteLog1(b);
		}

		TelNegFeed(&tn, &cfg, b, tel_sink, NULL);
		if (tn.Status == TelIdle) cv.TelMode = FALSE;

		if (cv.TelMode) c = CommReadRawByte(&cv, &b);
	}

	TelStatus = tn.Status;
	ts.LocalEcho = (WORD)tn.LocalEcho;
	cv.TelLineMode = tn.TelLineMode;
	cv.TelBinRecv = tn.TelBinRecv;
	cv.TelBinSend = tn.TelBinSend;

	*Size = tn.ChangeWinSize;
	*nx = tn.WinSizeX;
	*ny = tn.WinSizeY;
}

void TelEnableHisOpt(BYTE b)
{
	TelNegEnableHisOpt(&tn, b, tel_sink, NULL);
}

void TelEnableMyOpt(BYTE b)
{
	TelNegEnableMyOpt(&tn, b, tel_sink, NULL);
}

void TelInformWinSize(int nx, int ny)
{
	TelNegInformWinSize(&tn, nx, ny, tel_sink, NULL);
}

void TelSendAYT(void)
{
	BYTE Str[2];

	Str[0] = IAC;
	Str[1] = AYT;
	CommRawOut(&cv, Str, 2);
	CommSend(&cv);
	if (telnet_log)
		TelWriteLog(Str, 2);
}

void TelSendBreak(void)
{
	BYTE Str[2];

	Str[0] = IAC;
	Str[1] = BREAK;
	CommRawOut(&cv, Str, 2);
	CommSend(&cv);
	if (telnet_log)
		TelWriteLog(Str, 2);
}

void TelChangeEcho(void)
{
	if (ts.LocalEcho==0)
		TelNegEnableHisOpt(&tn, ECHO, tel_sink, NULL);
	else
		TelNegDisableHisOpt(&tn, ECHO, tel_sink, NULL);
}

static void TelSendNOP(void)
{
	BYTE Str[2];

	Str[0] = IAC;
	Str[1] = NOP;
	CommRawOut(&cv, Str, 2);
	CommSend(&cv);
	if (telnet_log)
		TelWriteLog(Str, 2);
}

#define WM_SEND_HEARTBEAT (WM_USER + 1)

static INT_PTR CALLBACK telnet_heartbeat_dlg_proc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_INITDIALOG:
		return FALSE;

	case WM_SEND_HEARTBEAT:
		TelSendNOP();
		return TRUE;
		break;

	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDOK:
			return TRUE;
		case IDCANCEL:
			EndDialog(hWnd, 0);
			return TRUE;
		default:
			return FALSE;
		}
		break;

	case WM_CLOSE:
		// closeボタンが押下されても window が閉じないようにする。
		return TRUE;

	case WM_DESTROY:
		return TRUE;

	default:
		return FALSE;
	}
	return TRUE;
}


static unsigned _stdcall TelKeepAliveThread(void *dummy)
{
	static int instance = 0;

	if (instance > 0)
		return 0;
	instance++;

	while (cv.Open && nop_interval > 0) {
		if (time(NULL) >= cv.LastSendTime + nop_interval) {
			SendMessage(keepalive_dialog, WM_SEND_HEARTBEAT, 0, 0);
		}

		Sleep(100);
	}
	instance--;
	return 0;
}

void TelStartKeepAliveThread(void)
{
	unsigned tid;

	if (ts.TelKeepAliveInterval > 0) {
		nop_interval = ts.TelKeepAliveInterval;

		// モードレスダイアログを追加 (2007.12.26 yutaka)
		keepalive_dialog = CreateDialog(hInst, MAKEINTRESOURCE(IDD_BROADCAST_DIALOG),
										HVTWin, telnet_heartbeat_dlg_proc);

		keepalive_thread = (HANDLE)_beginthreadex(NULL, 0, TelKeepAliveThread, NULL, 0, &tid);
		if (keepalive_thread == 0) {
			keepalive_thread = INVALID_HANDLE_VALUE;
			nop_interval = 0;
		}
	}
}

static void TelStopKeepAliveThread(void)
{
	if (keepalive_thread != INVALID_HANDLE_VALUE) {
		nop_interval = 0;
		WaitForSingleObject(keepalive_thread, INFINITE);
		CloseHandle(keepalive_thread);
		keepalive_thread = INVALID_HANDLE_VALUE;

		DestroyWindow(keepalive_dialog);
	}
}

void TelUpdateKeepAliveInterval(void)
{
	if (cv.Open && cv.TelFlag && ts.TCPPort==ts.TelPort) {
		if (ts.TelKeepAliveInterval > 0 && keepalive_thread == INVALID_HANDLE_VALUE)
			TelStartKeepAliveThread();
		else if (ts.TelKeepAliveInterval == 0 && keepalive_thread != INVALID_HANDLE_VALUE)
			TelStopKeepAliveThread();
		else
			nop_interval = ts.TelKeepAliveInterval;
	}
}
