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

#include "tm/tm-events.hpp"
#include "tm/tm-types.hpp"

#include <cstdint>
#include <string>

namespace tmd {

/// What is on the field, as far as we can tell.
///
/// TM does not send this. There is no "queued" flag and no "running" flag on the
/// wire — only the five events — so the state is inferred from the sequence, and
/// this reducer is the inference.
enum class MatchKind {
	None,    ///< Nothing queued.
	Match,   ///< A real match.
	Timeout, ///< A team or alliance timeout.
};

enum class QueueState {
	Unplayed, ///< Queued, timer not started.
	Running,  ///< Timer started.
	Stopped,  ///< Timer stopped. Covers a normal end and an abort alike.
};

const char *matchKindName(MatchKind kind);
const char *queueStateName(QueueState state);

/// Short label for a match tuple, e.g. "Q12", "QF 2-1", "Skills".
///
/// Deliberately the same vocabulary the Stream Deck plugin puts on a key, so an
/// operator reading one and the other is reading the same names for the same
/// matches.
std::string shortMatchName(const MatchTuple &tuple);

struct FieldsetState {
	/// Field TM last assigned, activated, or ran a match on. Zero when unknown.
	int activeField = 0;

	MatchKind kind = MatchKind::None;
	QueueState queue = QueueState::Unplayed;

	/// Short label for what is queued, empty when nothing is.
	std::string label;

	/// Set when kind == Match.
	MatchTuple match;

	/// Local wall clock, in milliseconds, when matchStarted was observed. Zero
	/// when not running. TM never sends a timer value, so anything resembling a
	/// countdown is computed from this and is an operator aid, not the official
	/// match clock.
	int64_t startedAtMs = 0;
};

/// True when this assignment is a timeout rather than a match.
///
/// TM reports a timeout either as an empty match object or as a tuple whose
/// round is TIMEOUT, depending on the build, so both shapes have to be accepted.
/// This is why the test is a shape test and not a type tag.
bool isTimeoutAssignment(const TmEvent &event);

/// Folds one event into the state.
///
/// `nowMs` is supplied rather than read so that tests are not at the mercy of a
/// real clock.
FieldsetState reduceFieldsetState(const FieldsetState &state, const TmEvent &event, int64_t nowMs);

} // namespace tmd
