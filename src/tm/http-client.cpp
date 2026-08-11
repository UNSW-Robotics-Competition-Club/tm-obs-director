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

#include "http-client.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <mutex>

namespace tmd {

namespace {

std::once_flag g_curlInit;

size_t writeBody(char *data, size_t size, size_t count, void *userdata)
{
	auto *out = static_cast<std::string *>(userdata);
	const size_t total = size * count;
	out->append(data, total);
	return total;
}

/// Extracts `header`'s value from a raw response line, if that is the header
/// this line carries. Comparison is case-insensitive: HTTP header names are.
bool matchHeader(const char *data, size_t total, const char *header, size_t headerLen, std::string &out)
{
	if (total <= headerLen)
		return false;

	std::string lowered(data, headerLen);
	std::transform(lowered.begin(), lowered.end(), lowered.begin(),
		       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	if (lowered != header)
		return false;

	std::string value(data + headerLen, total - headerLen);
	// Strip surrounding whitespace and the trailing CRLF.
	const auto first = value.find_first_not_of(" \t\r\n");
	const auto last = value.find_last_not_of(" \t\r\n");
	out = (first == std::string::npos) ? std::string() : value.substr(first, last - first + 1);
	return true;
}

size_t readHeader(char *data, size_t size, size_t count, void *userdata)
{
	auto *response = static_cast<HttpResponse *>(userdata);
	const size_t total = size * count;

	static constexpr char kLastModified[] = "last-modified:";
	static constexpr char kDate[] = "date:";

	if (!matchHeader(data, total, kLastModified, sizeof(kLastModified) - 1, response->lastModified))
		matchHeader(data, total, kDate, sizeof(kDate) - 1, response->date);

	return total;
}

/// Shared setup for both request shapes.
void applyCommonOptions(CURL *curl, const std::string &url, long timeoutMs, HttpResponse &response)
{
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeoutMs);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeoutMs);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, readHeader);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);

	// A redirect away from the address the user typed would invalidate the
	// signature, which is bound to the host and path. Fail instead.
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
}

/// Runs the request. Takes a reference rather than a value because libcurl holds
/// pointers into this object for its write and header callbacks; moving it would
/// leave those pointers aimed at a moved-from husk.
void perform(CURL *curl, HttpResponse &response)
{
	const CURLcode code = curl_easy_perform(curl);

	if (code == CURLE_OK) {
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
	} else {
		response.transportFailed = true;
		response.transportError = curl_easy_strerror(code);
	}
}

} // namespace

HttpClient::HttpClient()
{
	// curl_global_init is not thread-safe and must happen once per process.
	// OBS also uses libcurl, but repeat initialisation is reference-counted
	// and harmless; skipping it entirely would not be.
	std::call_once(g_curlInit, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

HttpClient::~HttpClient() = default;

HttpResponse HttpClient::get(const std::string &url, const HttpHeaders &headers, long timeoutMs)
{
	HttpResponse response;

	CURL *curl = curl_easy_init();
	if (!curl) {
		response.transportFailed = true;
		response.transportError = "could not initialise libcurl";
		return response;
	}

	curl_slist *list = nullptr;
	for (const auto &[name, value] : headers) {
		list = curl_slist_append(list, (name + ": " + value).c_str());
	}

	applyCommonOptions(curl, url, timeoutMs, response);
	curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
	if (list)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);

	perform(curl, response);

	curl_slist_free_all(list);
	curl_easy_cleanup(curl);
	return response;
}

HttpResponse HttpClient::postForm(const std::string &url, const std::string &formBody, long timeoutMs)
{
	HttpResponse response;

	CURL *curl = curl_easy_init();
	if (!curl) {
		response.transportFailed = true;
		response.transportError = "could not initialise libcurl";
		return response;
	}

	curl_slist *list = curl_slist_append(nullptr, "Content-Type: application/x-www-form-urlencoded");

	applyCommonOptions(curl, url, timeoutMs, response);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, formBody.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(formBody.size()));
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);

	perform(curl, response);

	curl_slist_free_all(list);
	curl_easy_cleanup(curl);
	return response;
}

} // namespace tmd
