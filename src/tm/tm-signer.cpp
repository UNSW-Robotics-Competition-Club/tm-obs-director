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

#include "tm-signer.hpp"

#include <mbedtls/md.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace tmd {

namespace {

constexpr const char *kSchemeSeparator = "://";
constexpr int kDefaultHttpPort = 80;
constexpr int kDefaultHttpsPort = 443;
constexpr size_t kSha256DigestBytes = 32;

const std::array<const char *, 7> kDayNames = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const std::array<const char *, 12> kMonthNames = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
						  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

std::string toLowerAscii(const std::string &value)
{
	std::string lowered = value;
	std::transform(lowered.begin(), lowered.end(), lowered.begin(),
		       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	return lowered;
}

/// Strips the port from `authority` when it matches the scheme's default,
/// matching WHATWG URL's `host` getter.
std::string normaliseAuthority(const std::string &authority, const std::string &scheme)
{
	const size_t colon = authority.rfind(':');
	if (colon == std::string::npos)
		return authority;

	// An unbracketed IPv6 literal would confuse this, but TM is reached by
	// IPv4 address or hostname in every documented deployment.
	const std::string portText = authority.substr(colon + 1);
	if (portText.empty() ||
	    !std::all_of(portText.begin(), portText.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }))
		return authority;

	const int port = std::atoi(portText.c_str());
	const bool isDefault = (scheme == "http" && port == kDefaultHttpPort) ||
			       (scheme == "https" && port == kDefaultHttpsPort);

	return isDefault ? authority.substr(0, colon) : authority;
}

} // namespace

bool parseSignableUrl(const std::string &url, SignableUrl &out)
{
	const size_t schemeEnd = url.find(kSchemeSeparator);
	if (schemeEnd == std::string::npos)
		return false;

	const std::string scheme = toLowerAscii(url.substr(0, schemeEnd));
	if (scheme != "http" && scheme != "https" && scheme != "ws" && scheme != "wss")
		return false;

	const size_t authorityStart = schemeEnd + std::string(kSchemeSeparator).length();

	// The authority ends at the first '/', '?' or '#'.
	size_t authorityEnd = url.length();
	for (size_t i = authorityStart; i < url.length(); ++i) {
		const char ch = url[i];
		if (ch == '/' || ch == '?' || ch == '#') {
			authorityEnd = i;
			break;
		}
	}

	const std::string authority = url.substr(authorityStart, authorityEnd - authorityStart);
	if (authority.empty())
		return false;

	// ws/wss share http/https default ports for this purpose; TM only ever
	// signs the http form of the URL, but be permissive on input.
	const std::string portScheme = (scheme == "ws") ? "http" : (scheme == "wss") ? "https" : scheme;
	out.host = normaliseAuthority(authority, portScheme);

	std::string remainder = url.substr(authorityEnd);

	// A fragment is not sent to the server and is not signed.
	const size_t fragment = remainder.find('#');
	if (fragment != std::string::npos)
		remainder = remainder.substr(0, fragment);

	// An absent or empty path is "/", per WHATWG URL.
	out.pathAndQuery = remainder.empty() ? "/" : remainder;
	if (out.pathAndQuery[0] == '?')
		out.pathAndQuery = "/" + out.pathAndQuery;

	return true;
}

std::string buildStringToSign(const std::string &method, const SignableUrl &url, const std::string &bearerToken,
			      const std::string &tmDate)
{
	return method + "\n" + url.pathAndQuery + "\n" + "token:" + bearerToken + "\n" + "host:" + url.host + "\n" +
	       "x-tm-date:" + tmDate + "\n";
}

std::string hmacSha256Hex(const std::string &key, const std::string &message)
{
	const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	if (info == nullptr)
		return {};

	unsigned char digest[kSha256DigestBytes] = {0};
	const int result = mbedtls_md_hmac(info, reinterpret_cast<const unsigned char *>(key.data()), key.length(),
					   reinterpret_cast<const unsigned char *>(message.data()), message.length(),
					   digest);
	if (result != 0)
		return {};

	std::string hex;
	hex.reserve(kSha256DigestBytes * 2);
	for (size_t i = 0; i < kSha256DigestBytes; ++i) {
		char pair[3] = {0};
		std::snprintf(pair, sizeof(pair), "%02x", digest[i]);
		hex.append(pair);
	}
	return hex;
}

std::string signRequest(const std::string &method, const std::string &url, const std::string &bearerToken,
			const std::string &tmDate, const std::string &apiKey)
{
	SignableUrl parsed;
	if (!parseSignableUrl(url, parsed))
		return {};

	return hmacSha256Hex(apiKey, buildStringToSign(method, parsed, bearerToken, tmDate));
}

std::string formatHttpDate(time_t utcTime)
{
	struct tm parts = {};
#ifdef _WIN32
	if (gmtime_s(&parts, &utcTime) != 0)
		return {};
#else
	if (gmtime_r(&utcTime, &parts) == nullptr)
		return {};
#endif

	if (parts.tm_wday < 0 || parts.tm_wday > 6 || parts.tm_mon < 0 || parts.tm_mon > 11)
		return {};

	char buffer[40] = {0};
	std::snprintf(buffer, sizeof(buffer), "%s, %02d %s %04d %02d:%02d:%02d GMT",
		      kDayNames[static_cast<size_t>(parts.tm_wday)], parts.tm_mday,
		      kMonthNames[static_cast<size_t>(parts.tm_mon)], parts.tm_year + 1900, parts.tm_hour, parts.tm_min,
		      parts.tm_sec);

	return std::string(buffer);
}

bool parseHttpDate(const std::string &text, time_t &out)
{
	// IMF-fixdate: "Mon, 11 Aug 2026 03:14:00 GMT". The weekday is ignored --
	// it is redundant with the date, and trusting it would let a server with a
	// wrong weekday break skew detection for no benefit.
	char monthName[4] = {0};
	int day = 0, year = 0, hour = 0, minute = 0, second = 0;

	// Skip the weekday and its comma if present.
	const auto comma = text.find(',');
	const std::string rest = (comma == std::string::npos) ? text : text.substr(comma + 1);

	if (std::sscanf(rest.c_str(), " %d %3s %d %d:%d:%d", &day, monthName, &year, &hour, &minute, &second) != 6)
		return false;

	int month = -1;
	for (size_t i = 0; i < kMonthNames.size(); ++i) {
		if (std::strcmp(monthName, kMonthNames[i]) == 0) {
			month = static_cast<int>(i);
			break;
		}
	}
	if (month < 0)
		return false;

	if (day < 1 || day > 31 || year < 1970 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
	    second > 60)
		return false;

	struct tm parts = {};
	parts.tm_mday = day;
	parts.tm_mon = month;
	parts.tm_year = year - 1900;
	parts.tm_hour = hour;
	parts.tm_min = minute;
	parts.tm_sec = second;
	parts.tm_isdst = 0;

	// The value is UTC, so it must not go through mktime, which would apply the
	// host's local timezone offset.
#ifdef _WIN32
	const time_t result = _mkgmtime(&parts);
#else
	const time_t result = timegm(&parts);
#endif

	if (result == static_cast<time_t>(-1))
		return false;

	out = result;
	return true;
}

} // namespace tmd
