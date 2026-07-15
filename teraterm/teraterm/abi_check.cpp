/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Compile-time freeze of the plugin-ABI struct layout. TTTSet, TComVar, and
 * TTXExports are shared by binary layout with every loaded TTX plugin (ttxssh,
 * TTProxy, TTXKanjiMenu, ...). Any accidental reorder or insert that changes
 * their size or a sentinel offset must break THIS compile, not a customer's
 * already-built plugin. Grow the structs append-only (or reuse reserve_N slots);
 * when a change is deliberate, bump the frozen number here in the same commit.
 *
 * Frozen for target x86_64-pc-windows-msvc (clang-cl). A different ABI (x86,
 * ARM64) has different numbers — if this file is ever built for another target,
 * gate the constants on the architecture.
 */
#include <stddef.h>

#include "teraterm.h"
#include "tttypes.h"
#include "ttplugin.h"

static_assert(sizeof(TTTSet) == 10632, "TTTSet layout changed; see note above");
static_assert(sizeof(TComVar) == 98768, "TComVar layout changed; see note above");
static_assert(sizeof(TTXExports) == 112, "TTXExports layout changed; see note above");

/* Sentinel offsets catch a same-size field reorder that the sizeof checks miss. */
static_assert(offsetof(TTXExports, size) == 0, "TTXExports.size moved");
static_assert(offsetof(TTXExports, loadOrder) == 4, "TTXExports.loadOrder moved");

static_assert(offsetof(TComVar, ts) == 98736, "TComVar.ts moved; layout changed");
