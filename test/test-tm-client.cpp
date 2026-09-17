/*
tm-obs-director — VEX Tournament Manager automation for OBS Studio
Copyright (C) 2026 UNSW Robotics Competition Club <kevin.li@sydneyrobotics.com.au>

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

// Integration test for TmApiClient against test/mock-tm-server.mjs.
//
// This is the test that proves our C++ signer interoperates: the mock rebuilds
// the canonical string independently, in JavaScript, and rejects us if we get it
// wrong. The unit tests pin the algorithm; this proves it works over the wire.
//
// Launched by test/run-client-test.mjs, which supplies the base URL and the
// scenario. Not meant to be run by hand.

#include "src/tm/tm-api-client.hpp"

#include <cstring>
#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void check(const std::string &name, bool condition, const std::string &detail = {})
{
	if (condition) {
		std::cout << "  ok   " << name << "\n";
	} else {
		++g_failures;
		std::cout << "  FAIL " << name;
		if (!detail.empty())
			std::cout << "\n         " << detail;
		std::cout << "\n";
	}
}

void checkError(const std::string &name, tmd::TmError actual, tmd::TmError expected, const std::string &detail)
{
	check(name, actual == expected,
	      std::string("expected ") + tmd::tmErrorName(expected) + ", got " + tmd::tmErrorName(actual) +
		      (detail.empty() ? "" : " (" + detail + ")"));
}

/// Matches CERBERUS_KEY in test/mock-tm-server.mjs.
const std::string kCerberusKey = "ctm_0123456789abcdef_0123456789abcdef0123456789abcdef0123456789abcdef";

tmd::TmConfig configFor(const std::string &base)
{
	tmd::TmConfig config;
	config.baseUrl = base;
	config.apiKey = "TESTAPIKEY0123456789";
	config.cerberus.endpoint = base;
	config.cerberus.apiKey = kCerberusKey;
	config.cerberus.buildVersion = "0.1.0";
	return config;
}

// --- scenario: ok ----------------------------------------------------------

void testHappyPath(const std::string &base)
{
	std::cout << "happy path\n";

	tmd::TmApiClient client(configFor(base));

	std::string detail;
	checkError("testConnection succeeds", client.testConnection(detail), tmd::TmError::None, detail);

	const auto event = client.getEvent();
	check("event name parsed", event.ok() && event.value.name == "UNSW Mock Scrimmage", event.detail);
	check("event code parsed", event.ok() && event.value.code == "RE-V5RC-25-0000", event.detail);

	const auto divisions = client.getDivisions();
	check("two divisions", divisions.ok() && divisions.value.size() == 2, divisions.detail);
	check("division id and name",
	      divisions.ok() && divisions.value[0].id == 1 && divisions.value[0].name == "Division 1");

	const auto fieldSets = client.getFieldSets();
	check("two field sets", fieldSets.ok() && fieldSets.value.size() == 2, fieldSets.detail);

	const auto fields = client.getFields(1);
	check("two fields in field set 1", fields.ok() && fields.value.size() == 2, fields.detail);
	check("field name parsed", fields.ok() && fields.value[1].name == "Field 2");

	const auto teams = client.getTeams();
	check("four teams", teams.ok() && teams.value.size() == 4, teams.detail);
	check("team fields parsed", teams.ok() && teams.value[0].number == "1234A" && teams.value[0].checkedIn &&
					    teams.value[0].divisionId == 1);
	check("checkedIn false round-trips", teams.ok() && !teams.value[2].checkedIn);

	const auto matches = client.getMatches(1);
	check("three matches", matches.ok() && matches.value.size() == 3, matches.detail);

	if (matches.ok() && matches.value.size() == 3) {
		const tmd::Match &first = matches.value[0];
		check("match tuple round", first.tuple.round == "QUAL");
		check("match tuple number", first.tuple.match == 1);
		check("match label", first.tuple.label() == "QUAL 1", first.tuple.label());
		check("red alliance teams", first.redTeams.size() == 2 && first.redTeams[0] == "1234A");
		check("blue alliance teams", first.blueTeams.size() == 2 && first.blueTeams[1] == "5678D");
		check("final score", first.redScore == 42 && first.blueScore == 31);
		check("winning alliance red", first.winningAlliance == 0);
		check("scored state", first.isScored());

		const tmd::Match &second = matches.value[1];
		check("blue win parsed", second.winningAlliance == 1 && second.blueScore == 88);

		const tmd::Match &third = matches.value[2];
		check("unplayed match not scored", !third.isScored() && third.state == "UNPLAYED");
	}

	// Conditional GET. A client that ignores Last-Modified would poll TM harder
	// than the API guide allows, so this is behaviour worth pinning.
	const auto fresh = client.getRaw("/api/event");
	check("first GET returns a body", fresh.ok() && !fresh.notModified && !fresh.body.empty());
	check("Last-Modified surfaced", !fresh.lastModified.empty());

	const auto cached = client.getRaw("/api/event", fresh.lastModified);
	check("If-Modified-Since yields 304", cached.ok() && cached.notModified, cached.detail);
	check("304 carries no body", cached.body.empty());

	// The token is cached, so a second call must not re-hit the OAuth endpoint.
	// run-client-test.mjs asserts the request count; here we just confirm reuse
	// does not break anything.
	const auto again = client.getEvent();
	check("second call reuses cached token", again.ok(), again.detail);

	check("no clock skew against a local mock", std::abs(client.lastObservedClockSkewSeconds()) <= 2,
	      std::to_string(client.lastObservedClockSkewSeconds()) + "s");
}

// --- failure scenarios -----------------------------------------------------

void testBadApiKey(const std::string &base)
{
	std::cout << "wrong TM API key\n";

	tmd::TmConfig config = configFor(base);
	config.apiKey = "WRONG-KEY-FROM-LAST-EVENT";

	tmd::TmApiClient client(config);

	std::string detail;
	checkError("reported as InvalidSignature", client.testConnection(detail), tmd::TmError::InvalidSignature,
		   detail);
}

void testRevokedCerberusKey(const std::string &base)
{
	std::cout << "sign-in key rejected\n";

	tmd::TmConfig config = configFor(base);
	// Well formed, so it survives the local check and is actually sent.
	config.cerberus.apiKey = "ctm_" + std::string(16, '0') + "_" + std::string(48, '0');

	tmd::TmApiClient client(config);

	std::string detail;
	checkError("reported as CerberusKeyInvalid", client.testConnection(detail), tmd::TmError::CerberusKeyInvalid,
		   detail);
}

void testMissingCerberusKey(const std::string &base)
{
	std::cout << "no sign-in key in this build\n";

	tmd::TmConfig config = configFor(base);
	config.cerberus.apiKey.clear();

	tmd::TmApiClient client(config);

	std::string detail;
	checkError("caught before any request", client.testConnection(detail), tmd::TmError::CerberusKeyInvalid,
		   detail);
	check("says the build carries no key", detail.find("built into") != std::string::npos, detail);
}

void testMalformedCerberusKey(const std::string &base)
{
	std::cout << "malformed sign-in key\n";

	tmd::TmConfig config = configFor(base);
	config.cerberus.apiKey = "ctm_not-hex";

	tmd::TmApiClient client(config);

	std::string detail;
	checkError("caught before any request", client.testConnection(detail), tmd::TmError::CerberusKeyInvalid,
		   detail);
	check("says the format is wrong", detail.find("format") != std::string::npos, detail);
}

void testUpgradeRequired(const std::string &base)
{
	std::cout << "build below the accepted minimum\n";

	tmd::TmApiClient client(configFor(base));

	std::string detail;
	checkError("reported as CerberusUpgradeRequired", client.testConnection(detail),
		   tmd::TmError::CerberusUpgradeRequired, detail);
}

void testRateLimited(const std::string &base)
{
	std::cout << "sign-in rate limited\n";

	tmd::TmApiClient client(configFor(base));

	std::string detail;
	checkError("reported as CerberusRateLimited", client.testConnection(detail), tmd::TmError::CerberusRateLimited,
		   detail);
}

void testApiDisabled(const std::string &base)
{
	std::cout << "Local TM API not enabled\n";

	tmd::TmApiClient client(configFor(base));

	std::string detail;
	checkError("reported as LocalApiDisabled", client.testConnection(detail), tmd::TmError::LocalApiDisabled,
		   detail);
}

void testHostUnreachable()
{
	std::cout << "host unreachable\n";

	// Port 1 on loopback: nothing listens there, and it fails fast.
	tmd::TmApiClient client(configFor("http://127.0.0.1:1"));

	std::string detail;
	// Sign-in happens first, so an entirely dead address surfaces as
	// CerberusUnreachable. That is the honest answer: we never got far enough to
	// learn anything about TM itself.
	checkError("reported as CerberusUnreachable", client.testConnection(detail), tmd::TmError::CerberusUnreachable,
		   detail);
}

void testClockSkew(const std::string &base)
{
	std::cout << "clock skew\n";

	tmd::TmApiClient client(configFor(base));

	std::string detail;
	// Both a skewed clock and a wrong API key surface as a 401. Getting this
	// wrong sends the user off rotating a perfectly good API key while the real
	// problem is the machine's time, so it is worth pinning.
	checkError("reported as ClockSkew, not InvalidSignature", client.testConnection(detail),
		   tmd::TmError::ClockSkew, detail);

	const int skew = client.lastObservedClockSkewSeconds();
	check("measured skew is about an hour", skew < -3000 && skew > -4200, std::to_string(skew) + "s");
}

void testOurCredentialRejected(const std::string &base)
{
	std::cout << "The club's own credential rejected upstream\n";

	tmd::TmApiClient client(configFor(base));

	std::string detail;
	// Not CerberusKeyInvalid: the operator's build is fine, ours is not, and
	// telling them otherwise sends them off to debug the one thing that works.
	checkError("reported as InvalidClientCredentials", client.testConnection(detail),
		   tmd::TmError::InvalidClientCredentials, detail);
}

void testUpstreamDown(const std::string &base)
{
	std::cout << "sign-in could not reach DWAB\n";

	tmd::TmApiClient client(configFor(base));

	std::string detail;
	checkError("reported as AuthHostUnreachable", client.testConnection(detail), tmd::TmError::AuthHostUnreachable,
		   detail);
}

void testErrorMessages()
{
	std::cout << "error reporting\n";

	// Every error needs a name and a remedy: the dock shows the remedy, and a
	// missing one would leave the user staring at a blank explanation.
	const tmd::TmError all[] = {
		tmd::TmError::None,
		tmd::TmError::HostUnreachable,
		tmd::TmError::LocalApiDisabled,
		tmd::TmError::InvalidClientCredentials,
		tmd::TmError::AuthHostUnreachable,
		tmd::TmError::InvalidSignature,
		tmd::TmError::ClockSkew,
		tmd::TmError::TokenExpired,
		tmd::TmError::HttpError,
		tmd::TmError::MalformedResponse,
	};

	bool allNamed = true;
	bool allExplained = true;
	for (const tmd::TmError error : all) {
		if (std::strcmp(tmd::tmErrorName(error), "Unknown") == 0)
			allNamed = false;
		if (std::strlen(tmd::tmErrorRemedy(error)) < 10)
			allExplained = false;
	}

	check("every error has a name", allNamed);
	check("every error has a remedy", allExplained);

	// The audience display enum is the vocabulary for Phase 5 actions, so a
	// mismatch between our names and TM's would break scene automation silently.
	check("audience display round-trips", [] {
		for (int i = 0; i < 12; ++i) {
			const auto display = static_cast<tmd::AudienceDisplay>(i);
			tmd::AudienceDisplay parsed{};
			if (!tmd::parseAudienceDisplay(tmd::audienceDisplayName(display), parsed))
				return false;
			if (parsed != display)
				return false;
		}
		return true;
	}());

	check("IN_MATCH spelled as TM spells it",
	      std::strcmp(tmd::audienceDisplayName(tmd::AudienceDisplay::InMatch), "IN_MATCH") == 0);

	tmd::AudienceDisplay ignored{};
	check("unknown display name rejected", !tmd::parseAudienceDisplay("NOT_A_SCREEN", ignored));
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 3) {
		std::cerr << "usage: test-tm-client <scenario> <base-url>\n";
		return 2;
	}

	const std::string scenario = argv[1];
	const std::string base = argv[2];

	if (scenario == "ok") {
		testHappyPath(base);
		testErrorMessages();
	} else if (scenario == "bad-key") {
		testBadApiKey(base);
	} else if (scenario == "revoked-key") {
		testRevokedCerberusKey(base);
	} else if (scenario == "no-key") {
		testMissingCerberusKey(base);
	} else if (scenario == "malformed-key") {
		testMalformedCerberusKey(base);
	} else if (scenario == "upgrade-required") {
		testUpgradeRequired(base);
	} else if (scenario == "rate-limited") {
		testRateLimited(base);
	} else if (scenario == "credentials-invalid") {
		testOurCredentialRejected(base);
	} else if (scenario == "upstream-down") {
		testUpstreamDown(base);
	} else if (scenario == "api-disabled") {
		testApiDisabled(base);
	} else if (scenario == "clock-skew") {
		testClockSkew(base);
	} else if (scenario == "unreachable") {
		testHostUnreachable();
	} else {
		std::cerr << "unknown scenario: " << scenario << "\n";
		return 2;
	}

	if (g_failures == 0) {
		std::cout << "  (" << scenario << " passed)\n";
		return 0;
	}

	std::cout << "  " << g_failures << " FAILED in " << scenario << "\n";
	return 1;
}
