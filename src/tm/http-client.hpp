/*
obs-tm-director — VEX Tournament Manager automation for OBS Studio
Copyright (C) 2026 Sydney Robotics Academy <kevin.li@sydneyrobotics.com.au>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace tmd {

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

struct HttpResponse {
	/// HTTP status, or 0 when the request never got a response.
	long status = 0;
	std::string body;

	/// Value of the Last-Modified response header, empty when absent. Kept so
	/// the caller can echo it back as If-Modified-Since on the next poll.
	std::string lastModified;

	/// Value of the Date response header, empty when absent.
	///
	/// This is how clock skew gets detected without guessing at TM's error
	/// strings: the server tells us its own clock on every response, so we can
	/// compare it to ours and name the problem precisely. Signing is bound to
	/// x-tm-date, so a skewed clock otherwise looks exactly like a bad API key.
	std::string date;

	/// True when the request failed below HTTP: DNS, connect, timeout, TLS.
	/// Distinguishing this from an HTTP error status is what lets the client
	/// tell "TM is not there" from "TM said no".
	bool transportFailed = false;
	std::string transportError;

	bool ok() const { return !transportFailed && status >= 200 && status < 300; }
	bool notModified() const { return !transportFailed && status == 304; }
};

/// Minimal blocking HTTP client over libcurl.
///
/// Deliberately not a general-purpose HTTP library: it does exactly the two
/// request shapes this plugin needs - a signed GET against TM, and a POST
/// carrying an Authorization header to mint a bearer - and nothing else.
class HttpClient {
public:
	HttpClient();
	~HttpClient();

	HttpClient(const HttpClient &) = delete;
	HttpClient &operator=(const HttpClient &) = delete;

	HttpResponse get(const std::string &url, const HttpHeaders &headers, long timeoutMs = kDefaultTimeoutMs);

	/// POSTs `body` with `headers`. An empty body still sends Content-Length: 0,
	/// which is what Cerberus's token endpoint expects.
	HttpResponse post(const std::string &url, const std::string &body, const HttpHeaders &headers,
			  long timeoutMs = kDefaultTimeoutMs);

	static constexpr long kDefaultTimeoutMs = 10000;
};

} // namespace tmd
