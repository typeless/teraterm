/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Pure cell layer of the VT screen buffer, moved verbatim from buffer.c.
 * See buffcell.h.
 */
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "codeconv.h"

#include "buffcell.h"

buff_char_t *CellPtrRel(buff_char_t *base, size_t buffer_size, buff_char_t *p, int rel)
{
	ptrdiff_t idx = (ptrdiff_t)(p - base) + rel;
	for (;;) {
		if (idx < 0) {
			idx += buffer_size;
		}
		else if (idx >= (ptrdiff_t)buffer_size) {
			idx -= buffer_size;
		}
		else {
			break;
		}
	}
	p = &base[(int)idx];
	return p;
}

void CellFreeCombination(buff_char_t *b)
{
	if (b->pCombinationChars16 != NULL) {
		free(b->pCombinationChars16);
		b->pCombinationChars16 = NULL;
	}
	b->CombinationCharSize16 = 0;
	b->CombinationCharCount16 = 0;

	if (b->pCombinationChars32 != NULL) {
		free(b->pCombinationChars32);
		b->pCombinationChars32 = NULL;
	}
	b->CombinationCharSize32 = 0;
	b->CombinationCharCount32 = 0;
}

static void DupCombinationBuf(buff_char_t *b)
{
	size_t size;

	size = b->CombinationCharSize16;
	if (size > 0) {
		wchar_t *new_buf = (wchar_t *)malloc(sizeof(wchar_t) * size);
		memcpy(new_buf, b->pCombinationChars16, sizeof(wchar_t) * size);
		b->pCombinationChars16 = new_buf;
	}
	size = b->CombinationCharSize32;
	if (size > 0) {
		char32_t *new_buf = (char32_t *)malloc(sizeof(char32_t) * size);
		memcpy(new_buf, b->pCombinationChars32, sizeof(char32_t) * size);
		b->pCombinationChars32 = new_buf;
	}
}

void CellCopy(buff_char_t *dest, const buff_char_t *src)
{
	CellFreeCombination(dest);

	// 構造体をコピーする
#if ENABLE_CELL_INDEX
	int idx = dest->idx;
#endif
	*dest = *src;
#if ENABLE_CELL_INDEX
	dest->idx = idx;
#endif

	DupCombinationBuf(dest);
}

void CellSetChar2(buff_char_t *cell, char32_t u32, char property, int half_width, char emoji,
                  CellToMB to_mb, void *to_mb_ctx)
{
	size_t wstr_len;
	buff_char_t *p = cell;

	CellFreeCombination(p);
	p->WidthProperty = property;
	p->cell = half_width ? 1 : 2;
	p->u32 = u32;
	p->u32_last = u32;
	p->Padding = 0;
	p->Emoji = emoji;
	p->fg = AttrDefaultFG;
	p->bg = AttrDefaultBG;

	//
	wstr_len = UTF32ToUTF16(u32, &p->wc2[0], 2);
	switch (wstr_len) {
	case 0:
	default:
		p->wc2[0] = 0;
		p->wc2[1] = 0;
		break;
	case 1:
		p->wc2[1] = 0;
		break;
	case 2:
		break;
	}

	if (u32 < 0x80) {
		p->ansi_char = (unsigned short)u32;
	}
	else {
		char strA[4];
		size_t lenA = to_mb(u32, to_mb_ctx, strA, sizeof(strA));
		switch (lenA) {
		case 0:
		default:
			p->ansi_char = '?';
			break;
		case 1:
			p->ansi_char = (unsigned char)strA[0];
			break;
		case 2:
			p->ansi_char = (unsigned char)strA[1] | ((unsigned char)strA[0] << 8);
			break;
		}
	}
}

void CellSetChar4(buff_char_t *cell, char32_t u32, unsigned char fg, unsigned char bg,
                  unsigned char attr, unsigned char attr2, char property,
                  CellToMB to_mb, void *to_mb_ctx)
{
	buff_char_t *p = cell;
	CellSetChar2(p, u32, property, 1, 0, to_mb, to_mb_ctx);
	p->fg = fg;
	p->bg = bg;
	p->attr = attr;
	p->attr2 = attr2;
}

void CellSetChar(buff_char_t *cell, char32_t u32, char property, CellToMB to_mb, void *to_mb_ctx)
{
	CellSetChar2(cell, u32, property, 1, 0, to_mb, to_mb_ctx);
}

/**
 *	文字の追加、コンビネーション
 */
void CellAddChar(buff_char_t *cell, char32_t u32)
{
	buff_char_t *p = cell;
	assert(p->u32 != 0);
	// 後に続く文字領域を拡大する
	if (p->CombinationCharSize16 < p->CombinationCharCount16 + 2) {
		size_t new_size = p->CombinationCharSize16;
		new_size = new_size == 0 ? 5 : new_size * 2;
		if (new_size > MAX_CHAR_SIZE) {
			new_size = MAX_CHAR_SIZE;
		}
		if (p->CombinationCharSize16 != new_size) {
			p->pCombinationChars16 = (wchar_t *)realloc(p->pCombinationChars16, sizeof(wchar_t) * new_size);
			p->CombinationCharSize16 = (char)new_size;
		}
	}
	if (p->CombinationCharSize32 < p->CombinationCharCount32 + 2) {
		size_t new_size = p->CombinationCharSize32;
		new_size = new_size == 0 ? 5 : new_size * 2;
		if (new_size > MAX_CHAR_SIZE) {
			new_size = MAX_CHAR_SIZE;
		}
		if (p->CombinationCharSize32 != new_size) {
			p->pCombinationChars32 = (char32_t *)realloc(p->pCombinationChars32, sizeof(char32_t) * new_size);
			p->CombinationCharSize32 = (char)new_size;
		}
	}

	// UTF-32
	if (p->CombinationCharCount32 < p->CombinationCharSize32) {
		p->u32_last = u32;
		p->pCombinationChars32[(size_t)p->CombinationCharCount32] = u32;
		p->CombinationCharCount32++;
	}

	// UTF-16
	if (p->CombinationCharCount16 < p->CombinationCharSize16) {
		wchar_t u16_str[2];
		size_t wlen = UTF32ToUTF16(u32, &u16_str[0], 2);
		if (p->CombinationCharCount16 + wlen <= p->CombinationCharSize16) {
			size_t i = (size_t)p->CombinationCharCount16;
			p->pCombinationChars16[i] = u16_str[0];
			if (wlen == 2) {
				i++;
				p->pCombinationChars16[i] = u16_str[1];
			}
			p->CombinationCharCount16 += (unsigned char)wlen;
		}
	}
}

void CellsCopy(buff_char_t *dest, const buff_char_t *src, size_t count)
{
	size_t i;

	if (dest == src || count == 0) {
		return;
	}

	for (i = 0; i < count; i++) {
		CellCopy(dest, src);
		dest++;
		src++;
	}
}

void CellsFill(buff_char_t *dest, wchar_t ch, unsigned char fg, unsigned char bg,
               unsigned char attr, unsigned char attr2, size_t count,
               CellToMB to_mb, void *to_mb_ctx)
{
	size_t i;
	for (i=0; i<count; i++) {
		CellSetChar(dest, ch, 'H', to_mb, to_mb_ctx);
		dest->fg = fg;
		dest->bg = bg;
		dest->attr = attr;
		dest->attr2 = attr2;
		dest++;
	}
}

void CellsMove(buff_char_t *dest, const buff_char_t *src, size_t count)
{
	size_t i;

	if (dest == src || count == 0) {
		return;
	}


	if (dest < src) {
		// 前からコピーする? -> CellsCopy() でok
		CellsCopy(dest, src, count);
	}
	else {
		// 後ろからコピーする
		dest += count - 1;
		src += count - 1;
		for (i = 0; i < count; i++) {
			CellCopy(dest, src);
			dest--;
			src--;
		}
	}
}

int CellIsPadding(const buff_char_t *cell)
{
	if (cell->Padding == 1)
		return 1;
	if (cell->u32 == 0)
		return 1;
	return 0;
}

int CellIsFullWidth(const buff_char_t *cell)
{
	if (cell->cell != 1)
		return 1;
	return 0;
}

/**
 *	1セルを構成する文字列(wchar_t)を取り出す
 *
 *	@param			b			1文字を指すバッファへのポインタ
 *	@param[out]		buf			文字列出力先(NULLのとき長さだけを返す)
 *	@param			buf_size	出力先文字数(wchar_t単位)
 *	@param[out]		too_small	出力先が小さすぎる(NULL可)
 *	@retval			文字数		出力文字数
 *								0のとき、文字出力なし
 */
size_t CellExpandWchar(const buff_char_t *cell, wchar_t *buf, size_t buf_size, int *too_small)
{
	const buff_char_t *b = cell;
	size_t len;

	if (CellIsPadding(b)) {
		if (too_small != NULL) {
			*too_small = 0;
		}
		return 0;
	}

	// 長さを測る
	len = 0;
	if (b->wc2[1] == 0) {
		// サロゲートペアではない
		len++;
	} else {
		// サロゲートペア
		len += 2;
	}
	// コンビネーション
	len += b->CombinationCharCount16;

	if (buf == NULL) {
		// 長さだけを返す
		return len;
	}

	// バッファに収まる?
	if (len > buf_size) {
		// バッファに収まらない
		if (too_small != NULL) {
			*too_small = 1;
		}
		return len;
	}
	if (too_small != NULL) {
		*too_small = 0;
	}

	// 展開していく
	*buf++ = b->wc2[0];
	if (b->wc2[1] != 0) {
		*buf++ = b->wc2[1];
	}
	if (b->CombinationCharCount16 != 0) {
		memcpy(buf, b->pCombinationChars16, b->CombinationCharCount16 * sizeof(wchar_t));
	}

	return len;
}
