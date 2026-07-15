/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Stub bodies for the codepage entry points declared in the host <windows.h>
 * shim. Pure host tests never reach these; if one does, aborting is correct —
 * real codepage conversion is a native-Windows concern and belongs on that lane.
 */
#include <windows.h>
#include <assert.h>

int MultiByteToWideChar(UINT CodePage, DWORD dwFlags, const char *lpMultiByteStr,
						int cbMultiByte, wchar_t *lpWideCharStr, int cchWideChar)
{
	(void)CodePage; (void)dwFlags; (void)lpMultiByteStr; (void)cbMultiByte;
	(void)lpWideCharStr; (void)cchWideChar;
	assert(0 && "winhost: MultiByteToWideChar is a stub; test its callers on the native-Windows lane");
	return 0;
}

int WideCharToMultiByte(UINT CodePage, DWORD dwFlags, const wchar_t *lpWideCharStr,
						int cchWideChar, char *lpMultiByteStr, int cbMultiByte,
						const char *lpDefaultChar, LPBOOL lpUsedDefaultChar)
{
	(void)CodePage; (void)dwFlags; (void)lpWideCharStr; (void)cchWideChar;
	(void)lpMultiByteStr; (void)cbMultiByte; (void)lpDefaultChar; (void)lpUsedDefaultChar;
	assert(0 && "winhost: WideCharToMultiByte is a stub; test its callers on the native-Windows lane");
	return 0;
}

UINT GetACP(void)
{
	return CP_UTF8;
}

DWORD GetLastError(void)
{
	return 0;
}
