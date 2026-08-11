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

#include "tm-types.hpp"

#include <array>
#include <cstddef>

namespace tmd {

const char *tmErrorName(TmError error)
{
	switch (error) {
	case TmError::None:
		return "None";
	case TmError::HostUnreachable:
		return "HostUnreachable";
	case TmError::LocalApiDisabled:
		return "LocalApiDisabled";
	case TmError::InvalidClientCredentials:
		return "InvalidClientCredentials";
	case TmError::AuthHostUnreachable:
		return "AuthHostUnreachable";
	case TmError::InvalidSignature:
		return "InvalidSignature";
	case TmError::ClockSkew:
		return "ClockSkew";
	case TmError::TokenExpired:
		return "TokenExpired";
	case TmError::HttpError:
		return "HttpError";
	case TmError::MalformedResponse:
		return "MalformedResponse";
	}
	return "Unknown";
}

const char *tmErrorRemedy(TmError error)
{
	switch (error) {
	case TmError::None:
		return "Connected.";
	case TmError::HostUnreachable:
		return "Could not reach Tournament Manager. Check the address and port, confirm TM is "
		       "running, and confirm this machine is on the same network.";
	case TmError::LocalApiDisabled:
		return "Tournament Manager answered but its API is switched off. In TM, open "
		       "Tools > Options > Web Publishing and tick Enable Local TM API. This has to be "
		       "done for each event.";
	case TmError::InvalidClientCredentials:
		return "The client ID or client secret was rejected. These come from DWAB and are tied "
		       "to a season, so last season's credentials will not work.";
	case TmError::AuthHostUnreachable:
		return "Could not reach auth.vextm.dwabtech.com. Tournament Manager is on the local "
		       "network but sign-in needs internet access. Check the venue's uplink.";
	case TmError::InvalidSignature:
		return "The TM API key was rejected. TM generates a new key for every event, so copy "
		       "the key from this event's Web Publishing options.";
	case TmError::ClockSkew:
		return "This machine's clock is too far from the Tournament Manager server's. Enable "
		       "automatic time synchronisation on both machines.";
	case TmError::TokenExpired:
		return "The access token expired and could not be renewed. Check internet access.";
	case TmError::HttpError:
		return "Tournament Manager returned an unexpected response.";
	case TmError::MalformedResponse:
		return "Tournament Manager returned a response this plugin could not read. It may be a "
		       "different TM version than this plugin supports.";
	}
	return "Unknown error.";
}

namespace {

// Index must line up with the AudienceDisplay enumerators.
constexpr std::array<const char *, 12> kAudienceDisplayNames = {
	"BLANK", "LOGO",      "INTRO",     "IN_MATCH",           "RESULTS", "SCHEDULE",
	"RANKINGS", "SC_RANKINGS", "ALLIANCE_SELECTION", "BRACKET", "AWARD", "INSPECTION",
};

} // namespace

const char *audienceDisplayName(AudienceDisplay display)
{
	const auto index = static_cast<size_t>(display);
	if (index >= kAudienceDisplayNames.size())
		return "BLANK";
	return kAudienceDisplayNames[index];
}

bool parseAudienceDisplay(const std::string &name, AudienceDisplay &out)
{
	for (size_t i = 0; i < kAudienceDisplayNames.size(); ++i) {
		if (name == kAudienceDisplayNames[i]) {
			out = static_cast<AudienceDisplay>(i);
			return true;
		}
	}
	return false;
}

std::string MatchTuple::label() const
{
	return round + " " + std::to_string(match);
}

} // namespace tmd
