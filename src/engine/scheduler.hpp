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

#include "engine/rules.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace tmd {

/// Decides when a resolved switch actually happens.
///
/// Two knobs, both of which exist because a match generates several events in
/// quick succession and a stream that lurches between scenes on each one looks
/// broken:
///
///   delay     wait this long before switching, so a switch can be timed to the
///             thing the viewer is about to see rather than to the event
///   min hold  having switched, stay put at least this long
///
/// Deliberately free of threads, timers and OBS. The caller pumps it, which is
/// what makes the awkward cases — a switch superseded mid-delay, a switch landing
/// inside a hold — testable against a fake clock instead of a stopwatch.
class SwitchScheduler {
public:
	/// Accepts a decision, replacing any switch still waiting out its delay.
	///
	/// Replacing rather than queueing is the point. If a match starts while the
	/// "queued" switch is still in its delay, the viewer should see the match,
	/// not the on-deck shot followed a second later by the match.
	void submit(const SceneDecision &decision, int64_t nowMs);

	/// The scene to switch to now, if one is due. Call after submit() and on a
	/// timer.
	///
	/// Does not deduplicate against the scene already showing: only the caller
	/// knows what OBS is actually on, and an operator who switched away by hand
	/// should be brought back.
	std::optional<std::string> poll(int64_t nowMs);

	/// When poll() next has something to do, or zero when nothing is pending.
	int64_t nextDueMs() const;

	bool hasPending() const { return pending_; }

	/// What is waiting, for the dock to show. Empty when nothing is.
	const std::string &pendingScene() const { return pendingScene_; }

	/// The last scene this scheduler handed out. Empty until it has handed one
	/// out.
	const std::string &lastApplied() const { return lastApplied_; }

	/// Forgets the pending switch and the hold. For when automation is switched
	/// off, or the connection is torn down.
	void reset();

private:
	/// The moment the pending switch may happen: its own delay, pushed out by any
	/// hold still owed to the previous switch.
	int64_t dueMs() const;

	bool pending_ = false;
	std::string pendingScene_;
	int64_t pendingReadyMs_ = 0;
	int pendingMinHoldMs_ = 0;

	std::string lastApplied_;
	int64_t lastAppliedMs_ = 0;
	int lastMinHoldMs_ = 0;
};

} // namespace tmd
