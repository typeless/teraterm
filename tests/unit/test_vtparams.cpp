/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Host unit tests for the pure VT control-sequence parameter accumulator
 * (vtparams), extracted from vtterm.c. The module is proven byte-identical to
 * the original inline collection by a differential harness: an independent
 * transcription of ControlSequence's/EscapeSequence's collection arms (with its
 * own copy of the ParamIncr arithmetic) is fed the same byte streams as the
 * module and the entire accumulator is compared after every byte. Property
 * tests pin the saturating arithmetic; example tests pin the 16/16/5 bounds.
 * Fixed seeds keep failures reproducible.
 */
#include "catch_amalgamated.hpp"

#include <climits>
#include <cstring>
#include <random>
#include <vector>

#include "vtparams.h"

namespace {

// Independent transcription of the original vtterm.c accumulator + collection,
// used only as the differential reference. Kept structurally identical to the
// pre-extraction code so a divergence means a real behaviour change.
struct RefParams {
	int Param[VT_NPARAM_MAX + 1];
	int SubParam[VT_NPARAM_MAX + 1][VT_NSPARAM_MAX + 1];
	int NParam;
	int NSParam[VT_NPARAM_MAX + 1];
	unsigned char IntChar[VT_INTCHAR_MAX + 1];
	int ICount;
	unsigned char Prv;
	int FirstPrm;
};

// The original ParamIncr macro, transcribed as a function.
void ref_param_incr(int *p, unsigned char b)
{
	unsigned int ptmp;
	if (*p != (int)UINT_MAX) {
		ptmp = (unsigned int)(*p);
		if (ptmp > UINT_MAX / 10 || ptmp * 10 > UINT_MAX - (unsigned int)(b - 0x30)) {
			*p = (int)UINT_MAX;
		}
		else {
			*p = (int)(ptmp * 10 + b - 0x30);
		}
	}
}

void ref_clear(RefParams &p)
{
	p.ICount = 0;
	p.NParam = 1;
	p.NSParam[1] = 0;
	p.Param[1] = 0;
	p.Prv = 0;
}

// Transcription of the ControlSequence "else" collection arm. Returns true on
// the >0xA0 re-dispatch (the original's owner-side abort).
bool ref_feed_csi(RefParams &p, unsigned char b)
{
	if ((b >= 0x20) && (b <= 0x2F)) {
		if (p.ICount < VT_INTCHAR_MAX)
			p.ICount++;
		p.IntChar[p.ICount] = b;
	}
	else if ((b >= 0x30) && (b <= 0x39)) {
		if (p.NSParam[p.NParam] > 0)
			ref_param_incr(&p.SubParam[p.NParam][p.NSParam[p.NParam]], b);
		else
			ref_param_incr(&p.Param[p.NParam], b);
	}
	else if (b == 0x3A) {
		if (p.NSParam[p.NParam] < VT_NSPARAM_MAX) {
			p.NSParam[p.NParam]++;
			p.SubParam[p.NParam][p.NSParam[p.NParam]] = 0;
		}
	}
	else if (b == 0x3B) {
		if (p.NParam < VT_NPARAM_MAX) {
			p.NParam++;
			p.Param[p.NParam] = 0;
			p.NSParam[p.NParam] = 0;
		}
	}
	else if ((b >= 0x3C) && (b <= 0x3F)) {
		if (p.FirstPrm)
			p.Prv = b;
	}
	else if (b > 0xA0) {
		return true;
	}
	return false;
}

// Transcription of the EscapeSequence intermediate arm (0x20-0x2F).
void ref_feed_esc_intermediate(RefParams &p, unsigned char b)
{
	if (p.ICount < VT_INTCHAR_MAX)
		p.ICount++;
	p.IntChar[p.ICount] = b;
}

// Whole-struct equality, so shrinking localises the first divergent byte.
bool equal(const RefParams &r, const VTParams &m)
{
	if (r.NParam != m.NParam || r.ICount != m.ICount || r.Prv != m.Prv ||
	    r.FirstPrm != m.FirstPrm)
		return false;
	if (memcmp(r.Param, m.Param, sizeof(r.Param)) != 0)
		return false;
	if (memcmp(r.SubParam, m.SubParam, sizeof(r.SubParam)) != 0)
		return false;
	if (memcmp(r.NSParam, m.NSParam, sizeof(r.NSParam)) != 0)
		return false;
	if (memcmp(r.IntChar, m.IntChar, sizeof(r.IntChar)) != 0)
		return false;
	return true;
}

// Both sides start from the same arbitrary-but-identical memory so the harness
// also exercises the partial reset (stale high indices must round-trip).
void seed(RefParams &r, VTParams &m, unsigned int salt)
{
	std::mt19937 rng(salt);
	unsigned char *rp = reinterpret_cast<unsigned char *>(&r);
	unsigned char *mp = reinterpret_cast<unsigned char *>(&m);
	static_assert(sizeof(RefParams) == sizeof(VTParams), "layouts must match");
	for (size_t i = 0; i < sizeof(RefParams); i++) {
		unsigned char v = (unsigned char)rng();
		rp[i] = v;
		mp[i] = v;
	}
}

} // namespace

TEST_CASE("VTParamsClear matches the original partial reset", "[vtparams]")
{
	RefParams r;
	VTParams m;
	for (unsigned int s = 0; s < 64; s++) {
		seed(r, m, 0xC0FFEE + s);
		ref_clear(r);
		VTParamsClear(&m);
		REQUIRE(equal(r, m));
	}
}

TEST_CASE("CSI collection is byte-identical to the original over full-byte fuzz", "[vtparams]")
{
	std::mt19937 rng(20260715);
	for (int iter = 0; iter < 4000; iter++) {
		RefParams r;
		VTParams m;
		seed(r, m, rng());

		// Start a fresh sequence, like the owner: ClearParams + FirstPrm = TRUE.
		ref_clear(r);
		VTParamsClear(&m);
		r.FirstPrm = m.FirstPrm = 1;

		int len = 1 + (int)(rng() % 40);
		for (int k = 0; k < len; k++) {
			// Bias toward the collection range but include every byte class,
			// and occasionally restart a sequence mid-stream.
			unsigned char b;
			unsigned int roll = rng() % 100;
			if (roll < 70)
				b = (unsigned char)(0x20 + rng() % 0x20); // 0x20-0x3F collection
			else if (roll < 78)
				b = ';';
			else if (roll < 86)
				b = ':';
			else if (roll < 90)
				b = (unsigned char)(0x3C + rng() % 4); // private
			else
				b = (unsigned char)(rng() % 0x100); // anything, incl >0xA0 / no-ops

			if (roll >= 98) {
				ref_clear(r);
				VTParamsClear(&m);
				r.FirstPrm = m.FirstPrm = 1;
				continue;
			}

			bool ref_redis = ref_feed_csi(r, b);
			VTParamsDisp d = VTParamsFeedCSI(&m, b);
			REQUIRE(((d == VTPARAMS_REDISPATCH) == ref_redis));
			// The owner clears FirstPrm after every CSI byte.
			r.FirstPrm = 0;
			m.FirstPrm = 0;
			REQUIRE(equal(r, m));
		}
	}
}

TEST_CASE("every byte value behaves identically from a fresh sequence", "[vtparams]")
{
	for (int first = 0; first < 0x100; first++) {
		for (int second = 0; second < 0x100; second++) {
			RefParams r;
			VTParams m;
			seed(r, m, (unsigned int)(first * 257 + second) + 1);
			ref_clear(r);
			VTParamsClear(&m);
			r.FirstPrm = m.FirstPrm = 1;

			bool rd1 = ref_feed_csi(r, (unsigned char)first);
			VTParamsDisp d1 = VTParamsFeedCSI(&m, (unsigned char)first);
			r.FirstPrm = m.FirstPrm = 0;
			REQUIRE(((d1 == VTPARAMS_REDISPATCH) == rd1));
			REQUIRE(equal(r, m));

			bool rd2 = ref_feed_csi(r, (unsigned char)second);
			VTParamsDisp d2 = VTParamsFeedCSI(&m, (unsigned char)second);
			r.FirstPrm = m.FirstPrm = 0;
			REQUIRE(((d2 == VTPARAMS_REDISPATCH) == rd2));
			REQUIRE(equal(r, m));
		}
	}
}

TEST_CASE("ESC intermediate collection is byte-identical", "[vtparams]")
{
	std::mt19937 rng(0x5C0FF);
	for (int iter = 0; iter < 2000; iter++) {
		RefParams r;
		VTParams m;
		seed(r, m, rng());
		ref_clear(r);
		VTParamsClear(&m);

		int len = 1 + (int)(rng() % 12);
		for (int k = 0; k < len; k++) {
			unsigned char b = (unsigned char)(0x20 + rng() % 0x10); // 0x20-0x2F
			ref_feed_esc_intermediate(r, b);
			VTParamsFeedIntermediate(&m, b);
			REQUIRE(equal(r, m));
		}
	}
}

TEST_CASE("parameter digit saturates and pins (law)", "[vtparams]")
{
	// Property: feeding digits is monotone non-decreasing, never overflows, and
	// once pinned at (int)UINT_MAX stays pinned regardless of further digits.
	std::mt19937 rng(99);
	for (int iter = 0; iter < 3000; iter++) {
		VTParams m;
		VTParamsClear(&m);
		m.FirstPrm = 1;
		unsigned int prev = 0;
		int ndigits = 1 + (int)(rng() % 15);
		bool pinned = false;
		for (int k = 0; k < ndigits; k++) {
			unsigned char d = (unsigned char)('0' + rng() % 10);
			VTParamsFeedCSI(&m, d);
			unsigned int cur = (unsigned int)m.Param[1];
			REQUIRE(cur >= prev);                 // monotone
			REQUIRE(cur <= (unsigned int)UINT_MAX);
			if (pinned) REQUIRE(cur == (unsigned int)UINT_MAX);
			if (cur == (unsigned int)UINT_MAX) pinned = true;
			prev = cur;
		}
	}
}

TEST_CASE("small parameters accumulate exactly", "[vtparams]")
{
	VTParams m;
	VTParamsClear(&m);
	m.FirstPrm = 1;
	for (char c : std::string("12345"))
		VTParamsFeedCSI(&m, (unsigned char)c);
	CHECK(m.Param[1] == 12345);
}

TEST_CASE("parameter and subparameter delimiters and bounds", "[vtparams]")
{
	VTParams m;
	VTParamsClear(&m);
	m.FirstPrm = 1;

	// "1;2;3" -> three params
	for (char c : std::string("1;2;3"))
		VTParamsFeedCSI(&m, (unsigned char)c);
	CHECK(m.NParam == 3);
	CHECK(m.Param[1] == 1);
	CHECK(m.Param[2] == 2);
	CHECK(m.Param[3] == 3);

	// ':' routes subsequent digits to the subparameter.
	VTParamsClear(&m);
	for (char c : std::string("38:5:200"))
		VTParamsFeedCSI(&m, (unsigned char)c);
	CHECK(m.Param[1] == 38);
	CHECK(m.NSParam[1] == 2);
	CHECK(m.SubParam[1][1] == 5);
	CHECK(m.SubParam[1][2] == 200);
}

TEST_CASE("delimiter counts clamp at the maxima", "[vtparams]")
{
	VTParams m;
	VTParamsClear(&m);
	m.FirstPrm = 1;
	for (int i = 0; i < 40; i++)
		VTParamsFeedCSI(&m, ';');
	CHECK(m.NParam == VT_NPARAM_MAX);

	VTParamsClear(&m);
	for (int i = 0; i < 40; i++)
		VTParamsFeedCSI(&m, ':');
	CHECK(m.NSParam[1] == VT_NSPARAM_MAX);

	VTParamsClear(&m);
	for (int i = 0; i < 40; i++)
		VTParamsFeedCSI(&m, '!'); // 0x21, an intermediate
	CHECK(m.ICount == VT_INTCHAR_MAX);
	CHECK(m.IntChar[VT_INTCHAR_MAX] == '!');
}

TEST_CASE("private marker only latches on the first byte of a sequence", "[vtparams]")
{
	VTParams m;
	VTParamsClear(&m);
	m.FirstPrm = 1;
	CHECK(VTParamsFeedCSI(&m, '?') == VTPARAMS_CONSUMED);
	CHECK(m.Prv == '?');

	// A private char not on the first byte (FirstPrm cleared) must NOT latch.
	VTParamsClear(&m);
	m.FirstPrm = 0;
	VTParamsFeedCSI(&m, '>');
	CHECK(m.Prv == 0);
}

TEST_CASE("byte > 0xA0 asks for re-dispatch without touching the accumulator", "[vtparams]")
{
	VTParams m;
	VTParamsClear(&m);
	m.FirstPrm = 1;
	for (char c : std::string("12;34"))
		VTParamsFeedCSI(&m, (unsigned char)c);
	VTParams before;
	memcpy(&before, &m, sizeof(m));
	CHECK(VTParamsFeedCSI(&m, 0xA1) == VTPARAMS_REDISPATCH);
	CHECK(memcmp(&before, &m, sizeof(m)) == 0);

	// 0x7F and 0xA0 are silent no-ops, consumed, no state change.
	CHECK(VTParamsFeedCSI(&m, 0x7F) == VTPARAMS_CONSUMED);
	CHECK(VTParamsFeedCSI(&m, 0xA0) == VTPARAMS_CONSUMED);
	CHECK(memcmp(&before, &m, sizeof(m)) == 0);
}
