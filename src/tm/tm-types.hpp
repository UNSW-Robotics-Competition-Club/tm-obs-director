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

#include <cstdint>
#include <string>
#include <vector>

namespace tmd {

/// Why a TM API call failed.
///
/// These are deliberately fine-grained. Every one of them surfaces as a 401 or a
/// connection error at the transport level, but each has a completely different
/// remedy, and the dock's Test Connection button has to tell the user which one
/// they are actually looking at. Collapsing these into "auth failed" is the
/// difference between a fixable problem and a lost afternoon at an event.
enum class TmError {
	None,

	/// Could not reach the host at all: wrong IP, wrong port, firewall, or TM
	/// simply is not running.
	HostUnreachable,

	/// The host answered but /api/* does not exist. Almost always means the
	/// Event Partner has not ticked Tools -> Options -> Web Publishing ->
	/// Enable Local TM API for this event.
	LocalApiDisabled,

	/// The OAuth server rejected the client id/secret. The application-level
	/// credentials from DWAB are wrong, revoked, or for a different season.
	InvalidClientCredentials,

	/// Could not reach the cloud OAuth host. Distinct from HostUnreachable:
	/// TM is on the LAN but auth.vextm.dwabtech.com needs the internet, so
	/// this is the failure a venue with no WAN uplink hits.
	AuthHostUnreachable,

	/// The request signature did not match. The TM API key is wrong — usually
	/// pasted from a different event, since the key is regenerated per event.
	InvalidSignature,

	/// x-tm-date is too far from the TM server's clock. Replay protection is
	/// rejecting us; fix the clock, not the credentials.
	ClockSkew,

	/// The access token expired and could not be refreshed.
	TokenExpired,

	/// Reached the API and authenticated, but it returned an unexpected status.
	HttpError,

	/// Authenticated and got a 2xx, but the body was not the JSON we expected.
	MalformedResponse,
};

const char *tmErrorName(TmError error);

/// Human-readable, actionable description. This is what the dock shows the user,
/// so it names the fix rather than restating the failure.
const char *tmErrorRemedy(TmError error);

/// TM's audience display screens, as accepted by {"cmd":"setAudienceDisplay"}.
/// Names and spellings come from vex-tm-client, the reference implementation.
enum class AudienceDisplay {
	Blank,
	Logo,
	Intro,
	InMatch,
	Results,
	Schedule,
	Rankings,
	ScRankings,
	AllianceSelection,
	Bracket,
	Award,
	Inspection,
};

/// Wire name for an audience display, e.g. AudienceDisplay::InMatch -> "IN_MATCH".
const char *audienceDisplayName(AudienceDisplay display);

/// Parses a wire name back to the enum. Returns false when unrecognised, which
/// matters because TM may add screens we do not know about yet.
bool parseAudienceDisplay(const std::string &name, AudienceDisplay &out);

struct Event {
	std::string name;
	std::string code; ///< RobotEvents SKU, e.g. "RE-V5RC-25-0000"
};

struct Division {
	int id = 0;
	std::string name;
};

struct FieldSet {
	int id = 0;
	std::string name;
};

struct Field {
	int id = 0;
	std::string name;
};

struct Team {
	std::string number;
	std::string name;
	std::string school;
	std::string city;
	std::string region;
	std::string country;
	std::string ageGroup;
	int divisionId = 0;
	bool checkedIn = false;
};

/// Identifies a match. TM's own tuple; carried verbatim so it can be matched
/// against websocket events, which use the same shape.
struct MatchTuple {
	int session = 0;
	int division = 0;
	std::string round; ///< "QUAL", "ELIM", "PRACTICE", ...
	int instance = 0;
	int match = 0;

	bool operator==(const MatchTuple &other) const
	{
		return session == other.session && division == other.division && round == other.round &&
		       instance == other.instance && match == other.match;
	}

	/// Short human label for logs and chapter titles, e.g. "QUAL 12".
	std::string label() const;
};

struct Match {
	MatchTuple tuple;
	std::string state; ///< "UNPLAYED", "SCORED", ...
	int64_t timeScheduled = 0;
	std::vector<std::string> redTeams;
	std::vector<std::string> blueTeams;
	int redScore = 0;
	int blueScore = 0;
	int winningAlliance = -1; ///< 0 red, 1 blue, -1 none/tie/unplayed

	bool isScored() const { return state == "SCORED"; }
};

} // namespace tmd
