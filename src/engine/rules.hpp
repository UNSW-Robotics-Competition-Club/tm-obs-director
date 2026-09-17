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

#include "tm/tm-events.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace tmd {

/// The four moments an operator switches on.
///
/// TM sends a fifth event, audienceDisplayChanged, which is deliberately not a
/// trigger: it fires whenever anyone touches the audience display, including in
/// response to our own switching, and wiring it to a scene change invites a
/// loop.
enum class Trigger {
	Queued,    ///< fieldMatchAssigned - a match is on deck for this field.
	Activated, ///< fieldActivated - this field is the one on the audience display.
	Started,   ///< matchStarted - the timer is running.
	Stopped,   ///< matchStopped - the timer stopped, by the clock or by an abort.
};

constexpr int kTriggerCount = 4;

const char *triggerName(Trigger trigger);

/// Maps a wire event onto a trigger. False for events that are not triggers.
bool triggerForEvent(TmEventType type, Trigger &out);

/// What one field does.
///
/// The common venue setup is a camera per field, so a field has one scene and
/// the per-trigger overrides are the exception rather than the rule. An empty
/// scene name means "no switch", which is how a field is excluded without
/// deleting its rule.
struct FieldRule {
	int fieldId = 0;

	/// TM's own name for the field, kept so the dock can label the row and so a
	/// saved configuration still reads sensibly if TM renumbers fields.
	std::string name;

	std::string defaultScene;

	/// Per-trigger override, indexed by Trigger. Empty falls back to
	/// defaultScene.
	std::array<std::string, kTriggerCount> sceneFor;

	/// Negative means "inherit the rule set's value". Zero is a real zero.
	int delayMs = -1;
	int minHoldMs = -1;
};

/// A switch to make, once its delay has elapsed.
struct SceneDecision {
	std::string scene;
	int delayMs = 0;
	int minHoldMs = 0;
	Trigger trigger = Trigger::Queued;
	int fieldId = 0;
};

struct RuleSet {
	/// Master switch. An operator taking manual control mid-match must be able to
	/// stop the automation without dismantling its configuration.
	bool enabled = true;

	int defaultDelayMs = 0;
	int defaultMinHoldMs = 0;

	std::vector<FieldRule> fields;

	const FieldRule *findField(int fieldId) const;
	FieldRule *findField(int fieldId);

	/// Adds a rule for `fieldId` if there is not one already, and returns it.
	FieldRule &ensureField(int fieldId, const std::string &name = {});

	/// What this event should do, or nothing.
	///
	/// Takes only the event: with scene switching as the single action, the
	/// decision depends on which field and which trigger, both of which every
	/// trigger event carries. Nothing here needs the inferred match state.
	std::optional<SceneDecision> resolve(const TmEvent &event) const;
};

} // namespace tmd
