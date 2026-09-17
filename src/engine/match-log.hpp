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

#include "engine/fieldset-state.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace tmd {

/// One played match, as it will appear in the CSV.
///
/// Wall clock answers "when did Q12 run"; the offsets answer "where is Q12 in
/// the recording", which is the question anyone cutting a VOD or writing chapter
/// markers actually has. Both are kept because neither substitutes for the other
/// - a stream that dropped and restarted has a wall clock that still makes sense
/// and an offset that does not.
struct MatchLogRow {
	std::string label; ///< Short name, e.g. "Q12" or "QF 2-1".
	std::string round; ///< TM's round string, e.g. "QUAL".
	int division = 0;
	int instance = 0;
	int match = 0;
	int fieldId = 0;

	/// Local wall clock in milliseconds since the epoch. `endedAtMs` is zero when
	/// the match never stopped - OBS closed mid-match, say.
	int64_t startedAtMs = 0;
	int64_t endedAtMs = 0;

	/// Milliseconds into the stream and the recording when the match started.
	/// Negative when that output was not running, which is not an error: plenty
	/// of events record without streaming, or neither.
	int64_t streamOffsetMs = -1;
	int64_t recordOffsetMs = -1;
};

/// Pairs each match start with its stop.
///
/// Deliberately knows nothing about files or OBS, so the awkward parts - a match
/// that never stops, a timeout arriving mid-match - are testable without either.
class MatchLog {
public:
	/// A match started. Returns a row when a previous match was still open, which
	/// happens if TM moved on without a stop: the old match is closed out with no
	/// end time rather than silently dropped.
	///
	/// Timeouts and skills-less noise are not matches and open nothing, but they
	/// do close anything already open.
	std::optional<MatchLogRow> started(const FieldsetState &state, int fieldId, int64_t nowMs,
					   int64_t streamOffsetMs, int64_t recordOffsetMs);

	/// A match stopped. Returns the completed row, or nothing when no match was
	/// open - TM sends a stop for timeouts too.
	std::optional<MatchLogRow> stopped(int64_t nowMs);

	/// Hands back anything still open, for shutdown or a dropped connection.
	std::optional<MatchLogRow> close();

	bool hasOpenMatch() const { return open_; }

private:
	bool open_ = false;
	MatchLogRow current_;
};

/// The header line, without a trailing newline.
std::string csvHeader();

/// One row, without a trailing newline.
std::string csvRow(const MatchLogRow &row);

/// Appends `row` to `path`, writing the header first if the file is new or
/// empty. Returns false and sets `error` when the file cannot be written -
/// a full disk or a path on a USB stick that was pulled out mid-event.
bool appendMatchCsv(const std::string &path, const MatchLogRow &row, std::string &error);

} // namespace tmd
