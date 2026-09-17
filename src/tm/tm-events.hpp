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

#pragma once

#include "tm-types.hpp"

#include <string>

namespace tmd {

/// Events TM pushes down the field set websocket.
///
/// Shapes are from the TM Public API Guide v1.2 and cross-checked against
/// vex-tm-client (out/Fieldset.js), the reference implementation.
enum class TmEventType {
	/// Not one of the documented types. Carried rather than dropped so a TM
	/// update that adds an event shows up in the log instead of vanishing.
	Unknown,

	/// A match was queued onto a field. Carries the match tuple.
	FieldMatchAssigned,

	/// A field became the active one.
	FieldActivated,

	/// The match timer started.
	MatchStarted,

	/// The match timer stopped. Fires on abort as well as on a normal end, so
	/// this alone does not mean "scored" — cross-check /api/matches state.
	MatchStopped,

	/// The audience display screen changed, whoever changed it.
	AudienceDisplayChanged,
};

/// Wire name, e.g. TmEventType::MatchStarted -> "matchStarted".
const char *tmEventTypeName(TmEventType type);

/// One decoded websocket message.
struct TmEvent {
	TmEventType type = TmEventType::Unknown;

	/// Which field the event concerns. Absent on audienceDisplayChanged, where
	/// it stays 0.
	int fieldId = 0;

	/// Set only on FieldMatchAssigned.
	bool hasMatch = false;
	MatchTuple match;

	/// Set only on AudienceDisplayChanged, and only when the screen name was one
	/// we recognise. An unrecognised name leaves this false rather than
	/// defaulting to Blank, which would look like a real instruction to blank
	/// the display.
	bool hasDisplay = false;
	AudienceDisplay display = AudienceDisplay::Blank;

	/// The `type` field exactly as it arrived. Populated for every event, and
	/// the only identification available when type == Unknown.
	std::string rawType;

	/// The whole message. Kept for logging and for rules that need a field we
	/// have not modelled yet.
	std::string raw;
};

/// Decodes one websocket text frame.
///
/// Returns false only when the payload is not a JSON object with a string
/// `type` — i.e. when it is not a TM event at all. An event whose type we do not
/// recognise still parses, with type == Unknown and rawType filled in.
bool parseTmEvent(const std::string &payload, TmEvent &out);

/// Builds an argument-free field set command: "start", "endEarly", "abort",
/// "reset", "queuePrevMatch", "queueNextMatch".
std::string buildCommand(const std::string &command);

/// Builds {"cmd":"setAudienceDisplay","display":"..."}.
std::string buildSetAudienceDisplayCommand(AudienceDisplay display);

/// Builds {"cmd":"queueSkills","skillsID":N}.
std::string buildQueueSkillsCommand(int skillsId);

} // namespace tmd
