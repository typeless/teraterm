/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Host unit tests for the pure Unicode scalar converters in common/codeconv.cpp.
 * These functions take values and buffers only (no Win32), so they run on the
 * host lane. The Win32-backed conversions (the WideCharToMultiByte and GetACP
 * paths) are stubbed by winhost/ and must be covered by [win]-tagged tests on
 * the native lane.
 */
#include "catch_amalgamated.hpp"

#include <cstdint>
#include <string>

#include "codeconv.h"

namespace {

// The Unicode scalar range, minus the UTF-16 surrogate block. Every value here
// is a legal codepoint that a well-formed UTF-8/UTF-16 stream can carry.
auto is_surrogate(uint32_t cp) -> bool { return cp >= 0xD800 && cp <= 0xDFFF; }

} // namespace

TEST_CASE("UTF-32 -> UTF-8 -> UTF-32 round-trips every scalar value", "[codeconv][utf8]")
{
	for (uint32_t cp = 0; cp <= 0x10FFFF; ++cp) {
		if (is_surrogate(cp)) {
			continue;
		}
		char buf[4];
		const size_t enc = UTF32ToUTF8(cp, buf, sizeof(buf));
		REQUIRE(enc >= 1);
		REQUIRE(enc <= 4);

		uint32_t back = 0xFFFFFFFFu;
		const size_t dec = UTF8ToUTF32(buf, enc, &back);
		INFO("codepoint U+" << std::hex << cp << " encoded length " << std::dec << enc);
		REQUIRE(dec == enc);   // consumed exactly what was produced
		REQUIRE(back == cp);   // and recovered the original value
	}
}

TEST_CASE("UTF-32 -> UTF-16 -> UTF-32 round-trips every scalar value", "[codeconv][utf16]")
{
	for (uint32_t cp = 0; cp <= 0x10FFFF; ++cp) {
		if (is_surrogate(cp)) {
			continue;
		}
		wchar_t u16[2];
		const size_t enc = UTF32ToUTF16(cp, u16, 2);
		REQUIRE(enc >= 1);
		REQUIRE(enc <= 2);
		REQUIRE((cp <= 0xFFFF ? enc == 1 : enc == 2));

		uint32_t back = 0xFFFFFFFFu;
		const size_t dec = UTF16ToUTF32(u16, enc, &back);
		INFO("codepoint U+" << std::hex << cp << " units " << std::dec << enc);
		REQUIRE(dec == enc);
		REQUIRE(back == cp);
	}
}

TEST_CASE("UTF-8 encoded length matches the codepoint's range", "[codeconv][utf8]")
{
	auto expected_len = [](uint32_t cp) -> size_t {
		if (cp <= 0x7F) return 1;
		if (cp <= 0x7FF) return 2;
		if (cp <= 0xFFFF) return 3;
		return 4;
	};
	const uint32_t samples[] = {0x00, 0x7F, 0x80, 0x7FF, 0x800, 0xFFFF, 0x10000, 0x10FFFF};
	for (uint32_t cp : samples) {
		char buf[4];
		REQUIRE(UTF32ToUTF8(cp, buf, sizeof(buf)) == expected_len(cp));
	}
}

TEST_CASE("UTF32ToUTF8 rejects out-of-range codepoints", "[codeconv][utf8]")
{
	char buf[4];
	REQUIRE(UTF32ToUTF8(0x110000, buf, sizeof(buf)) == 0);
	REQUIRE(UTF32ToUTF8(0x7FFFFFFF, buf, sizeof(buf)) == 0);
}

TEST_CASE("UTF32ToUTF8 returns 0 without writing when the buffer is too small",
		  "[codeconv][utf8]")
{
	// A single byte of room cannot hold a 3-byte character. With a non-NULL
	// buffer the function returns 0 (not the required length) and writes nothing.
	char one[1];
	REQUIRE(UTF32ToUTF8(0x20AC /* euro sign, 3 bytes */, one, sizeof(one)) == 0);
}

TEST_CASE("UTF8ToUTF32 rejects malformed lead and continuation bytes", "[codeconv][utf8]")
{
	struct Bad { const char *bytes; size_t len; };
	const Bad bad[] = {
		{"\x80", 1},         // lone continuation byte
		{"\xC0\x80", 2},     // overlong encoding of NUL
		{"\xC2\x20", 2},     // 2-byte lead, bad continuation
		{"\xE0\x80\x80", 3}, // overlong 3-byte
	};
	for (const auto &b : bad) {
		uint32_t u32 = 0xDEAD;
		const size_t n = UTF8ToUTF32(b.bytes, b.len, &u32);
		INFO("first byte 0x" << std::hex << (unsigned)(unsigned char)b.bytes[0]);
		REQUIRE(n == 0);
		REQUIRE(u32 == 0);
	}
}

// Characterization of the DECODER'S CURRENT (lax) behaviour, not an endorsement.
// UTF8ToUTF32 does not enforce the U+10FFFF ceiling on 4-byte sequences, nor
// reject the UTF-16 surrogate range in 3-byte sequences. These are candidates to
// tighten when codeconv is rewritten (each would flip to n==0 with a RED test).
// Locking the behaviour here means such a change can never happen silently.
TEST_CASE("UTF8ToUTF32 currently accepts over-range and surrogate encodings",
		  "[codeconv][utf8][characterization]")
{
	uint32_t u32 = 0;

	// 0xF5 lead decodes to U+140000, above the Unicode ceiling.
	REQUIRE(UTF8ToUTF32("\xF5\x80\x80\x80", 4, &u32) == 4);
	REQUIRE(u32 == 0x140000);

	// 0xED 0xA0 0x80 decodes to U+D800, a high surrogate.
	REQUIRE(UTF8ToUTF32("\xED\xA0\x80", 3, &u32) == 3);
	REQUIRE(u32 == 0xD800);
}
