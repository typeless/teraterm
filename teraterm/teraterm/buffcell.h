/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Pure cell layer of the VT screen buffer, extracted from buffer.c: the
 * per-cell character record (buff_char_t) and the primitives that construct,
 * copy, move, and expand cells — including the heap-allocated combining-
 * character buffers. No ts/cv globals, no buffer geometry, no Win32 beyond
 * what codeconv.h already carries, so the layer compiles and unit-tests on
 * the host lane. The multibyte conversion feeding the precomputed ansi_char
 * is Win32-bound (WideCharToMultiByte behind UTF32ToMBCP), so it is injected
 * as a callback in UTF32ToMBCP's exact shape; the owner (buffer.c) binds its
 * CodePage static plus the U+203E/CP932 policy there, while the ansi_char
 * byte packing — the field's representation — stays in this layer.
 */
#pragma once

#include <stdlib.h>	// for size_t, wchar_t
#include "ttcstd.h"	// for char32_t in C

#ifdef __cplusplus
extern "C" {
#endif

#define	ENABLE_CELL_INDEX	0

// バッファ内の半角1文字分の情報
typedef struct {
	char32_t u32;
	char32_t u32_last;
	char WidthProperty;				// 'W' or 'F' or 'H' or 'A' or 'n'(Narrow) or 'N'(Neutual) (文字の属性)
	char cell;			// 文字のcell数 1/2/3+=半角,全角,3以上
						// 2以上のとき、この文字の後ろにpaddingがcell-1個続く
	char Padding;					// TRUE = 全角の次の詰め物 or 行末の詰め物
	char Emoji;						// TRUE = 絵文字
	unsigned char CombinationCharCount16;	// character count
	unsigned char CombinationCharSize16;		// buffer size
	unsigned char CombinationCharCount32;
	unsigned char CombinationCharSize32;
	wchar_t *pCombinationChars16;
	char32_t *pCombinationChars32;
	wchar_t	wc2[2];
	unsigned char fg;
	unsigned char bg;
	unsigned char attr;
	unsigned char attr2;
	unsigned short ansi_char;
#if ENABLE_CELL_INDEX
	int idx;	// セル通し番号
#endif
} buff_char_t;

// 1文字あたりのコンビネーションバッファ最大サイズ
#define MAX_CHAR_SIZE	100

#define AttrDefault       0x00
#define AttrDefaultFG     0x00
#define AttrDefaultBG     0x00

/* Multibyte conversion for the precomputed ansi_char, UTF32ToMBCP's shape
 * with ctx in place of the code page. Called only for u32 >= 0x80; returns
 * the byte count written to mb_ptr (0 = not convertible). */
typedef size_t (*CellToMB)(char32_t u32, void *ctx, char *mb_ptr, size_t mb_len);

/* Cell construction. */
void CellSetChar2(buff_char_t *cell, char32_t u32, char property, int half_width, char emoji,
                  CellToMB to_mb, void *to_mb_ctx);
void CellSetChar4(buff_char_t *cell, char32_t u32, unsigned char fg, unsigned char bg,
                  unsigned char attr, unsigned char attr2, char property,
                  CellToMB to_mb, void *to_mb_ctx);
void CellSetChar(buff_char_t *cell, char32_t u32, char property, CellToMB to_mb, void *to_mb_ctx);

/* Append a combining character (grows the per-cell heap buffers, capped at
 * MAX_CHAR_SIZE). The cell must already hold a base character. */
void CellAddChar(buff_char_t *cell, char32_t u32);

/* Release a cell's combining-character buffers. */
void CellFreeCombination(buff_char_t *cell);

/* Deep single-cell copy (clones the combining buffers). */
void CellCopy(buff_char_t *dest, const buff_char_t *src);

/* Deep cell-range operations (memcpy/memset/memmove over buff_char_t). */
void CellsCopy(buff_char_t *dest, const buff_char_t *src, size_t count);
void CellsFill(buff_char_t *dest, wchar_t ch, unsigned char fg, unsigned char bg,
               unsigned char attr, unsigned char attr2, size_t count,
               CellToMB to_mb, void *to_mb_ctx);
void CellsMove(buff_char_t *dest, const buff_char_t *src, size_t count);

int CellIsPadding(const buff_char_t *cell);
int CellIsFullWidth(const buff_char_t *cell);

/* Expand a cell to its full UTF-16 sequence (base + combining). buf==NULL
 * queries the length. */
size_t CellExpandWchar(const buff_char_t *cell, wchar_t *buf, size_t buf_size, int *too_small);

/* Move a cell pointer rel cells within a ring buffer of buffer_size cells. */
buff_char_t *CellPtrRel(buff_char_t *base, size_t buffer_size, buff_char_t *p, int rel);

#ifdef __cplusplus
}
#endif
