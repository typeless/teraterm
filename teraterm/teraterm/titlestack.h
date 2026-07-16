/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * XTerm window-title save/restore stack (OSC 22 / OSC 23), lifted out of
 * vtterm.c. The old version was a hand-rolled singly-linked malloc/next list
 * with no free-all path, so any pushed-but-never-popped title leaked at exit.
 * This is a value container (std::vector) behind a C-compatible API: entries own
 * their strings and are released when the process ends, so the leak is closed by
 * construction. A stacked title may be "none" (the original pushed a NULL title
 * when there was no current one); that round-trips as an empty pop result.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Push a title (may be NULL = "no title"); the stack keeps its own copy. */
void TitleStackPush(const wchar_t *title);

/* 1 if the stack has an entry, else 0. */
int TitleStackEmpty(void);

/* Pop the top entry. Returns 0 (and *out = NULL) if empty; otherwise returns 1
 * and hands ownership of the popped title to the caller through *out — a
 * malloc'd copy the caller frees, or NULL if the entry was "no title". */
int TitleStackPop(wchar_t **out);

#ifdef __cplusplus
}
#endif
