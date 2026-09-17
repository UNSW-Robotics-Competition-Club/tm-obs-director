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

#include "engine/scheduler.hpp"

#include <algorithm>

namespace tmd {

void SwitchScheduler::submit(const SceneDecision &decision, int64_t nowMs)
{
	pending_ = true;
	pendingScene_ = decision.scene;
	pendingReadyMs_ = nowMs + std::max(0, decision.delayMs);
	pendingMinHoldMs_ = std::max(0, decision.minHoldMs);
}

int64_t SwitchScheduler::dueMs() const
{
	// A hold defers a switch; it does not cancel it. Dropping the switch instead
	// would mean a match that starts a second after its field was activated never
	// reaches the match scene at all.
	const int64_t holdUntil = lastApplied_.empty() ? 0 : lastAppliedMs_ + lastMinHoldMs_;
	return std::max(pendingReadyMs_, holdUntil);
}

int64_t SwitchScheduler::nextDueMs() const
{
	return pending_ ? dueMs() : 0;
}

std::optional<std::string> SwitchScheduler::poll(int64_t nowMs)
{
	if (!pending_ || nowMs < dueMs())
		return std::nullopt;

	std::string scene = pendingScene_;

	lastApplied_ = scene;
	lastAppliedMs_ = nowMs;
	lastMinHoldMs_ = pendingMinHoldMs_;

	pending_ = false;
	pendingScene_.clear();
	pendingReadyMs_ = 0;
	pendingMinHoldMs_ = 0;

	return scene;
}

void SwitchScheduler::reset()
{
	pending_ = false;
	pendingScene_.clear();
	pendingReadyMs_ = 0;
	pendingMinHoldMs_ = 0;

	lastApplied_.clear();
	lastAppliedMs_ = 0;
	lastMinHoldMs_ = 0;
}

} // namespace tmd
