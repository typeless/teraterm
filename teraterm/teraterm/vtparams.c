/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Pure parameter accumulator for the VT control-sequence parser. See vtparams.h.
 * The collection transitions are transcribed verbatim in behaviour from
 * vtterm.c's ControlSequence/EscapeSequence collection arms.
 */
#include <limits.h>

#include "vtparams.h"

/* Saturating base-10 accumulate, matching vtterm.c's ParamIncr macro: a digit
 * is folded into the parameter, pinning at (int)UINT_MAX on overflow and never
 * overflowing the intermediate multiply. Once pinned, further digits are
 * ignored. */
static void param_incr(int *p, unsigned char b)
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

static void feed_intermediate(VTParams *p, unsigned char b)
{
	if (p->ICount < VT_INTCHAR_MAX)
		p->ICount++;
	p->IntChar[p->ICount] = b;
}

void VTParamsClear(VTParams *p)
{
	p->ICount = 0;
	p->NParam = 1;
	p->NSParam[1] = 0;
	p->Param[1] = 0;
	p->Prv = 0;
}

VTParamsDisp VTParamsFeedCSI(VTParams *p, unsigned char b)
{
	if ((b >= 0x20) && (b <= 0x2F)) { /* intermediate char */
		feed_intermediate(p, b);
	}
	else if ((b >= 0x30) && (b <= 0x39)) { /* parameter value */
		if (p->NSParam[p->NParam] > 0) {
			param_incr(&p->SubParam[p->NParam][p->NSParam[p->NParam]], b);
		}
		else {
			param_incr(&p->Param[p->NParam], b);
		}
	}
	else if (b == 0x3A) { /* ':' Subparameter delimiter */
		if (p->NSParam[p->NParam] < VT_NSPARAM_MAX) {
			p->NSParam[p->NParam]++;
			p->SubParam[p->NParam][p->NSParam[p->NParam]] = 0;
		}
	}
	else if (b == 0x3B) { /* ';' Parameter delimiter */
		if (p->NParam < VT_NPARAM_MAX) {
			p->NParam++;
			p->Param[p->NParam] = 0;
			p->NSParam[p->NParam] = 0;
		}
	}
	else if ((b >= 0x3C) && (b <= 0x3F)) { /* private char */
		if (p->FirstPrm)
			p->Prv = b;
	}
	else if (b > 0xA0) {
		return VTPARAMS_REDISPATCH;
	}
	return VTPARAMS_CONSUMED;
}

void VTParamsFeedIntermediate(VTParams *p, unsigned char b)
{
	feed_intermediate(p, b);
}
