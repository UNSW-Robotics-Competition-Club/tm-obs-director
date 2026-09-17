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

#include "engine/match-log.hpp"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace tmd {

namespace {

/// Local time as YYYY-MM-DD HH:MM:SS. Local rather than UTC on purpose: every
/// other record of an event - the schedule, the score sheets, the person
/// remembering when lunch was - is in venue time.
std::string localTimestamp(int64_t epochMs)
{
	if (epochMs <= 0)
		return {};

	const std::time_t seconds = static_cast<std::time_t>(epochMs / 1000);
	std::tm parts{};
#ifdef _WIN32
	localtime_s(&parts, &seconds);
#else
	localtime_r(&seconds, &parts);
#endif

	char buffer[32];
	if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts) == 0)
		return {};
	return buffer;
}

/// hh:mm:ss, which is what a chapter list wants pasted into it. Empty for a
/// negative offset, meaning that output was not running.
std::string clockOffset(int64_t offsetMs)
{
	if (offsetMs < 0)
		return {};

	const int64_t total = offsetMs / 1000;
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld", static_cast<long long>(total / 3600),
		      static_cast<long long>((total / 60) % 60), static_cast<long long>(total % 60));
	return buffer;
}

/// Quotes a field only when it has to be. TM's own strings are tame, but a field
/// name is operator-typed and nothing stops someone calling a field "Field 1, red
/// side".
std::string escape(const std::string &value)
{
	const bool needsQuotes = value.find_first_of(",\"\n\r") != std::string::npos;
	if (!needsQuotes)
		return value;

	std::string out = "\"";
	for (const char c : value) {
		if (c == '"')
			out += "\"\"";
		else
			out += c;
	}
	out += "\"";
	return out;
}

} // namespace

std::optional<MatchLogRow> MatchLog::started(const FieldsetState &state, int fieldId, int64_t nowMs,
					     int64_t streamOffsetMs, int64_t recordOffsetMs)
{
	// Whatever was running is finished with, one way or another.
	std::optional<MatchLogRow> superseded = close();

	// A timeout has no match number, so there is nothing to log about it.
	if (state.kind != MatchKind::Match)
		return superseded;

	current_ = MatchLogRow{};
	current_.label = state.label;
	current_.round = state.match.round;
	current_.division = state.match.division;
	current_.instance = state.match.instance;
	current_.match = state.match.match;
	current_.fieldId = fieldId != 0 ? fieldId : state.activeField;
	current_.startedAtMs = nowMs;
	current_.streamOffsetMs = streamOffsetMs;
	current_.recordOffsetMs = recordOffsetMs;
	open_ = true;

	return superseded;
}

std::optional<MatchLogRow> MatchLog::stopped(int64_t nowMs)
{
	if (!open_)
		return std::nullopt;

	current_.endedAtMs = nowMs;
	open_ = false;
	return current_;
}

std::optional<MatchLogRow> MatchLog::close()
{
	if (!open_)
		return std::nullopt;

	// Left with endedAtMs zero, which reads as an empty cell: better an honest
	// gap than a made-up end time.
	open_ = false;
	return current_;
}

std::string csvHeader()
{
	return "match,round,division,instance,match_number,field,started,ended,duration_s,stream_offset,record_offset";
}

std::string csvRow(const MatchLogRow &row)
{
	std::ostringstream out;
	out << escape(row.label) << ',' << escape(row.round) << ',' << row.division << ',' << row.instance << ','
	    << row.match << ',' << row.fieldId << ',' << escape(localTimestamp(row.startedAtMs)) << ','
	    << escape(localTimestamp(row.endedAtMs)) << ',';

	if (row.endedAtMs > 0 && row.startedAtMs > 0)
		out << (row.endedAtMs - row.startedAtMs) / 1000;

	out << ',' << escape(clockOffset(row.streamOffsetMs)) << ',' << escape(clockOffset(row.recordOffsetMs));
	return out.str();
}

bool appendMatchCsv(const std::string &path, const MatchLogRow &row, std::string &error)
{
	if (path.empty()) {
		error = "no file configured";
		return false;
	}

	// Checked before opening for append, because opening creates the file and
	// would then always look non-empty.
	bool needsHeader = true;
	{
		std::ifstream existing(path, std::ios::binary | std::ios::ate);
		if (existing.good() && existing.tellg() > 0)
			needsHeader = false;
	}

	std::ofstream file(path, std::ios::app | std::ios::binary);
	if (!file) {
		error = "could not open " + path;
		return false;
	}

	// CRLF: this file is opened in Excel on a Windows laptop far more often than
	// anywhere else.
	if (needsHeader)
		file << csvHeader() << "\r\n";
	file << csvRow(row) << "\r\n";

	file.flush();
	if (!file) {
		// Worth distinguishing: the open succeeded, so the path is fine and the
		// disk or the stick it was on is not.
		error = "could not write to " + path;
		return false;
	}
	return true;
}

} // namespace tmd
