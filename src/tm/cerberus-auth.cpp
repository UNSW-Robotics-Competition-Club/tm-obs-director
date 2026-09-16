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

#include "cerberus-auth.hpp"
#include "json-util.hpp"

#include <cctype>

namespace tmd {

namespace {

/// Maps a Cerberus wire error code onto a TmError.
///
/// The split that matters in a venue is between the three outcomes an operator
/// can act on differently: their build is wrong (CerberusKeyInvalid,
/// CerberusUpgradeRequired), our credential is wrong (InvalidClientCredentials),
/// or something upstream is down (AuthHostUnreachable). Collapsing these sends
/// an operator to debug whichever one is not actually broken.
TmError errorForCode(const std::string &code)
{
	if (code == "invalid_client")
		return TmError::CerberusKeyInvalid;
	if (code == "upgrade_required")
		return TmError::CerberusUpgradeRequired;
	if (code == "rate_limited")
		return TmError::CerberusRateLimited;
	if (code == "credentials_expired" || code == "credentials_invalid")
		return TmError::InvalidClientCredentials;
	if (code == "upstream_error")
		return TmError::AuthHostUnreachable;
	return TmError::HttpError;
}

bool isHex(const std::string &value, size_t from, size_t count)
{
	if (value.size() < from + count)
		return false;
	for (size_t i = from; i < from + count; ++i) {
		const unsigned char c = static_cast<unsigned char>(value[i]);
		const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
		if (!hex)
			return false;
	}
	return true;
}

std::string trimTrailingSlashes(std::string url)
{
	while (!url.empty() && url.back() == '/')
		url.pop_back();
	return url;
}

} // namespace

#ifndef TMD_CERBERUS_KEY
#define TMD_CERBERUS_KEY ""
#endif

#ifndef TMD_BUILD_VERSION
#define TMD_BUILD_VERSION "0.0.0"
#endif

CerberusConfig defaultCerberusConfig()
{
	CerberusConfig config;
	config.apiKey = TMD_CERBERUS_KEY;
	config.buildVersion = TMD_BUILD_VERSION;
	return config;
}

bool isValidCerberusKey(const std::string &key)
{
	// ctm_<16 hex>_<48 hex>. Lower case only, which is how Cerberus issues them.
	if (key.size() != 4 + 16 + 1 + 48)
		return false;
	if (key.compare(0, 4, "ctm_") != 0)
		return false;
	if (key[4 + 16] != '_')
		return false;
	return isHex(key, 4, 16) && isHex(key, 4 + 16 + 1, 48);
}

CerberusAuth::CerberusAuth(CerberusConfig config) : config_(std::move(config)) {}

void CerberusAuth::setConfig(CerberusConfig config)
{
	config_ = std::move(config);
	invalidate();
}

void CerberusAuth::setEvent(const std::string &code, const std::string &name, const std::string &sku)
{
	config_.eventCode = code;
	config_.eventName = name;
	config_.eventSku = sku;
}

void CerberusAuth::invalidate()
{
	token_.clear();
	expiresAt_ = 0;
}

TmError CerberusAuth::token(std::string &token, std::string &detail)
{
	if (!token_.empty() && expiresAt_ > nowSeconds() + config_.refreshMarginSeconds) {
		token = token_;
		return TmError::None;
	}

	const TmError error = mint(detail);
	if (error != TmError::None)
		return error;

	token = token_;
	return TmError::None;
}

TmError CerberusAuth::mint(std::string &detail)
{
	if (!isValidCerberusKey(config_.apiKey)) {
		// Locally detectable, so it is caught here rather than spent as a request
		// and returned as an indistinguishable 401.
		detail = config_.apiKey.empty() ? "no sign-in key was built into this plugin"
					        : "sign-in key is not in the expected ctm_ format";
		return TmError::CerberusKeyInvalid;
	}

	const std::string url = trimTrailingSlashes(config_.endpoint) + "/v1/token";

	std::string build = config_.buildVersion.empty() ? std::string("0.0.0") : config_.buildVersion;
	if (!config_.buildHash.empty())
		build += "/" + config_.buildHash;

	HttpHeaders headers;
	headers.emplace_back("Authorization", "Bearer " + config_.apiKey);
	headers.emplace_back("x-cerberus-build", build);
	if (!config_.eventCode.empty())
		headers.emplace_back("x-cerberus-event", config_.eventCode);
	if (!config_.eventName.empty())
		headers.emplace_back("x-cerberus-event-name", config_.eventName);
	if (!config_.eventSku.empty())
		headers.emplace_back("x-cerberus-event-sku", config_.eventSku);

	const HttpResponse response = http_.post(url, std::string(), headers, config_.timeoutMs);

	if (response.transportFailed) {
		detail = response.transportError;
		return TmError::CerberusUnreachable;
	}

	if (!response.ok()) {
		json document;
		std::string ignored;
		std::string code;
		if (parseJson(response.body, document, ignored) && document.is_object()) {
			code = stringField(document, "error");
			detail = stringField(document, "message");
		}
		if (detail.empty())
			detail = "sign-in returned " + std::to_string(response.status);
		return errorForCode(code);
	}

	json document;
	if (!parseJson(response.body, document, detail))
		return TmError::MalformedResponse;

	const std::string accessToken = stringField(document, "access_token");
	if (accessToken.empty()) {
		detail = "sign-in response had no access_token";
		return TmError::MalformedResponse;
	}

	// Cerberus reports the token's REMAINING life, not the value DWAB issued, so
	// this is an offset from now and not from when DWAB minted it.
	const int expiresIn = intField(document, "expires_in", 0);
	if (expiresIn <= 0) {
		detail = "sign-in response reported expires_in=" + std::to_string(expiresIn);
		return TmError::TokenExpired;
	}

	token_ = accessToken;
	expiresAt_ = nowSeconds() + expiresIn;
	return TmError::None;
}

} // namespace tmd
