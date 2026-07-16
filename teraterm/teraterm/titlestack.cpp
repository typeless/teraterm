/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * XTerm window-title save/restore stack. See titlestack.h.
 */
#include "titlestack.h"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

// Function-local static: constructed on first use, destroyed at process exit,
// which is what closes the old leak. std::nullopt is the "no title" entry.
auto stack() -> std::vector<std::optional<std::wstring>>&
{
	static std::vector<std::optional<std::wstring>> s;
	return s;
}

} // namespace

// The C boundary is noexcept: an allocation failure must not unwind into the C
// caller (vtterm.c). On OOM a push is silently dropped, matching the original's
// behaviour when its malloc/_wcsdup failed.
auto TitleStackPush(const wchar_t* title) -> void
{
	try {
		if (title == nullptr) {
			stack().emplace_back(std::nullopt);
		}
		else {
			stack().emplace_back(std::wstring(title));
		}
	}
	catch (...) {
	}
}

auto TitleStackEmpty(void) -> int
{
	return stack().empty() ? 1 : 0;
}

auto TitleStackPop(wchar_t** out) -> int
{
	auto& s = stack();
	if (s.empty()) {
		*out = nullptr;
		return 0;
	}
	auto entry = std::move(s.back());
	s.pop_back();
	if (!entry.has_value()) {
		*out = nullptr;
		return 1;
	}
	size_t const n = entry->size() + 1;
	auto* copy = static_cast<wchar_t*>(malloc(n * sizeof(wchar_t)));
	if (copy != nullptr) {
		memcpy(copy, entry->c_str(), n * sizeof(wchar_t));
	}
	*out = copy;
	return 1;
}
