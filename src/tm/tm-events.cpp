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

#include "tm-events.hpp"

#include <nlohmann/json.hpp>

namespace tmd {

namespace {

using nlohmann::json;

int intField(const json &object, const char *key, int fallback = 0)
{
	const auto it = object.find(key);
	if (it == object.end() || !it->is_number_integer())
		return fallback;
	return it->get<int>();
}

std::string stringField(const json &object, const char *key)
{
	const auto it = object.find(key);
	if (it == object.end() || !it->is_string())
		return {};
	return it->get<std::string>();
}

} // namespace

const char *tmEventTypeName(TmEventType type)
{
	switch (type) {
	case TmEventType::FieldMatchAssigned:
		return "fieldMatchAssigned";
	case TmEventType::FieldActivated:
		return "fieldActivated";
	case TmEventType::MatchStarted:
		return "matchStarted";
	case TmEventType::MatchStopped:
		return "matchStopped";
	case TmEventType::AudienceDisplayChanged:
		return "audienceDisplayChanged";
	case TmEventType::Unknown:
		break;
	}
	return "unknown";
}

bool parseTmEvent(const std::string &payload, TmEvent &out)
{
	const json document = json::parse(payload, nullptr, false);
	if (document.is_discarded() || !document.is_object())
		return false;

	const std::string type = stringField(document, "type");
	if (type.empty())
		return false;

	out = TmEvent{};
	out.rawType = type;
	out.raw = payload;

	// Note the capitalisation: TM spells it fieldID, not fieldId.
	out.fieldId = intField(document, "fieldID");

	if (type == tmEventTypeName(TmEventType::FieldMatchAssigned)) {
		out.type = TmEventType::FieldMatchAssigned;

		const auto match = document.find("match");
		if (match != document.end() && match->is_object()) {
			out.hasMatch = true;
			out.match.session = intField(*match, "session");
			out.match.division = intField(*match, "division");
			// If real TM ever sends a numeric round here we will find out at
			// the Phase 5 bench test; the guide and vex-tm-client both show a
			// string, and guessing at a numeric mapping now would be inventing
			// an encoding we have never seen.
			out.match.round = stringField(*match, "round");
			out.match.instance = intField(*match, "instance");
			out.match.match = intField(*match, "match");
		}
		return true;
	}

	if (type == tmEventTypeName(TmEventType::FieldActivated)) {
		out.type = TmEventType::FieldActivated;
		return true;
	}

	if (type == tmEventTypeName(TmEventType::MatchStarted)) {
		out.type = TmEventType::MatchStarted;
		return true;
	}

	if (type == tmEventTypeName(TmEventType::MatchStopped)) {
		out.type = TmEventType::MatchStopped;
		return true;
	}

	if (type == tmEventTypeName(TmEventType::AudienceDisplayChanged)) {
		out.type = TmEventType::AudienceDisplayChanged;

		AudienceDisplay display{};
		if (parseAudienceDisplay(stringField(document, "display"), display)) {
			out.hasDisplay = true;
			out.display = display;
		}
		return true;
	}

	out.type = TmEventType::Unknown;
	return true;
}

std::string buildCommand(const std::string &command)
{
	return json{{"cmd", command}}.dump();
}

std::string buildSetAudienceDisplayCommand(AudienceDisplay display)
{
	return json{{"cmd", "setAudienceDisplay"}, {"display", audienceDisplayName(display)}}.dump();
}

std::string buildQueueSkillsCommand(int skillsId)
{
	return json{{"cmd", "queueSkills"}, {"skillsID", skillsId}}.dump();
}

} // namespace tmd
