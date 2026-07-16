/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Pure TELNET option-negotiation state machine, extracted verbatim in behaviour
 * from telnet.c. No ts/cv globals, no comm layer, no Win32 — every input is a
 * parameter and every emitted byte goes through the sink, so it is host-testable
 * and differentially checkable against the original.
 *
 * Modernized to the house C++23 conventions (anonymous-namespace internal
 * linkage, trailing return types, left-bound pointers/refs). The public surface
 * keeps C linkage and a C-compatible header because telnet.c (still C) calls it.
 */
#include <cstdio>
#include <cstring>

#include "telnet_neg.h"

namespace {

/* Option status and queue values, matching the original enums in telnet.c. */
enum { No, Yes, WantNo, WantYes };
enum { Empty, Opposite };

auto send_back(TelNegState* st, TelNegSink sink, void* ctx, unsigned char a, unsigned char b) -> void
{
	unsigned char s[3];
	(void)st;
	s[0] = IAC;
	s[1] = a;
	s[2] = b;
	sink(s, 3, ctx);
}

auto send_winsize(TelNegState* st, TelNegSink sink, void* ctx) -> void
{
	unsigned char buf[21];
	int i = 0;
	int const hx = (st->WinSizeX >> 8) & 0xff, lx = st->WinSizeX & 0xff;
	int const hy = (st->WinSizeY >> 8) & 0xff, ly = st->WinSizeY & 0xff;

	buf[i++] = IAC;
	buf[i++] = SB;
	buf[i++] = NAWS;
	if (hx == IAC) buf[i++] = IAC;
	buf[i++] = (unsigned char)hx;
	if (lx == IAC) buf[i++] = IAC;
	buf[i++] = (unsigned char)lx;
	if (hy == IAC) buf[i++] = IAC;
	buf[i++] = (unsigned char)hy;
	if (ly == IAC) buf[i++] = IAC;
	buf[i++] = (unsigned char)ly;
	buf[i++] = IAC;
	buf[i++] = SE;
	sink(buf, i, ctx);
}

auto parse_iac(TelNegState* st, unsigned char b) -> void
{
	switch (b) {
	case SE:
		break;
	case NOP:
	case DM:
	case BREAK:
	case IP:
	case AO:
	case AYT:
	case EC:
	case EL:
	case GOAHEAD:
		st->Status = TelIdle;
		break;
	case SB:
		st->Status = TelSB;
		st->SubOptCount = 0;
		break;
	case WILLTEL:
		st->Status = TelWill;
		break;
	case WONTTEL:
		st->Status = TelWont;
		break;
	case DOTEL:
		st->Status = TelDo;
		break;
	case DONTTEL:
		st->Status = TelDont;
		break;
	case IAC:
		st->Status = TelIdle;
		break;
	default:
		st->Status = TelIdle;
	}
}

auto parse_sb(TelNegState* st, const TelNegConfig* cfg, unsigned char b, TelNegSink sink, void* ctx) -> void
{
	if (st->SubOptIAC) {
		st->SubOptIAC = 0;
		switch (b) {
		case SE:
			if (st->SubOptCount <= 1) {
				st->SubOptCount = 0;
				st->Status = TelIdle;
				return;
			}
			switch (st->SubOptBuff[0]) {
			case TERMTYPE:
				if ((st->MyOpt[TERMTYPE].Status == Yes) && (st->SubOptBuff[1] == 1)) {
					unsigned char resp[51];
					int n = 0;
					resp[n++] = IAC;
					resp[n++] = SB;
					resp[n++] = TERMTYPE;
					resp[n++] = 0;
					{
						char const* p = cfg->TermType;
						for (; p != nullptr && *p != 0 && n < (int)sizeof(resp) - 2; p++) {
							resp[n++] = (unsigned char)*p;
						}
					}
					resp[n++] = IAC;
					resp[n++] = SE;
					sink(resp, n, ctx);
				}
				break;

			case NAWS:
				if (st->SubOptCount >= 5) {
					st->WinSizeX = st->SubOptBuff[1] * 256 + st->SubOptBuff[2];
					st->WinSizeY = st->SubOptBuff[3] * 256 + st->SubOptBuff[4];
					st->ChangeWinSize = 1;
				}
				break;

			case TERMSPEED:
				if ((st->MyOpt[TERMSPEED].Status == Yes) && (st->SubOptBuff[1] == 1)) {
					unsigned char resp[51];
					char num[40];
					int n = 0, k, m;
					resp[n++] = IAC;
					resp[n++] = SB;
					resp[n++] = TERMSPEED;
					resp[n++] = 0;
					m = snprintf(num, sizeof(num), "%d,%d", cfg->TerminalInputSpeed,
								 cfg->TerminalOutputSpeed);
					for (k = 0; k < m && n < (int)sizeof(resp) - 2; k++) {
						resp[n++] = (unsigned char)num[k];
					}
					resp[n++] = IAC;
					resp[n++] = SE;
					sink(resp, n, ctx);
				}
				break;
			}

			st->SubOptCount = 0;
			st->Status = TelIdle;
			return;

		case IAC:
			/* A doubled IAC is a literal 255 byte; fall through to append it. */
			break;

		default:
			/* An out-of-place TELNET command inside a sub-option is undefined;
			 * keep it as data, matching the original. */
			if (st->SubOptCount >= (int)sizeof(st->SubOptBuff) - 1) {
				st->SubOptCount = 0;
				st->Status = TelIdle;
				return;
			}
			else {
				st->SubOptBuff[st->SubOptCount] = IAC;
				st->SubOptCount++;
			}
		}
	}
	else if (b == IAC) {
		st->SubOptIAC = 1;
		return;
	}

	if (st->SubOptCount >= (int)sizeof(st->SubOptBuff) - 1) {
		st->SubOptCount = 0;
		st->SubOptIAC = 0;
		st->Status = TelIdle;
	}
	else {
		st->SubOptBuff[st->SubOptCount] = b;
		st->SubOptCount++;
	}
}

/* ECHO handling shared by WILL/WONT: flip LocalEcho when TelEcho is on, and drop
 * line mode once the peer echoes. */
auto apply_echo(TelNegState* st, const TelNegConfig* cfg) -> void
{
	if (cfg->TelEcho) {
		switch (st->HisOpt[ECHO].Status) {
		case Yes:
			st->LocalEcho = 0;
			break;
		case No:
			st->LocalEcho = 1;
			break;
		default:
			break;
		}
	}
	if (st->HisOpt[ECHO].Status == Yes) {
		st->TelLineMode = 0;
	}
}

auto parse_will(TelNegState* st, const TelNegConfig* cfg, unsigned char b, TelNegSink sink, void* ctx) -> void
{
	if (b <= MaxTelOpt) {
		switch (st->HisOpt[b].Status) {
		case No:
			if (st->HisOpt[b].Accept) {
				send_back(st, sink, ctx, DOTEL, b);
				st->HisOpt[b].Status = Yes;
			}
			else {
				send_back(st, sink, ctx, DONTTEL, b);
			}
			break;

		case WantNo:
			switch (st->HisOpt[b].Que) {
			case Empty:
				st->HisOpt[b].Status = No;
				break;
			case Opposite:
				st->HisOpt[b].Status = Yes;
				break;
			}
			break;

		case WantYes:
			switch (st->HisOpt[b].Que) {
			case Empty:
				st->HisOpt[b].Status = Yes;
				break;
			case Opposite:
				st->HisOpt[b].Status = WantNo;
				st->HisOpt[b].Que = Empty;
				send_back(st, sink, ctx, DONTTEL, b);
				break;
			}
			break;

		default:
			break;
		}
	}
	else {
		send_back(st, sink, ctx, DONTTEL, b);
	}

	switch (b) {
	case ECHO:
		apply_echo(st, cfg);
		break;

	case SGA:
		if (st->HisOpt[SGA].Status == Yes) {
			st->TelLineMode = 0;
		}
		break;

	case BINARY:
		switch (st->HisOpt[BINARY].Status) {
		case Yes:
			st->TelBinRecv = 1;
			break;
		case No:
			st->TelBinRecv = 0;
			break;
		default:
			break;
		}
		break;

	default:
		break;
	}
	st->Status = TelIdle;
}

auto parse_wont(TelNegState* st, const TelNegConfig* cfg, unsigned char b, TelNegSink sink, void* ctx) -> void
{
	if (b <= MaxTelOpt) {
		switch (st->HisOpt[b].Status) {
		case Yes:
			st->HisOpt[b].Status = No;
			send_back(st, sink, ctx, DONTTEL, b);
			break;

		case WantNo:
			switch (st->HisOpt[b].Que) {
			case Empty:
				st->HisOpt[b].Status = No;
				break;
			case Opposite:
				st->HisOpt[b].Status = WantYes;
				st->HisOpt[b].Que = Empty;
				send_back(st, sink, ctx, DOTEL, b);
				break;
			}
			break;

		case WantYes:
			switch (st->HisOpt[b].Que) {
			case Empty:
				st->HisOpt[b].Status = No;
				break;
			case Opposite:
				st->HisOpt[b].Status = No;
				st->HisOpt[b].Que = Empty;
				break;
			}
			break;

		default:
			break;
		}
	}
	else {
		send_back(st, sink, ctx, DONTTEL, b);
	}

	switch (b) {
	case ECHO:
		apply_echo(st, cfg);
		break;

	case BINARY:
		switch (st->HisOpt[BINARY].Status) {
		case Yes:
			st->TelBinRecv = 1;
			break;
		case No:
			st->TelBinRecv = 0;
			break;
		default:
			break;
		}
		break;

	default:
		break;
	}
	st->Status = TelIdle;
}

auto parse_do(TelNegState* st, unsigned char b, TelNegSink sink, void* ctx) -> void
{
	if (b <= MaxTelOpt) {
		switch (st->MyOpt[b].Status) {
		case No:
			if (st->MyOpt[b].Accept) {
				st->MyOpt[b].Status = Yes;
				send_back(st, sink, ctx, WILLTEL, b);
			}
			else {
				send_back(st, sink, ctx, WONTTEL, b);
			}
			break;

		case WantNo:
			switch (st->MyOpt[b].Que) {
			case Empty:
				st->MyOpt[b].Status = No;
				break;
			case Opposite:
				st->MyOpt[b].Status = Yes;
				break;
			}
			break;

		case WantYes:
			switch (st->MyOpt[b].Que) {
			case Empty:
				st->MyOpt[b].Status = Yes;
				break;
			case Opposite:
				st->MyOpt[b].Status = WantNo;
				st->MyOpt[b].Que = Empty;
				send_back(st, sink, ctx, WONTTEL, b);
				break;
			}
			break;

		default:
			break;
		}
	}
	else {
		send_back(st, sink, ctx, WONTTEL, b);
	}

	switch (b) {
	case BINARY:
		switch (st->MyOpt[BINARY].Status) {
		case Yes:
			st->TelBinSend = 1;
			break;
		case No:
			st->TelBinSend = 0;
			break;
		default:
			break;
		}
		break;

	case NAWS:
		if (st->MyOpt[NAWS].Status == Yes) {
			send_winsize(st, sink, ctx);
		}
		break;

	case SGA:
		if (st->MyOpt[SGA].Status == Yes) {
			st->TelLineMode = 0;
		}
		break;

	default:
		break;
	}
	st->Status = TelIdle;
}

auto parse_dont(TelNegState* st, unsigned char b, TelNegSink sink, void* ctx) -> void
{
	if (b <= MaxTelOpt) {
		switch (st->MyOpt[b].Status) {
		case Yes:
			st->MyOpt[b].Status = No;
			send_back(st, sink, ctx, WONTTEL, b);
			break;

		case WantNo:
			switch (st->MyOpt[b].Que) {
			case Empty:
				st->MyOpt[b].Status = No;
				break;
			case Opposite:
				st->MyOpt[b].Status = WantYes;
				st->MyOpt[b].Que = Empty;
				send_back(st, sink, ctx, WILLTEL, b);
				break;
			}
			break;

		case WantYes:
			switch (st->MyOpt[b].Que) {
			case Empty:
				st->MyOpt[b].Status = No;
				break;
			case Opposite:
				st->MyOpt[b].Status = No;
				st->MyOpt[b].Que = Empty;
				break;
			}
			break;

		default:
			break;
		}
	}
	else {
		send_back(st, sink, ctx, WONTTEL, b);
	}

	switch (b) {
	case BINARY:
		switch (st->MyOpt[BINARY].Status) {
		case Yes:
			st->TelBinSend = 1;
			break;
		case No:
			st->TelBinSend = 0;
			break;
		default:
			break;
		}
		break;

	default:
		break;
	}
	st->Status = TelIdle;
}

} // namespace

auto TelNegInit(TelNegState* st, int win_x, int win_y) -> void
{
	int i;
	memset(st, 0, sizeof(*st));

	for (i = 0; i <= MaxTelOpt; i++) {
		st->MyOpt[i].Accept = 0;
		st->MyOpt[i].Status = No;
		st->MyOpt[i].Que = Empty;
		st->HisOpt[i].Accept = 0;
		st->HisOpt[i].Status = No;
		st->HisOpt[i].Que = Empty;
	}
	st->SubOptCount = 0;
	st->SubOptIAC = 0;
	st->ChangeWinSize = 0;

	st->Status = TelIdle;
	st->MyOpt[BINARY].Accept = 1;
	st->HisOpt[BINARY].Accept = 1;
	st->MyOpt[SGA].Accept = 1;
	st->HisOpt[SGA].Accept = 1;
	st->HisOpt[ECHO].Accept = 1;
	st->MyOpt[TERMTYPE].Accept = 1;
	st->MyOpt[TERMSPEED].Accept = 1;
	st->MyOpt[NAWS].Accept = 1;
	st->HisOpt[NAWS].Accept = 1;
	st->WinSizeX = win_x;
	st->WinSizeY = win_y;
}

auto TelNegFeed(TelNegState* st, const TelNegConfig* cfg, unsigned char b, TelNegSink sink, void* ctx) -> void
{
	st->ChangeWinSize = 0;

	switch (st->Status) {
	case TelIAC:
		parse_iac(st, b);
		break;
	case TelSB:
		parse_sb(st, cfg, b, sink, ctx);
		break;
	case TelWill:
		parse_will(st, cfg, b, sink, ctx);
		break;
	case TelWont:
		parse_wont(st, cfg, b, sink, ctx);
		break;
	case TelDo:
		parse_do(st, b, sink, ctx);
		break;
	case TelDont:
		parse_dont(st, b, sink, ctx);
		break;
	case TelNop:
		st->Status = TelIdle;
		break;
	}
}

auto TelNegEnableHisOpt(TelNegState* st, unsigned char b, TelNegSink sink, void* ctx) -> void
{
	if (b <= MaxTelOpt) {
		switch (st->HisOpt[b].Status) {
		case No:
			st->HisOpt[b].Status = WantYes;
			send_back(st, sink, ctx, DOTEL, b);
			break;
		case WantNo:
			if (st->HisOpt[b].Que == Empty) st->HisOpt[b].Que = Opposite;
			break;
		case WantYes:
			if (st->HisOpt[b].Que == Opposite) st->HisOpt[b].Que = Empty;
			break;
		default:
			break;
		}
	}
}

auto TelNegDisableHisOpt(TelNegState* st, unsigned char b, TelNegSink sink, void* ctx) -> void
{
	if (b <= MaxTelOpt) {
		switch (st->HisOpt[b].Status) {
		case Yes:
			st->HisOpt[b].Status = WantNo;
			send_back(st, sink, ctx, DONTTEL, b);
			break;
		case WantNo:
			if (st->HisOpt[b].Que == Opposite) st->HisOpt[b].Que = Empty;
			break;
		case WantYes:
			if (st->HisOpt[b].Que == Empty) st->HisOpt[b].Que = Opposite;
			break;
		default:
			break;
		}
	}
}

auto TelNegEnableMyOpt(TelNegState* st, unsigned char b, TelNegSink sink, void* ctx) -> void
{
	if (b <= MaxTelOpt) {
		switch (st->MyOpt[b].Status) {
		case No:
			st->MyOpt[b].Status = WantYes;
			send_back(st, sink, ctx, WILLTEL, b);
			break;
		case WantNo:
			if (st->MyOpt[b].Que == Empty) st->MyOpt[b].Que = Opposite;
			break;
		case WantYes:
			if (st->MyOpt[b].Que == Opposite) st->MyOpt[b].Que = Empty;
			break;
		default:
			break;
		}
	}
}

auto TelNegDisableMyOpt(TelNegState* st, unsigned char b, TelNegSink sink, void* ctx) -> void
{
	if (b <= MaxTelOpt) {
		switch (st->MyOpt[b].Status) {
		case Yes:
			st->MyOpt[b].Status = WantNo;
			send_back(st, sink, ctx, WONTTEL, b);
			break;
		case WantNo:
			if (st->MyOpt[b].Que == Opposite) st->MyOpt[b].Que = Empty;
			break;
		case WantYes:
			if (st->MyOpt[b].Que == Empty) st->MyOpt[b].Que = Opposite;
			break;
		default:
			break;
		}
	}
}

auto TelNegInformWinSize(TelNegState* st, int nx, int ny, TelNegSink sink, void* ctx) -> void
{
	if ((st->MyOpt[NAWS].Status == Yes) && (nx != st->WinSizeX || ny != st->WinSizeY)) {
		st->WinSizeX = nx;
		st->WinSizeY = ny;
		send_winsize(st, sink, ctx);
	}
}
