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

#include "tm-api-client.hpp"
#include "tm-signer.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <ctime>

namespace tmd {

namespace {

using nlohmann::json;

/// Refresh this far before actual expiry, so a long call cannot straddle it.
constexpr int64_t kTokenRefreshMarginSeconds = 300;

int64_t nowSeconds()
{
	return static_cast<int64_t>(std::time(nullptr));
}

/// Percent-encodes a value for an application/x-www-form-urlencoded body.
/// Client secrets are opaque and may contain anything, so this cannot be skipped.
std::string formEncode(const std::string &value)
{
	static constexpr char kHex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(value.size());

	for (const unsigned char c : value) {
		const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
					(c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
		if (unreserved) {
			out.push_back(static_cast<char>(c));
		} else {
			out.push_back('%');
			out.push_back(kHex[c >> 4]);
			out.push_back(kHex[c & 0x0F]);
		}
	}
	return out;
}

/// Reads a JSON string field, tolerating absence and wrong types.
std::string stringField(const json &object, const char *key, const std::string &fallback = {})
{
	const auto it = object.find(key);
	if (it == object.end() || !it->is_string())
		return fallback;
	return it->get<std::string>();
}

int intField(const json &object, const char *key, int fallback = 0)
{
	const auto it = object.find(key);
	if (it == object.end() || !it->is_number_integer())
		return fallback;
	return it->get<int>();
}

bool boolField(const json &object, const char *key, bool fallback = false)
{
	const auto it = object.find(key);
	if (it == object.end() || !it->is_boolean())
		return fallback;
	return it->get<bool>();
}

/// Parses a response body, returning false (and setting `error`) on bad JSON.
bool parseJson(const std::string &body, json &out, std::string &detail)
{
	out = json::parse(body, nullptr, false);
	if (out.is_discarded()) {
		detail = "response was not valid JSON";
		return false;
	}
	return true;
}

/// Wraps a typed parse so every accessor shares the same failure handling.
template<typename T, typename Parser>
TmResult<T> parseInto(const TmRawResult &raw, Parser &&parser)
{
	TmResult<T> result;

	if (!raw.ok()) {
		result.error = raw.error;
		result.detail = raw.detail;
		return result;
	}

	json document;
	if (!parseJson(raw.body, document, result.detail)) {
		result.error = TmError::MalformedResponse;
		return result;
	}

	if (!parser(document, result.value)) {
		result.error = TmError::MalformedResponse;
		result.detail = "response JSON did not have the expected shape";
	}
	return result;
}

void parseTeamInto(const json &entry, Team &team)
{
	team.number = stringField(entry, "number");
	team.name = stringField(entry, "name");
	team.school = stringField(entry, "school");
	team.city = stringField(entry, "city");
	team.region = stringField(entry, "region");
	team.country = stringField(entry, "country");
	team.ageGroup = stringField(entry, "ageGroup");
	team.divisionId = intField(entry, "divId");
	team.checkedIn = boolField(entry, "checkedIn");
}

bool parseTeamsDocument(const json &document, std::vector<Team> &out)
{
	const auto teams = document.find("teams");
	if (teams == document.end() || !teams->is_array())
		return false;

	for (const auto &entry : *teams) {
		if (!entry.is_object())
			continue;
		Team team;
		parseTeamInto(entry, team);
		out.push_back(team);
	}
	return true;
}

void parseAllianceTeams(const json &alliance, std::vector<std::string> &out)
{
	const auto teams = alliance.find("teams");
	if (teams == alliance.end() || !teams->is_array())
		return;

	for (const auto &team : *teams) {
		if (team.is_object())
			out.push_back(stringField(team, "number"));
	}
}

} // namespace

TmApiClient::TmApiClient(TmConfig config) : config_(std::move(config)) {}

void TmApiClient::setConfig(TmConfig config)
{
	config_ = std::move(config);

	// Credentials may have changed, so the cached token is no longer trustworthy.
	token_.clear();
	tokenExpiresAt_ = 0;
	clockSkewSeconds_ = 0;
}

void TmApiClient::recordClockSkew(const HttpResponse &response)
{
	if (response.date.empty())
		return;

	time_t serverTime = 0;
	if (!parseHttpDate(response.date, serverTime))
		return;

	clockSkewSeconds_ = static_cast<int>(nowSeconds() - static_cast<int64_t>(serverTime));
}

TmError TmApiClient::ensureToken(std::string &detail)
{
	if (!token_.empty() && tokenExpiresAt_ > nowSeconds() + kTokenRefreshMarginSeconds)
		return TmError::None;

	if (config_.clientId.empty() || config_.clientSecret.empty()) {
		detail = "client id and secret are not configured";
		return TmError::InvalidClientCredentials;
	}

	const std::string body = "grant_type=client_credentials&client_id=" + formEncode(config_.clientId) +
				 "&client_secret=" + formEncode(config_.clientSecret);

	const HttpResponse response = http_.postForm(config_.authUrl, body);

	if (response.transportFailed) {
		// Distinct from the TM host being unreachable: this one is a cloud
		// endpoint, so it fails when the venue has no internet even though TM
		// itself is reachable on the LAN.
		detail = response.transportError;
		return TmError::AuthHostUnreachable;
	}

	if (response.status == 400 || response.status == 401 || response.status == 403) {
		detail = "OAuth endpoint returned " + std::to_string(response.status) + ": " + response.body;
		return TmError::InvalidClientCredentials;
	}

	if (!response.ok()) {
		detail = "OAuth endpoint returned " + std::to_string(response.status);
		return TmError::HttpError;
	}

	json document;
	if (!parseJson(response.body, document, detail))
		return TmError::MalformedResponse;

	const std::string accessToken = stringField(document, "access_token");
	if (accessToken.empty()) {
		detail = "OAuth response had no access_token";
		return TmError::MalformedResponse;
	}

	const int expiresIn = intField(document, "expires_in", 0);
	if (expiresIn <= 0) {
		detail = "OAuth response reported expires_in=" + std::to_string(expiresIn);
		return TmError::TokenExpired;
	}

	token_ = accessToken;
	tokenExpiresAt_ = nowSeconds() + expiresIn;
	return TmError::None;
}

TmError TmApiClient::acquireToken(std::string &token, std::string &detail)
{
	const TmError error = ensureToken(detail);
	if (error != TmError::None)
		return error;

	token = token_;
	return TmError::None;
}

TmError TmApiClient::classify(const HttpResponse &response, std::string &detail) const
{
	if (response.transportFailed) {
		detail = response.transportError;
		return TmError::HostUnreachable;
	}

	if (response.status == 404) {
		// The whole /api/* namespace is absent until the Event Partner enables
		// the Local TM API, so a 404 on a path we know exists means that, not a
		// missing resource. Callers asking for a genuinely absent resource (an
		// unknown division, say) get the same code -- the detail disambiguates.
		detail = "server returned 404";
		return TmError::LocalApiDisabled;
	}

	if (response.status == 401 || response.status == 403) {
		// Clock skew and a wrong API key both land here, and TM's error body is
		// not something we can rely on. The server's own Date header is, so use
		// the measured drift to tell them apart.
		if (std::abs(clockSkewSeconds_) > config_.maxClockSkewSeconds) {
			detail = "clock differs from TM server by " + std::to_string(clockSkewSeconds_) + "s";
			return TmError::ClockSkew;
		}
		detail = "server returned " + std::to_string(response.status) + ": " + response.body;
		return TmError::InvalidSignature;
	}

	if (!response.ok() && !response.notModified()) {
		detail = "server returned " + std::to_string(response.status);
		return TmError::HttpError;
	}

	return TmError::None;
}

TmRawResult TmApiClient::getRaw(const std::string &path, const std::string &ifModifiedSince)
{
	TmRawResult result;

	const TmError tokenError = ensureToken(result.detail);
	if (tokenError != TmError::None) {
		result.error = tokenError;
		return result;
	}

	const std::string url = config_.baseUrl + path;
	const std::string tmDate = formatHttpDate(static_cast<time_t>(nowSeconds()));
	const std::string signature = signRequest("GET", url, token_, tmDate, config_.apiKey);

	if (signature.empty()) {
		result.error = TmError::HttpError;
		result.detail = "could not sign request for " + url + " (is the address a valid URL?)";
		return result;
	}

	HttpHeaders headers = {
		{"Authorization", "Bearer " + token_},
		{"x-tm-date", tmDate},
		{"x-tm-signature", signature},
		{"Accept", "application/json"},
	};

	if (!ifModifiedSince.empty())
		headers.push_back({"If-Modified-Since", ifModifiedSince});

	const HttpResponse response = http_.get(url, headers);

	// Do this before classify(): the skew reading is what classify() uses to
	// distinguish a skewed clock from a bad API key.
	recordClockSkew(response);

	result.error = classify(response, result.detail);
	if (result.error != TmError::None)
		return result;

	result.notModified = response.notModified();
	result.lastModified = response.lastModified;
	if (!result.notModified)
		result.body = response.body;

	return result;
}

TmResult<Event> TmApiClient::getEvent()
{
	return parseInto<Event>(getRaw("/api/event"), [](const json &document, Event &out) {
		const auto event = document.find("event");
		if (event == document.end() || !event->is_object())
			return false;
		out.name = stringField(*event, "name");
		out.code = stringField(*event, "code");
		return true;
	});
}

TmResult<std::vector<Division>> TmApiClient::getDivisions()
{
	return parseInto<std::vector<Division>>(
		getRaw("/api/divisions"), [](const json &document, std::vector<Division> &out) {
			const auto divisions = document.find("divisions");
			if (divisions == document.end() || !divisions->is_array())
				return false;

			for (const auto &entry : *divisions) {
				if (!entry.is_object())
					continue;
				out.push_back({intField(entry, "id"), stringField(entry, "name")});
			}
			return true;
		});
}

TmResult<std::vector<FieldSet>> TmApiClient::getFieldSets()
{
	return parseInto<std::vector<FieldSet>>(
		getRaw("/api/fieldsets"), [](const json &document, std::vector<FieldSet> &out) {
			const auto fieldSets = document.find("fieldSets");
			if (fieldSets == document.end() || !fieldSets->is_array())
				return false;

			for (const auto &entry : *fieldSets) {
				if (!entry.is_object())
					continue;
				out.push_back({intField(entry, "id"), stringField(entry, "name")});
			}
			return true;
		});
}

TmResult<std::vector<Field>> TmApiClient::getFields(int fieldSetId)
{
	const std::string path = "/api/fieldsets/" + std::to_string(fieldSetId) + "/fields";

	return parseInto<std::vector<Field>>(getRaw(path), [](const json &document, std::vector<Field> &out) {
		const auto fields = document.find("fields");
		if (fields == document.end() || !fields->is_array())
			return false;

		for (const auto &entry : *fields) {
			if (!entry.is_object())
				continue;
			out.push_back({intField(entry, "id"), stringField(entry, "name")});
		}
		return true;
	});
}

TmResult<std::vector<Team>> TmApiClient::getTeams()
{
	return parseInto<std::vector<Team>>(getRaw("/api/teams"), parseTeamsDocument);
}

TmResult<std::vector<Team>> TmApiClient::getTeams(int divisionId)
{
	return parseInto<std::vector<Team>>(getRaw("/api/teams/" + std::to_string(divisionId)),
					    parseTeamsDocument);
}

TmResult<std::vector<Match>> TmApiClient::getMatches(int divisionId)
{
	const std::string path = "/api/matches/" + std::to_string(divisionId);

	return parseInto<std::vector<Match>>(getRaw(path), [](const json &document, std::vector<Match> &out) {
		const auto matches = document.find("matches");
		if (matches == document.end() || !matches->is_array())
			return false;

		for (const auto &entry : *matches) {
			if (!entry.is_object())
				continue;

			Match match;

			const auto info = entry.find("matchInfo");
			if (info != entry.end() && info->is_object()) {
				match.state = stringField(*info, "state");

				const auto scheduled = info->find("timeScheduled");
				if (scheduled != info->end() && scheduled->is_number())
					match.timeScheduled = scheduled->get<int64_t>();

				const auto tuple = info->find("matchTuple");
				if (tuple != info->end() && tuple->is_object()) {
					match.tuple.session = intField(*tuple, "session");
					match.tuple.division = intField(*tuple, "division");
					match.tuple.round = stringField(*tuple, "round");
					match.tuple.instance = intField(*tuple, "instance");
					match.tuple.match = intField(*tuple, "match");
				}

				const auto alliances = info->find("alliances");
				if (alliances != info->end() && alliances->is_array()) {
					if (alliances->size() > 0 && (*alliances)[0].is_object())
						parseAllianceTeams((*alliances)[0], match.redTeams);
					if (alliances->size() > 1 && (*alliances)[1].is_object())
						parseAllianceTeams((*alliances)[1], match.blueTeams);
				}
			}

			const auto score = entry.find("finalScore");
			if (score != entry.end() && score->is_array() && score->size() >= 2) {
				if ((*score)[0].is_number_integer())
					match.redScore = (*score)[0].get<int>();
				if ((*score)[1].is_number_integer())
					match.blueScore = (*score)[1].get<int>();
			}

			match.winningAlliance = intField(entry, "winningAlliance", -1);

			out.push_back(std::move(match));
		}
		return true;
	});
}

TmError TmApiClient::testConnection(std::string &detail)
{
	const TmError tokenError = ensureToken(detail);
	if (tokenError != TmError::None)
		return tokenError;

	// /api/event is the cheapest resource that exercises signing end to end.
	const TmResult<Event> event = getEvent();
	if (!event.ok()) {
		detail = event.detail;
		return event.error;
	}

	detail = "connected to \"" + event.value.name + "\" (" + event.value.code + ")";
	return TmError::None;
}

} // namespace tmd
