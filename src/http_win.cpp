/*
Censr - real-time on-screen redaction for OBS
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

/* Windows HTTPS via WinHTTP. */

#include "http.h"

#include <windows.h>
#include <winhttp.h>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace censr {

static std::wstring widen(const std::string &s)
{
	if (s.empty())
		return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
	return w;
}

bool https_post(const std::string &url, const std::string &content_type,
		const std::string &body, long &out_status, std::string &out_response)
{
	out_status = 0;
	out_response.clear();

	std::wstring wurl = widen(url);
	URL_COMPONENTS uc = {};
	uc.dwStructSize = sizeof(uc);
	wchar_t host[256] = {0};
	wchar_t path[2048] = {0};
	uc.lpszHostName = host;
	uc.dwHostNameLength = _countof(host);
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = _countof(path);
	if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc))
		return false;

	HINTERNET session = WinHttpOpen(L"Censr/1.0",
					WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
					WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session)
		return false;

	bool ok = false;
	HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
	if (connect) {
		DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS)
				      ? WINHTTP_FLAG_SECURE
				      : 0;
		HINTERNET request = WinHttpOpenRequest(
			connect, L"POST", path, nullptr, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
		if (request) {
			std::wstring headers = L"Content-Type: " +
					       widen(content_type) +
					       L"\r\nAccept: application/json";
			if (WinHttpSendRequest(request, headers.c_str(),
					       (DWORD)-1L, (LPVOID)body.data(),
					       (DWORD)body.size(),
					       (DWORD)body.size(), 0) &&
			    WinHttpReceiveResponse(request, nullptr)) {
				DWORD status = 0, len = sizeof(status);
				WinHttpQueryHeaders(
					request,
					WINHTTP_QUERY_STATUS_CODE |
						WINHTTP_QUERY_FLAG_NUMBER,
					WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
					WINHTTP_NO_HEADER_INDEX);
				out_status = (long)status;

				DWORD avail = 0;
				do {
					avail = 0;
					if (!WinHttpQueryDataAvailable(request, &avail))
						break;
					if (!avail)
						break;
					std::vector<char> buf(avail);
					DWORD read = 0;
					if (!WinHttpReadData(request, buf.data(), avail,
							     &read))
						break;
					out_response.append(buf.data(), read);
				} while (avail > 0);
				ok = true;
			}
			WinHttpCloseHandle(request);
		}
		WinHttpCloseHandle(connect);
	}
	WinHttpCloseHandle(session);
	return ok;
}

} // namespace censr
