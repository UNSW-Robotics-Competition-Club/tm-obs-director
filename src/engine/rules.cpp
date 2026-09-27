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

#include "engine/rules.hpp"

namespace tmd {

const char *triggerName(Trigger trigger)
{
	switch (trigger) {
	case Trigger::Queued:
		return "Queued";
	case Trigger::Activated:
		return "Activated";
	case Trigger::Started:
		return "Started";
	case Trigger::Stopped:
		return "Stopped";
	}
	return "Queued";
}

bool triggerForEvent(TmEventType type, Trigger &out)
{
	switch (type) {
	case TmEventType::FieldMatchAssigned:
		out = Trigger::Queued;
		return true;
	case TmEventType::FieldActivated:
		out = Trigger::Activated;
		return true;
	case TmEventType::MatchStarted:
		out = Trigger::Started;
		return true;
	case TmEventType::MatchStopped:
		out = Trigger::Stopped;
		return true;
	case TmEventType::AudienceDisplayChanged:
	case TmEventType::Unknown:
		return false;
	}
	return false;
}

const FieldRule *RuleSet::findField(int fieldId) const
{
	for (const FieldRule &rule : fields) {
		if (rule.fieldId == fieldId)
			return &rule;
	}
	return nullptr;
}

FieldRule *RuleSet::findField(int fieldId)
{
	return const_cast<FieldRule *>(static_cast<const RuleSet *>(this)->findField(fieldId));
}

FieldRule &RuleSet::ensureField(int fieldId, const std::string &name)
{
	if (FieldRule *existing = findField(fieldId)) {
		if (!name.empty())
			existing->name = name;
		return *existing;
	}

	FieldRule rule;
	rule.fieldId = fieldId;
	rule.name = name;
	fields.push_back(rule);
	return fields.back();
}

ResultsCheck checkResultsSaved(const MatchTuple &played, const std::vector<Match> &matches)
{
	for (const Match &match : matches) {
		if (match.tuple == played)
			return match.isScored() ? ResultsCheck::Saved : ResultsCheck::NotSaved;
	}
	return ResultsCheck::NotFound;
}

std::optional<SceneDecision> RuleSet::resolve(const TmEvent &event) const
{
	if (!enabled)
		return std::nullopt;

	if (event.type == TmEventType::AudienceDisplayChanged) {
		// Only the results screen. The others either follow the match events
		// this already switches on (intro, in-match) or are not match moments
		// at all (rankings, alliance selection), and are left to the operator.
		if (!event.hasDisplay || event.display != AudienceDisplay::Results || resultsScene.empty())
			return std::nullopt;

		SceneDecision decision;
		decision.scene = resultsScene;
		decision.delayMs = defaultDelayMs;
		decision.minHoldMs = defaultMinHoldMs;
		decision.results = true;
		return decision;
	}

	Trigger trigger = Trigger::Queued;
	if (!triggerForEvent(event.type, trigger))
		return std::nullopt;

	// Field 0 is TM saying "no field", which happens on the assignment that
	// clears the field set. There is nothing to switch to.
	if (event.fieldId == 0)
		return std::nullopt;

	const FieldRule *rule = findField(event.fieldId);
	if (rule == nullptr)
		return std::nullopt;

	const std::string &specific = rule->sceneFor[static_cast<size_t>(trigger)];
	const std::string &scene = specific.empty() ? rule->defaultScene : specific;
	if (scene.empty())
		return std::nullopt;

	SceneDecision decision;
	decision.scene = scene;
	decision.delayMs = rule->delayMs >= 0 ? rule->delayMs : defaultDelayMs;
	decision.minHoldMs = rule->minHoldMs >= 0 ? rule->minHoldMs : defaultMinHoldMs;
	decision.trigger = trigger;
	decision.fieldId = event.fieldId;
	return decision;
}

} // namespace tmd
