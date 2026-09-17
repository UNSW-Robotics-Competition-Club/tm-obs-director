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

#include "http-client.hpp"
#include "tm-types.hpp"

#include <cstdint>
#include <string>

namespace tmd {

/// Everything needed to mint a TM bearer token through Cerberus.
///
/// Cerberus (VEX-TM-Cerberus) holds SRA's DWAB client id and secret and hands
/// out short-lived bearer tokens in exchange for a client key. That is the whole
/// reason this plugin does not carry a DWAB secret: a secret baked into a build
/// that ships to venue laptops cannot be rotated without recalling the build.
///
/// The TM Client API Key is unaffected and stays in TmConfig. Cerberus supplies
/// only the bearer; the plugin still signs each request itself, and the two
/// secrets meet nowhere except inside the signature.
struct CerberusConfig {
	/// Base URL, no trailing slash. The token endpoint is `{endpoint}/v1/token`.
	std::string endpoint = "https://tm.unswrobotics.com";

	/// Client key, `ctm_<16 hex>_<48 hex>`. Baked in at build time via
	/// TMD_CERBERUS_KEY rather than typed by the operator, because a key is
	/// issued per build, not per laptop.
	std::string apiKey;

	/// Sent as x-cerberus-build. Cerberus enforces a minimum version and answers
	/// `upgrade_required` below it, so this is not decoration.
	std::string buildVersion;
	std::string buildHash;

	/// Unverified attribution from TM's /api/event, forwarded so Cerberus's
	/// audit log can say which event a token was minted for. Optional.
	std::string eventCode;
	std::string eventName;
	std::string eventSku;

	/// Refetch when less than this remains. Matches the reference client.
	int refreshMarginSeconds = 300;

	long timeoutMs = 10000;
};

/// The config this build ships with: SRA's endpoint, the key compiled in via
/// TMD_CERBERUS_KEY, and this build's version. Start from this and override only
/// what a test or an operator needs to change.
CerberusConfig defaultCerberusConfig();

/// True when `key` has the shape Cerberus issues. Checked before any request so
/// a mistyped key fails instantly and locally rather than as a 401 at a venue.
bool isValidCerberusKey(const std::string &key);

/// Caches a bearer token obtained from Cerberus.
///
/// Caching here is not redundant with Cerberus's own caching. Cerberus exists so
/// that DWAB is called once an hour rather than once a request; a client that
/// asked Cerberus every time would just move the stampede one hop closer.
///
/// Not thread-safe. The field set socket calls this from its own thread through
/// TmApiClient, so the caller serialises access.
class CerberusAuth {
public:
	explicit CerberusAuth(CerberusConfig config);

	const CerberusConfig &config() const { return config_; }

	/// Replacing the config drops any cached token: a new endpoint or key has no
	/// business reusing a token minted under the old one.
	void setConfig(CerberusConfig config);

	/// Attributes subsequent mints to an event. Does not invalidate the token.
	void setEvent(const std::string &code, const std::string &name, const std::string &sku);

	/// Hands back a cached token, or mints one. `detail` carries Cerberus's own
	/// message on failure, which is written for an operator to read.
	TmError token(std::string &token, std::string &detail);

	/// Drops the cached token. Call this when TM answers 401, since TM uses the
	/// same status for a stale bearer and a wrong API key.
	void invalidate();

	/// Absolute expiry of the cached token, in seconds since the epoch. Zero when
	/// nothing is cached. Exposed so the dock can show how long is left.
	int64_t expiresAt() const { return expiresAt_; }

private:
	TmError mint(std::string &detail);

	CerberusConfig config_;
	HttpClient http_;

	std::string token_;
	int64_t expiresAt_ = 0;
};

} // namespace tmd
