/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Pure parameter accumulator for the VT control-sequence parser, extracted from
 * vtterm.c. It holds the numeric parameters, sub-parameters, intermediate bytes
 * and private-marker collected while a CSI or ESC sequence is being read, and
 * the transitions that build them from incoming bytes — including the saturating
 * parameter arithmetic. No ts/cv globals, no comm layer, no <windows.h>, so it
 * compiles and unit-tests on the host and is differentially checkable against
 * the original inline collection.
 *
 * Scope note: the owner (vtterm.c) keeps everything effectful — the C0/C1
 * control handling and final-byte dispatch that surround collection, the
 * printer passthrough, the >0xA0 ISO-2022 re-dispatch (this module only reports
 * that a byte calls for it), and FirstPrm's lifetime (set at sequence entry,
 * cleared after each CSI byte). The DCS parameter dialect is deliberately NOT
 * handled here: it uses a different, unclamped arithmetic and stays inline.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define VT_NPARAM_MAX  16
#define VT_NSPARAM_MAX 16
#define VT_INTCHAR_MAX  5

typedef struct {
	int Param[VT_NPARAM_MAX + 1];
	int SubParam[VT_NPARAM_MAX + 1][VT_NSPARAM_MAX + 1];
	int NParam;
	int NSParam[VT_NPARAM_MAX + 1];
	unsigned char IntChar[VT_INTCHAR_MAX + 1];
	int ICount;
	unsigned char Prv;
	int FirstPrm; /* BOOL; owned by vtterm.c, read here for the private marker */
} VTParams;

typedef enum {
	VTPARAMS_CONSUMED,   /* byte was collected (or is a silent no-op) */
	VTPARAMS_REDISPATCH, /* byte > 0xA0: owner must abort to ParseFirst */
} VTParamsDisp;

/* Reset before a new sequence. Matches vtterm.c's ClearParams exactly: it is a
 * PARTIAL reset — ICount, NParam, NSParam[1], Param[1], Prv only. It does not
 * touch FirstPrm, Param[2..], SubParam, NSParam[2..], or IntChar; the higher
 * indices are stale until reused, by design. */
void VTParamsClear(VTParams *p);

/* Feed one collection-range byte in CSI context (the ControlSequence "else"
 * arm): intermediate 0x20-0x2F, parameter digit 0x30-0x39 (saturating),
 * sub-parameter ':' 0x3A, parameter ';' 0x3B, private 0x3C-0x3F (only while
 * FirstPrm). Returns VTPARAMS_REDISPATCH for b > 0xA0; other unmatched bytes
 * (0x7F, 0xA0) are silent no-ops. The caller must already have handled C0/C1
 * controls, the final byte 0x40-0x7E, and printer passthrough. */
VTParamsDisp VTParamsFeedCSI(VTParams *p, unsigned char b);

/* Collect one intermediate byte (0x20-0x2F) in ESC context. */
void VTParamsFeedIntermediate(VTParams *p, unsigned char b);

#ifdef __cplusplus
}
#endif
