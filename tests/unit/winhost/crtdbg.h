/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Host stub for <crtdbg.h>. Core TUs include it for the MSVC debug-heap macros;
 * on the host those are no-ops so the include just needs to resolve.
 */
#pragma once

#include <assert.h>

#ifndef _ASSERTE
#define _ASSERTE(expr) assert(expr)
#endif
#ifndef _ASSERT
#define _ASSERT(expr) assert(expr)
#endif
