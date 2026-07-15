/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Host unit tests for the pure VT-buffer cell layer (buffcell), extracted from
 * buffer.c. The Win32-bound multibyte conversion behind ansi_char is injected
 * (CellToMB), so the whole layer — including the ansi_char byte packing, which
 * stays inside buffcell — runs on the host under ASan/UBSan with a pure fake.
 * cp932_ref below is the exact callback shape buffer.c binds in production;
 * its U+203E short-circuit runs on the host, its UTF32ToMBCP arm is
 * [win]-tagged for the native lane. Randomized properties use fixed seeds so
 * failures replay deterministically.
 */
#include "catch_amalgamated.hpp"

#include <random>
#include <vector>

#include "buffcell.h"
#include "codeconv.h"

namespace {

constexpr char32_t kCombiningAcute = 0x0301;

// Pure fake: length and bytes derived from u32 so every packing arm
// (0 -> '?', 1, 2, >2 -> '?') is exercised deterministically on the host.
size_t fake_mb(char32_t u32, void *ctx, char *mb_ptr, size_t mb_len)
{
	(void)ctx;
	size_t len = u32 % 4;
	for (size_t i = 0; i < len && i < mb_len; i++) {
		mb_ptr[i] = (char)(0x40 + ((u32 >> (4 * i)) & 0x3F));
	}
	return len;
}

unsigned short fake_mb_expected(char32_t u32)
{
	char mb[4];
	size_t len = fake_mb(u32, NULL, mb, sizeof(mb));
	switch (len) {
	case 1:
		return (unsigned char)mb[0];
	case 2:
		return (unsigned char)mb[1] | ((unsigned char)mb[0] << 8);
	default:
		return '?';
	}
}

// The owner-shaped production callback: display code page in ctx, U+203E
// special-cased under CP932, everything else through UTF32ToMBCP.
size_t cp932_ref(char32_t u32, void *ctx, char *mb_ptr, size_t mb_len)
{
	int code_page = *(int *)ctx;
	if (u32 == 0x203e && code_page == 932) {
		mb_ptr[0] = 0x7e;
		return 1;
	}
	return UTF32ToMBCP((unsigned int)u32, code_page, mb_ptr, mb_len);
}

size_t counting_mb(char32_t u32, void *ctx, char *mb_ptr, size_t mb_len)
{
	*(int *)ctx += 1;
	return fake_mb(u32, NULL, mb_ptr, mb_len);
}

struct Cell {
	buff_char_t c{};
	~Cell() { CellFreeCombination(&c); }
};

struct CellArray {
	std::vector<buff_char_t> v;
	explicit CellArray(size_t n) : v(n) {}
	~CellArray()
	{
		for (auto &c : v) CellFreeCombination(&c);
	}
	buff_char_t *data() { return v.data(); }
};

void set2(buff_char_t *c, char32_t u32, char property = 'H', int half_width = 1, char emoji = 0)
{
	CellSetChar2(c, u32, property, half_width, emoji, fake_mb, NULL);
}

std::vector<wchar_t> utf16_of(char32_t u32)
{
	wchar_t w[2];
	size_t n = UTF32ToUTF16((unsigned int)u32, w, 2);
	return std::vector<wchar_t>(w, w + n);
}

std::vector<wchar_t> expand(const buff_char_t *c)
{
	size_t len = CellExpandWchar(c, NULL, 0, NULL);
	std::vector<wchar_t> out(len);
	if (len > 0) {
		int too_small = -1;
		REQUIRE(CellExpandWchar(c, out.data(), len, &too_small) == len);
		REQUIRE(too_small == 0);
	}
	return out;
}

} // namespace

TEST_CASE("cell stores the UTF-16 encoding and packed ansi_char of its code point", "[buffcell]")
{
	Cell cell;
	for (char32_t u32 = 1; u32 <= 0x10FFFF; u32++) {
		set2(&cell.c, u32);

		wchar_t w[2] = {0, 0};
		size_t n = UTF32ToUTF16((unsigned int)u32, w, 2);
		if (n < 2) w[1] = 0;
		if (n < 1) w[0] = 0;

		REQUIRE((cell.c.wc2[0] == w[0] && cell.c.wc2[1] == w[1] &&
		         cell.c.u32 == u32 && cell.c.u32_last == u32));

		unsigned short want_ansi =
			u32 < 0x80 ? (unsigned short)u32 : fake_mb_expected(u32);
		REQUIRE(cell.c.ansi_char == want_ansi);
	}
}

TEST_CASE("ASCII never reaches the to_mb callback", "[buffcell]")
{
	Cell cell;
	int calls = 0;
	for (char32_t u32 = 1; u32 < 0x80; u32++) {
		CellSetChar2(&cell.c, u32, 'H', 1, 0, counting_mb, &calls);
	}
	CHECK(calls == 0);
	CellSetChar2(&cell.c, 0x80, 'H', 1, 0, counting_mb, &calls);
	CHECK(calls == 1);
}

TEST_CASE("owner-shaped callback: U+203E is '~' under CP932", "[buffcell]")
{
	Cell cell;
	int cp = 932;
	CellSetChar2(&cell.c, 0x203E, 'H', 1, 0, cp932_ref, &cp);
	CHECK(cell.c.ansi_char == 0x7E);

	CellSetChar2(&cell.c, 'A', 'H', 1, 0, cp932_ref, &cp);
	CHECK(cell.c.ansi_char == 'A');
}

TEST_CASE("owner-shaped callback: real conversions", "[buffcell][win]")
{
	Cell cell;
	int cp = 932;

	// U+3042 HIRAGANA A -> CP932 double byte 0x82A0
	CellSetChar2(&cell.c, 0x3042, 'W', 0, 0, cp932_ref, &cp);
	CHECK(cell.c.ansi_char == 0x82A0);

	// U+FF71 HALFWIDTH KATAKANA A -> CP932 single byte 0xB1
	CellSetChar2(&cell.c, 0xFF71, 'H', 1, 0, cp932_ref, &cp);
	CHECK(cell.c.ansi_char == 0xB1);

	// U+0301 has no CP932 mapping -> '?'
	CellSetChar2(&cell.c, kCombiningAcute, 'H', 1, 0, cp932_ref, &cp);
	CHECK(cell.c.ansi_char == '?');

	int cp1252 = 1252;
	CellSetChar2(&cell.c, 0x00E9 /* e-acute */, 'H', 1, 0, cp932_ref, &cp1252);
	CHECK(cell.c.ansi_char == 0xE9);
}

TEST_CASE("cell construction fields", "[buffcell]")
{
	Cell cell;

	set2(&cell.c, 'x');
	CHECK(cell.c.cell == 1);
	CHECK(cell.c.Padding == 0);
	CHECK(cell.c.fg == AttrDefaultFG);
	CHECK(cell.c.bg == AttrDefaultBG);

	set2(&cell.c, 0x3042, 'W', 0, 0);
	CHECK(cell.c.cell == 2);

	CellSetChar4(&cell.c, 'y', 3, 5, 7, 9, 'H', fake_mb, NULL);
	CHECK(cell.c.fg == 3);
	CHECK(cell.c.bg == 5);
	CHECK(cell.c.attr == 7);
	CHECK(cell.c.attr2 == 9);
}

TEST_CASE("combining characters accumulate in order and expand back out", "[buffcell]")
{
	Cell cell;
	set2(&cell.c, 'a');

	std::vector<wchar_t> expected = utf16_of('a');
	int accepted = 0;
	for (int k = 1; k <= 99; k++) {
		// Every third mark is astral (a surrogate pair in the UTF-16 buffer).
		char32_t mark = (k % 3 == 0) ? 0x1D165 + (k % 4) : kCombiningAcute + (k % 16);
		auto m16 = utf16_of(mark);
		if (expected.size() + m16.size() > 1 + MAX_CHAR_SIZE) {
			break;
		}
		CellAddChar(&cell.c, mark);
		expected.insert(expected.end(), m16.begin(), m16.end());
		accepted++;

		REQUIRE(cell.c.CombinationCharCount32 == accepted);
		REQUIRE(cell.c.u32_last == mark);
		REQUIRE(expand(&cell.c) == expected);
		REQUIRE(cell.c.CombinationCharCount16 <= cell.c.CombinationCharSize16);
		REQUIRE(cell.c.CombinationCharSize16 <= MAX_CHAR_SIZE);
	}
	REQUIRE(accepted >= 70);
}

TEST_CASE("combining buffers cap at MAX_CHAR_SIZE", "[buffcell]")
{
	Cell cell;
	set2(&cell.c, 'a');

	for (int k = 1; k <= 120; k++) {
		CellAddChar(&cell.c, kCombiningAcute);
	}
	CHECK(cell.c.CombinationCharCount32 == MAX_CHAR_SIZE);
	CHECK(cell.c.CombinationCharSize32 == MAX_CHAR_SIZE);
	CHECK(cell.c.CombinationCharCount16 == MAX_CHAR_SIZE);
	CHECK(expand(&cell.c).size() == 1 + MAX_CHAR_SIZE);
}

TEST_CASE("astral combining marks fill the UTF-16 buffer at half the UTF-32 rate", "[buffcell]")
{
	Cell cell;
	set2(&cell.c, 'a');

	const char32_t astral_mark = 0x1D165; // MUSICAL SYMBOL COMBINING STEM (surrogate pair)
	for (int k = 1; k <= 60; k++) {
		CellAddChar(&cell.c, astral_mark);
	}
	// The UTF-32 side accepted all 60; the UTF-16 side (2 units each) hit the
	// 100-unit cap after 50.
	CHECK(cell.c.CombinationCharCount32 == 60);
	CHECK(cell.c.CombinationCharCount16 == MAX_CHAR_SIZE);

	std::vector<wchar_t> expected = utf16_of('a');
	auto pair = utf16_of(astral_mark);
	for (int k = 1; k <= 50; k++) {
		expected.insert(expected.end(), pair.begin(), pair.end());
	}
	CHECK(expand(&cell.c) == expected);
}

TEST_CASE("CellCopy is a deep copy", "[buffcell]")
{
	Cell src, dst;
	set2(&src.c, 0x1F600 /* emoji, surrogate pair */, 'W', 0, 1);
	CellAddChar(&src.c, kCombiningAcute);
	CellAddChar(&src.c, 0x0302);
	auto want = expand(&src.c);

	CellCopy(&dst.c, &src.c);
	// Destroy the source; the copy must be unaffected (ASan flags sharing).
	set2(&src.c, 'x');
	CHECK(expand(&dst.c) == want);
	CHECK(dst.c.Emoji == 1);
}

TEST_CASE("CellsCopy and CellsMove degenerate cases", "[buffcell]")
{
	CellArray a(3);
	set2(&a.v[0], 'q');
	CellAddChar(&a.v[0], kCombiningAcute);
	auto want = expand(&a.v[0]);

	CellsCopy(a.data(), a.data(), 3);
	CellsMove(a.data(), a.data(), 3);
	CellsCopy(a.data(), a.data() + 1, 0);
	CHECK(expand(&a.v[0]) == want);
}

TEST_CASE("CellsMove agrees with a snapshot model on overlapping ranges", "[buffcell]")
{
	std::mt19937 rng(20260715);
	const int N = 32;

	for (int iter = 0; iter < 200; iter++) {
		CellArray a(N);
		for (int i = 0; i < N; i++) {
			char32_t base = 'A' + (rng() % 26);
			if (rng() % 4 == 0) base = 0x1F600 + (rng() % 16);
			set2(&a.v[i], base);
			int marks = rng() % 4;
			for (int m = 0; m < marks; m++) {
				CellAddChar(&a.v[i], kCombiningAcute + (rng() % 8));
			}
		}

		int count = 1 + (int)(rng() % (N - 1));
		int src = (int)(rng() % (N - count + 1));
		int dst = (int)(rng() % (N - count + 1));

		std::vector<std::vector<wchar_t>> snapshot;
		for (int i = 0; i < count; i++) {
			snapshot.push_back(expand(&a.v[src + i]));
		}

		CellsMove(a.data() + dst, a.data() + src, count);

		for (int i = 0; i < count; i++) {
			REQUIRE(expand(&a.v[dst + i]) == snapshot[i]);
		}
	}
}

TEST_CASE("CellsFill constructs each cell like CellSetChar plus attributes", "[buffcell]")
{
	CellArray filled(5);
	CellsFill(filled.data(), 0x20, 2, 4, 6, 8, 5, fake_mb, NULL);

	Cell ref;
	CellSetChar(&ref.c, 0x20, 'H', fake_mb, NULL);
	ref.c.fg = 2;
	ref.c.bg = 4;
	ref.c.attr = 6;
	ref.c.attr2 = 8;

	for (auto &c : filled.v) {
		CHECK(c.u32 == ref.c.u32);
		CHECK(c.wc2[0] == ref.c.wc2[0]);
		CHECK(c.fg == ref.c.fg);
		CHECK(c.bg == ref.c.bg);
		CHECK(c.attr == ref.c.attr);
		CHECK(c.attr2 == ref.c.attr2);
		CHECK(c.WidthProperty == 'H');
	}
}

TEST_CASE("CellExpandWchar length query and too_small boundary", "[buffcell]")
{
	Cell cell;
	set2(&cell.c, 0x1F600, 'W', 0, 1); // surrogate pair base
	CellAddChar(&cell.c, kCombiningAcute);
	const size_t len = 3;

	REQUIRE(CellExpandWchar(&cell.c, NULL, 0, NULL) == len);

	std::vector<wchar_t> buf(len, 0x7777);
	int too_small = -1;
	CHECK(CellExpandWchar(&cell.c, buf.data(), len - 1, &too_small) == len);
	CHECK(too_small == 1);
	CHECK(buf[0] == 0x7777);

	CHECK(CellExpandWchar(&cell.c, buf.data(), len, &too_small) == len);
	CHECK(too_small == 0);

	Cell padding;
	padding.c.Padding = 1;
	CHECK(CellExpandWchar(&padding.c, NULL, 0, NULL) == 0);
}

TEST_CASE("padding and width predicates", "[buffcell]")
{
	Cell cell;
	set2(&cell.c, 'x');
	CHECK(!CellIsPadding(&cell.c));
	CHECK(!CellIsFullWidth(&cell.c));

	set2(&cell.c, 0x3042, 'W', 0, 0);
	CHECK(CellIsFullWidth(&cell.c));

	cell.c.Padding = 1;
	CHECK(CellIsPadding(&cell.c));
	cell.c.Padding = 0;
	cell.c.u32 = 0;
	CHECK(CellIsPadding(&cell.c));
}

TEST_CASE("CellPtrRel wraps ring indices", "[buffcell]")
{
	const int N = 7;
	CellArray a(N);
	for (int start = 0; start < N; start++) {
		for (int rel = -3 * N; rel <= 3 * N; rel++) {
			buff_char_t *p = CellPtrRel(a.data(), N, &a.v[start], rel);
			int want = ((start + rel) % N + N) % N;
			REQUIRE(p == &a.v[want]);
		}
	}
}
