/*
Censr - real-time on-screen redaction for OBS
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <string>

namespace censr {

/* A blocking HTTPS POST. Returns true when the request completed (out_status is
 * the HTTP status, out_response the body); false on a transport failure. Blocks
 * on the network, so never call it from the render thread. Platform-native:
 * NSURLSession on macOS, WinHTTP on Windows. */
bool https_post(const std::string &url, const std::string &content_type,
		const std::string &body, long &out_status, std::string &out_response);

} // namespace censr
