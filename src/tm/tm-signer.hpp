/*
obs-tm-director
Copyright (C) 2026 Sydney Robotics Academy

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

namespace tmd {

/// The pieces of a URL that participate in a TM API request signature.
///
/// `host` follows WHATWG URL semantics: it carries the port only when the port
/// is not the default for the scheme. `http://tm.local/x` signs `host:tm.local`,
/// while `http://tm.local:8080/x` signs `host:tm.local:8080`. Getting this wrong
/// produces a 401 that looks like a bad API key, so it is pinned by test vectors.
struct SignableUrl {
	std::string host;         ///< hostname, plus ":port" when non-default
	std::string pathAndQuery; ///< pathname + search, e.g. "/api/matches/1?x=1"
};

/// Splits an absolute http(s) URL into its signable parts.
/// Returns false if the input is not a parseable absolute URL.
bool parseSignableUrl(const std::string &url, SignableUrl &out);

/// Builds the canonical string that gets HMAC'd.
///
/// Layout (note the trailing newline after the final component):
///
///     METHOD \n
///     pathAndQuery \n
///     token:<bearerToken> \n
///     host:<host> \n
///     x-tm-date:<tmDate> \n
///
/// Mirrors vex-tm-client v1.5.3 (out/Client.js), the reference implementation.
std::string buildStringToSign(const std::string &method, const SignableUrl &url, const std::string &bearerToken,
			      const std::string &tmDate);

/// HMAC-SHA256 of `message` under `key`, returned as lowercase hex.
std::string hmacSha256Hex(const std::string &key, const std::string &message);

/// Convenience: parse, canonicalise, and sign in one step.
/// Returns an empty string if `url` could not be parsed.
std::string signRequest(const std::string &method, const std::string &url, const std::string &bearerToken,
			const std::string &tmDate, const std::string &apiKey);

/// Formats a UTC time_t as an RFC 7231 HTTP-date, the format TM expects in
/// `x-tm-date` (e.g. "Mon, 11 Aug 2026 03:14:00 GMT").
///
/// Always emits C-locale English day/month abbreviations regardless of the
/// host's locale, which `strftime("%a, %d %b %Y %H:%M:%S GMT")` would not.
std::string formatHttpDate(time_t utcTime);

/// Parses an RFC 7231 HTTP-date back to a UTC time_t.
///
/// Accepts the preferred IMF-fixdate form that servers emit in `Date` and
/// `Last-Modified` (e.g. "Mon, 11 Aug 2026 03:14:00 GMT"). Returns false on
/// anything it does not recognise rather than guessing, since a misparsed date
/// would silently produce a bogus clock-skew reading.
bool parseHttpDate(const std::string &text, time_t &out);

} // namespace tmd
