/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Host unit tests for the pure TELNET option-negotiation core (telnet_neg).
 * The module has no ts/cv/Win32 dependency, so it runs on the host lane as-is.
 * These are readable spec examples; the exhaustive equivalence to the original
 * telnet.c FSM is covered by the offline differential harness.
 */
#include "catch_amalgamated.hpp"

#include <vector>

#include "telnet_neg.h"

namespace {

struct Sink {
	std::vector<unsigned char> bytes;
};

void collect(const unsigned char *b, int n, void *ctx)
{
	auto *s = static_cast<Sink *>(ctx);
	s->bytes.insert(s->bytes.end(), b, b + n);
}

const TelNegConfig CFG = {1 /*TelEcho*/, "xterm", 38400, 38400};

// Drive a received IAC sequence (the caller enters TelIAC when it sees IAC).
Sink feed(TelNegState &st, std::initializer_list<int> seq)
{
	Sink s;
	st.Status = TelIAC;
	for (int b : seq) {
		TelNegFeed(&st, &CFG, static_cast<unsigned char>(b), collect, &s);
	}
	return s;
}

auto bytes(std::initializer_list<int> v) -> std::vector<unsigned char>
{
	std::vector<unsigned char> out;
	for (int b : v) out.push_back(static_cast<unsigned char>(b));
	return out;
}

} // namespace

TEST_CASE("DO NAWS is accepted with WILL NAWS and an immediate window size", "[telnet]")
{
	TelNegState st;
	TelNegInit(&st, 120, 40);
	Sink s = feed(st, {DOTEL, NAWS});
	REQUIRE(st.MyOpt[NAWS].Status == 1 /*Yes*/);
	// WILL NAWS, then SB NAWS <0,120,0,40> SE
	REQUIRE(s.bytes == bytes({IAC, WILLTEL, NAWS,
							   IAC, SB, NAWS, 0, 120, 0, 40, IAC, SE}));
}

TEST_CASE("WILL ECHO is accepted with DO ECHO and flips local echo off", "[telnet]")
{
	TelNegState st;
	TelNegInit(&st, 80, 24);
	Sink s = feed(st, {WILLTEL, ECHO});
	REQUIRE(s.bytes == bytes({IAC, DOTEL, ECHO}));
	REQUIRE(st.HisOpt[ECHO].Status == 1);
	REQUIRE(st.LocalEcho == 0);   // remote echoes, so we stop local echo
	REQUIRE(st.TelLineMode == 0);
}

TEST_CASE("binary mode is tracked per direction", "[telnet]")
{
	SECTION("DO BINARY -> we send binary") {
		TelNegState st; TelNegInit(&st, 80, 24);
		Sink s = feed(st, {DOTEL, BINARY});
		REQUIRE(s.bytes == bytes({IAC, WILLTEL, BINARY}));
		REQUIRE(st.TelBinSend == 1);
	}
	SECTION("WILL BINARY -> we receive binary") {
		TelNegState st; TelNegInit(&st, 80, 24);
		Sink s = feed(st, {WILLTEL, BINARY});
		REQUIRE(s.bytes == bytes({IAC, DOTEL, BINARY}));
		REQUIRE(st.TelBinRecv == 1);
	}
}

TEST_CASE("unsupported options are refused", "[telnet]")
{
	TelNegState st;
	TelNegInit(&st, 80, 24);
	// STATUS (5) is not in the accept set.
	REQUIRE(feed(st, {DOTEL, STATUS}).bytes == bytes({IAC, WONTTEL, STATUS}));
	REQUIRE(feed(st, {WILLTEL, STATUS}).bytes == bytes({IAC, DONTTEL, STATUS}));
}

TEST_CASE("TERMINAL-TYPE sub-negotiation returns the configured type", "[telnet]")
{
	TelNegState st;
	TelNegInit(&st, 80, 24);
	feed(st, {DOTEL, TERMTYPE});                 // enable so MyOpt[TERMTYPE]=Yes
	Sink s = feed(st, {SB, TERMTYPE, 1, IAC, SE}); // server asks: SEND
	// SB TERMTYPE IS 0 "xterm" SE
	REQUIRE(s.bytes == bytes({IAC, SB, TERMTYPE, 0, 'x', 't', 'e', 'r', 'm', IAC, SE}));
}

TEST_CASE("NAWS sub-negotiation from the peer updates the window size", "[telnet]")
{
	TelNegState st;
	TelNegInit(&st, 80, 24);
	feed(st, {SB, NAWS, 0, 132, 0, 50, IAC, SE});
	REQUIRE(st.ChangeWinSize == 1);
	REQUIRE(st.WinSizeX == 132);
	REQUIRE(st.WinSizeY == 50);
}

TEST_CASE("a doubled IAC inside a sub-option is a literal 0xFF", "[telnet]")
{
	TelNegState st;
	TelNegInit(&st, 80, 24);
	// NAWS width high byte = 0xFF must be sent as IAC IAC.
	feed(st, {SB, NAWS, IAC, IAC, 1, 0, 40, IAC, SE});
	REQUIRE(st.ChangeWinSize == 1);
	REQUIRE(st.WinSizeX == 0xFF * 256 + 1);
	REQUIRE(st.WinSizeY == 0x00 * 256 + 40);
}
