/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Pure parameter accumulator for the VT control-sequence parser. See vtparams.h.
 * The collection transitions are transcribed verbatim in behaviour from
 * vtterm.c's ControlSequence/EscapeSequence collection arms.
 *
 * First module modernized to the house C++23 conventions (AAA + trailing return
 * types, right-side const, anonymous-namespace internal linkage). The public
 * surface keeps C linkage and a C-compatible header because vtterm.c (still C)
 * calls it; the interface modernizes once its callers are C++.
 */
#include "vtparams.h"

#include <climits>

namespace {

// Saturating base-10 accumulate: fold a digit into the parameter, pinning at
// (int)UINT_MAX on overflow without overflowing the intermediate multiply. Once
// pinned, further digits are ignored — matching vtterm.c's old ParamIncr macro.
auto param_incr(int& p, unsigned char b) -> void
{
	if (p == static_cast<int>(UINT_MAX)) {
		return;
	}
	auto const acc = static_cast<unsigned int>(p);
	auto const digit = static_cast<unsigned int>(b - 0x30);
	if (acc > UINT_MAX / 10 || acc * 10 > UINT_MAX - digit) {
		p = static_cast<int>(UINT_MAX);
	}
	else {
		p = static_cast<int>(acc * 10 + digit);
	}
}

auto feed_intermediate(VTParams& p, unsigned char b) -> void
{
	if (p.ICount < VT_INTCHAR_MAX) {
		p.ICount++;
	}
	p.IntChar[p.ICount] = b;
}

} // namespace

auto VTParamsClear(VTParams* p) -> void
{
	p->ICount = 0;
	p->NParam = 1;
	p->NSParam[1] = 0;
	p->Param[1] = 0;
	p->Prv = 0;
}

auto VTParamsFeedCSI(VTParams* p, unsigned char b) -> VTParamsDisp
{
	if (b >= 0x20 && b <= 0x2F) { /* intermediate char */
		feed_intermediate(*p, b);
	}
	else if (b >= 0x30 && b <= 0x39) { /* parameter value */
		if (p->NSParam[p->NParam] > 0) {
			param_incr(p->SubParam[p->NParam][p->NSParam[p->NParam]], b);
		}
		else {
			param_incr(p->Param[p->NParam], b);
		}
	}
	else if (b == 0x3A) { /* ':' subparameter delimiter */
		if (p->NSParam[p->NParam] < VT_NSPARAM_MAX) {
			p->NSParam[p->NParam]++;
			p->SubParam[p->NParam][p->NSParam[p->NParam]] = 0;
		}
	}
	else if (b == 0x3B) { /* ';' parameter delimiter */
		if (p->NParam < VT_NPARAM_MAX) {
			p->NParam++;
			p->Param[p->NParam] = 0;
			p->NSParam[p->NParam] = 0;
		}
	}
	else if (b >= 0x3C && b <= 0x3F) { /* private char */
		if (p->FirstPrm) {
			p->Prv = b;
		}
	}
	else if (b > 0xA0) {
		return VTPARAMS_REDISPATCH;
	}
	return VTPARAMS_CONSUMED;
}

auto VTParamsFeedIntermediate(VTParams* p, unsigned char b) -> void
{
	feed_intermediate(*p, b);
}
