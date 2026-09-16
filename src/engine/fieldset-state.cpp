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

#include "engine/fieldset-state.hpp"

#include <array>

namespace tmd {

namespace {

/// Rounds that need an instance number to be unambiguous: there is more than one
/// QF 1, but only ever one Q1.
bool isBracketRound(const std::string &round)
{
	static const std::array<const char *, 7> kBracket = {"QF", "SF", "F", "R16", "R32", "R64", "R128"};
	for (const char *name : kBracket) {
		if (round == name)
			return true;
	}
	return false;
}

const char *shortPrefix(const std::string &round)
{
	if (round == "PRACTICE")
		return "P";
	if (round == "QUAL")
		return "Q";
	if (round == "TOP_N")
		return "T";
	if (round == "ROUND_ROBIN")
		return "RR";
	return nullptr;
}

} // namespace

const char *matchKindName(MatchKind kind)
{
	switch (kind) {
	case MatchKind::None:
		return "None";
	case MatchKind::Match:
		return "Match";
	case MatchKind::Timeout:
		return "Timeout";
	}
	return "None";
}

const char *queueStateName(QueueState state)
{
	switch (state) {
	case QueueState::Unplayed:
		return "Unplayed";
	case QueueState::Running:
		return "Running";
	case QueueState::Stopped:
		return "Stopped";
	}
	return "Unplayed";
}

std::string shortMatchName(const MatchTuple &tuple)
{
	if (tuple.round == "SKILLS") {
		// TM reports a skills run without saying driver or programming. Guessing
		// would put the wrong word on screen, so it stays unqualified.
		return "Skills";
	}
	if (tuple.round == "TIMEOUT")
		return "Timeout";
	if (tuple.round.empty() || tuple.round == "NONE")
		return "-";

	if (const char *prefix = shortPrefix(tuple.round))
		return prefix + std::to_string(tuple.match);

	if (isBracketRound(tuple.round))
		return tuple.round + " " + std::to_string(tuple.instance) + "-" + std::to_string(tuple.match);

	return tuple.round + " " + std::to_string(tuple.match);
}

bool isTimeoutAssignment(const TmEvent &event)
{
	// Two shapes, both seen in the field: an assignment with no match object at
	// all, and one carrying a tuple whose round is TIMEOUT. Which one arrives
	// depends on the TM build, so neither can be treated as the only form.
	if (!event.hasMatch)
		return true;
	return event.match.round == "TIMEOUT";
}

FieldsetState reduceFieldsetState(const FieldsetState &state, const TmEvent &event, int64_t nowMs)
{
	FieldsetState next = state;

	switch (event.type) {
	case TmEventType::FieldMatchAssigned: {
		// No match and no field is TM saying the field set is now empty, which is
		// a different thing from a timeout being queued onto a field.
		if (!event.hasMatch && event.fieldId == 0) {
			next.kind = MatchKind::None;
			next.queue = QueueState::Unplayed;
			next.label.clear();
			next.match = MatchTuple{};
			next.startedAtMs = 0;
			break;
		}

		const bool timeout = isTimeoutAssignment(event);
		if (event.fieldId != 0)
			next.activeField = event.fieldId;
		next.kind = timeout ? MatchKind::Timeout : MatchKind::Match;
		next.label = timeout ? "Timeout" : shortMatchName(event.match);
		next.match = timeout ? MatchTuple{} : event.match;
		next.queue = QueueState::Unplayed;
		next.startedAtMs = 0;
		break;
	}

	case TmEventType::FieldActivated:
		if (event.fieldId != 0)
			next.activeField = event.fieldId;
		break;

	case TmEventType::MatchStarted:
		if (event.fieldId != 0)
			next.activeField = event.fieldId;
		// A start with nothing queued is TM running a timeout. There is no
		// separate event for that, so this inference is the only way to know.
		if (next.kind == MatchKind::None) {
			next.kind = MatchKind::Timeout;
			next.label = "Timeout";
		}
		next.queue = QueueState::Running;
		next.startedAtMs = nowMs;
		break;

	case TmEventType::MatchStopped:
		// Fires on an abort as well as on a normal end. TM sends no separate
		// abort event, so "stopped" is as much as can honestly be said here.
		next.queue = QueueState::Stopped;
		break;

	case TmEventType::AudienceDisplayChanged:
	case TmEventType::Unknown:
		break;
	}

	return next;
}

} // namespace tmd
