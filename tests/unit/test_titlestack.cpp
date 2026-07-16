/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Host unit tests for the XTerm title save/restore stack (titlestack), extracted
 * from vtterm.c's OSC 22 / OSC 23 handling. The old linked list leaked pushed-
 * but-never-popped titles; the value-container version is exercised here for
 * LIFO order, the "no title" (NULL) round-trip, caller ownership of the popped
 * copy, and the empty-stack contract.
 */
#include "catch_amalgamated.hpp"

#include <cstdlib>
#include <cwchar>

#include "titlestack.h"

namespace {

// Drain any state left by a previous test so cases are independent (the stack
// is a process-lifetime singleton).
void drain()
{
	while (!TitleStackEmpty()) {
		wchar_t *p = nullptr;
		TitleStackPop(&p);
		free(p);
	}
}

} // namespace

TEST_CASE("empty stack contract", "[titlestack]")
{
	drain();
	CHECK(TitleStackEmpty() == 1);
	wchar_t *p = reinterpret_cast<wchar_t *>(1);
	CHECK(TitleStackPop(&p) == 0);
	CHECK(p == nullptr);
}

TEST_CASE("push/pop is LIFO and the caller owns the copy", "[titlestack]")
{
	drain();
	TitleStackPush(L"first");
	TitleStackPush(L"second");
	CHECK(TitleStackEmpty() == 0);

	wchar_t *p = nullptr;
	REQUIRE(TitleStackPop(&p) == 1);
	REQUIRE(p != nullptr);
	CHECK(wcscmp(p, L"second") == 0);
	free(p); // caller owns it

	REQUIRE(TitleStackPop(&p) == 1);
	CHECK(wcscmp(p, L"first") == 0);
	free(p);

	CHECK(TitleStackEmpty() == 1);
}

TEST_CASE("a NULL title round-trips as an empty pop result", "[titlestack]")
{
	drain();
	TitleStackPush(nullptr);
	TitleStackPush(L"real");

	wchar_t *p = nullptr;
	REQUIRE(TitleStackPop(&p) == 1);
	CHECK(wcscmp(p, L"real") == 0);
	free(p);

	// The NULL entry pops as (returned 1, *out == NULL).
	p = reinterpret_cast<wchar_t *>(1);
	REQUIRE(TitleStackPop(&p) == 1);
	CHECK(p == nullptr);

	CHECK(TitleStackEmpty() == 1);
}

TEST_CASE("the pushed copy is independent of the caller's buffer", "[titlestack]")
{
	drain();
	wchar_t buf[] = L"mutable";
	TitleStackPush(buf);
	buf[0] = L'X'; // mutate the source after pushing

	wchar_t *p = nullptr;
	REQUIRE(TitleStackPop(&p) == 1);
	CHECK(wcscmp(p, L"mutable") == 0);
	free(p);
}
