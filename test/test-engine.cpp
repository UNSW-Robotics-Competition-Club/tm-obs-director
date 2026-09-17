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

// Unit tests for the engine: the state a field set is inferred to be in, the
// rules that turn an event into a switch, and the timing that decides when that
// switch lands.
//
// No TM, no OBS, no real clock. Time is a parameter throughout the engine, which
// is the only reason the awkward cases below can be tested at all.

#include "src/engine/fieldset-state.hpp"
#include "src/engine/match-log.hpp"
#include "src/engine/rules.hpp"
#include "src/engine/scheduler.hpp"

#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void check(const std::string &name, bool condition, const std::string &detail = {})
{
	if (condition) {
		std::cout << "  ok   " << name << "\n";
		return;
	}
	++g_failures;
	std::cout << "  FAIL " << name;
	if (!detail.empty())
		std::cout << "\n         " << detail;
	std::cout << "\n";
}

tmd::TmEvent assigned(int fieldId, const std::string &round, int instance, int match)
{
	tmd::TmEvent event;
	event.type = tmd::TmEventType::FieldMatchAssigned;
	event.fieldId = fieldId;
	event.hasMatch = true;
	event.match.round = round;
	event.match.instance = instance;
	event.match.match = match;
	return event;
}

tmd::TmEvent bare(tmd::TmEventType type, int fieldId)
{
	tmd::TmEvent event;
	event.type = type;
	event.fieldId = fieldId;
	return event;
}

// --- match names -----------------------------------------------------------

void testMatchNames()
{
	std::cout << "match names\n";

	auto name = [](const std::string &round, int instance, int match) {
		tmd::MatchTuple tuple;
		tuple.round = round;
		tuple.instance = instance;
		tuple.match = match;
		return tmd::shortMatchName(tuple);
	};

	check("qualification is Q12", name("QUAL", 1, 12) == "Q12", name("QUAL", 1, 12));
	check("practice is P3", name("PRACTICE", 1, 3) == "P3");
	check("round robin is RR4", name("ROUND_ROBIN", 1, 4) == "RR4");
	// The instance matters here and nowhere else: there is more than one QF 1.
	check("quarterfinal carries its instance", name("QF", 2, 1) == "QF 2-1", name("QF", 2, 1));
	check("round of 16 carries its instance", name("R16", 3, 2) == "R16 3-2");
	check("skills is not split into driver and programming", name("SKILLS", 1, 1) == "Skills");
	check("an unknown round still reads sensibly", name("WEIRD", 1, 7) == "WEIRD 7");
}

// --- state inference -------------------------------------------------------

void testQueueThenRun()
{
	std::cout << "a match through its life\n";

	tmd::FieldsetState state;

	state = tmd::reduceFieldsetState(state, assigned(2, "QUAL", 1, 12), 1000);
	check("queueing sets the field", state.activeField == 2);
	check("queueing labels the match", state.label == "Q12", state.label);
	check("queued is unplayed", state.queue == tmd::QueueState::Unplayed);
	check("a queued match is a match", state.kind == tmd::MatchKind::Match);
	check("nothing has started yet", state.startedAtMs == 0);

	state = tmd::reduceFieldsetState(state, bare(tmd::TmEventType::FieldActivated, 2), 2000);
	check("activation leaves the label alone", state.label == "Q12");

	state = tmd::reduceFieldsetState(state, bare(tmd::TmEventType::MatchStarted, 2), 3000);
	check("starting marks it running", state.queue == tmd::QueueState::Running);
	check("the start time is recorded", state.startedAtMs == 3000);

	state = tmd::reduceFieldsetState(state, bare(tmd::TmEventType::MatchStopped, 2), 5000);
	check("stopping marks it stopped", state.queue == tmd::QueueState::Stopped);
	check("the start time survives the stop", state.startedAtMs == 3000);
}

void testTimeoutShapes()
{
	std::cout << "the two shapes of a timeout\n";

	// Shape one: an assignment with no match object at all.
	tmd::TmEvent empty;
	empty.type = tmd::TmEventType::FieldMatchAssigned;
	empty.fieldId = 1;
	empty.hasMatch = false;

	tmd::FieldsetState state = tmd::reduceFieldsetState(tmd::FieldsetState{}, empty, 1000);
	check("an empty assignment on a field is a timeout", state.kind == tmd::MatchKind::Timeout);
	check("and is labelled as one", state.label == "Timeout", state.label);

	// Shape two: a tuple whose round says TIMEOUT.
	state = tmd::reduceFieldsetState(tmd::FieldsetState{}, assigned(1, "TIMEOUT", 0, 0), 1000);
	check("a TIMEOUT round is also a timeout", state.kind == tmd::MatchKind::Timeout);

	// Neither match nor field: TM clearing the field set, which is not a timeout.
	tmd::TmEvent cleared;
	cleared.type = tmd::TmEventType::FieldMatchAssigned;
	cleared.fieldId = 0;
	cleared.hasMatch = false;

	state = tmd::reduceFieldsetState(state, cleared, 2000);
	check("no match and no field clears the field set", state.kind == tmd::MatchKind::None);
	check("and clears the label with it", state.label.empty(), state.label);
}

void testStartWithNothingQueued()
{
	std::cout << "a start with nothing queued\n";

	// TM has no event for starting a timeout. A start arriving with nothing
	// queued is the only signal that one is running.
	tmd::FieldsetState state =
		tmd::reduceFieldsetState(tmd::FieldsetState{}, bare(tmd::TmEventType::MatchStarted, 3), 1000);

	check("is inferred to be a timeout", state.kind == tmd::MatchKind::Timeout);
	check("is labelled as one", state.label == "Timeout", state.label);
	check("and is running", state.queue == tmd::QueueState::Running);
	check("on the field that started it", state.activeField == 3);
}

void testIgnoredEvents()
{
	std::cout << "events that change nothing\n";

	tmd::FieldsetState state = tmd::reduceFieldsetState(tmd::FieldsetState{}, assigned(1, "QUAL", 1, 5), 1000);

	tmd::TmEvent display;
	display.type = tmd::TmEventType::AudienceDisplayChanged;
	display.hasDisplay = true;

	const tmd::FieldsetState after = tmd::reduceFieldsetState(state, display, 2000);
	check("audience display changes leave the match alone", after.label == "Q5" && after.activeField == 1);

	tmd::TmEvent unknown;
	unknown.type = tmd::TmEventType::Unknown;
	unknown.rawType = "somethingTMAddedLater";

	const tmd::FieldsetState stillAfter = tmd::reduceFieldsetState(after, unknown, 3000);
	check("an unrecognised event is inert, not fatal", stillAfter.label == "Q5");
}

// --- rules -----------------------------------------------------------------

tmd::RuleSet twoFields()
{
	tmd::RuleSet rules;
	rules.defaultDelayMs = 500;
	rules.defaultMinHoldMs = 2000;

	tmd::FieldRule &one = rules.ensureField(1, "Field 1");
	one.defaultScene = "F1 Cam";
	one.sceneFor[static_cast<size_t>(tmd::Trigger::Started)] = "F1 Match";

	tmd::FieldRule &two = rules.ensureField(2, "Field 2");
	two.defaultScene = "F2 Cam";

	return rules;
}

void testRuleResolution()
{
	std::cout << "resolving an event to a scene\n";

	const tmd::RuleSet rules = twoFields();

	const auto queued = rules.resolve(assigned(1, "QUAL", 1, 12));
	check("a queued match falls back to the field's scene", queued && queued->scene == "F1 Cam");
	check("and inherits the default delay", queued && queued->delayMs == 500);
	check("and the default hold", queued && queued->minHoldMs == 2000);

	const auto started = rules.resolve(bare(tmd::TmEventType::MatchStarted, 1));
	check("a start uses the override", started && started->scene == "F1 Match");

	const auto otherField = rules.resolve(bare(tmd::TmEventType::MatchStarted, 2));
	check("a field without an override falls back", otherField && otherField->scene == "F2 Cam");

	const auto unmapped = rules.resolve(bare(tmd::TmEventType::MatchStarted, 9));
	check("an unmapped field does nothing", !unmapped.has_value());

	const auto noField = rules.resolve(bare(tmd::TmEventType::MatchStarted, 0));
	check("field zero does nothing", !noField.has_value());

	tmd::TmEvent display;
	display.type = tmd::TmEventType::AudienceDisplayChanged;
	display.fieldId = 1;
	check("audience display is not a trigger", !rules.resolve(display).has_value());
}

void testRuleOverrides()
{
	std::cout << "per-field timing and opting out\n";

	tmd::RuleSet rules = twoFields();
	rules.findField(2)->delayMs = 0;
	rules.findField(2)->minHoldMs = 0;

	const auto immediate = rules.resolve(bare(tmd::TmEventType::FieldActivated, 2));
	check("zero is a real zero, not inherit", immediate && immediate->delayMs == 0 && immediate->minHoldMs == 0);

	// An empty scene is how a field is excluded without deleting its row.
	rules.findField(2)->defaultScene.clear();
	check("a field with no scene does nothing", !rules.resolve(bare(tmd::TmEventType::FieldActivated, 2)));

	rules.enabled = false;
	check("the master switch stops everything", !rules.resolve(bare(tmd::TmEventType::MatchStarted, 1)));
}

// --- scheduling ------------------------------------------------------------

tmd::SceneDecision decision(const std::string &scene, int delayMs, int minHoldMs)
{
	tmd::SceneDecision out;
	out.scene = scene;
	out.delayMs = delayMs;
	out.minHoldMs = minHoldMs;
	return out;
}

void testDelay()
{
	std::cout << "delay\n";

	tmd::SwitchScheduler scheduler;
	scheduler.submit(decision("A", 1000, 0), 0);

	check("nothing happens immediately", !scheduler.poll(0).has_value());
	check("nor just before the delay is up", !scheduler.poll(999).has_value());
	check("knows when it is next due", scheduler.nextDueMs() == 1000);

	const auto applied = scheduler.poll(1000);
	check("switches once the delay elapses", applied && *applied == "A");
	check("and only once", !scheduler.poll(2000).has_value());
	check("remembers what it did", scheduler.lastApplied() == "A");
}

void testSupersede()
{
	std::cout << "a second decision during a delay\n";

	tmd::SwitchScheduler scheduler;
	scheduler.submit(decision("Queued", 1000, 0), 0);
	scheduler.submit(decision("Match", 1000, 0), 500);

	check("the first switch never happens", !scheduler.poll(1000).has_value());

	const auto applied = scheduler.poll(1500);
	check("only the newer one does", applied && *applied == "Match", applied ? *applied : "(none)");
	check("nothing is left queued behind it", !scheduler.hasPending());
}

void testMinimumHold()
{
	std::cout << "minimum hold\n";

	tmd::SwitchScheduler scheduler;
	scheduler.submit(decision("A", 0, 2000), 0);
	check("the first switch is not held back", scheduler.poll(0).value_or("") == "A");

	scheduler.submit(decision("B", 0, 0), 500);
	check("a switch inside the hold waits", !scheduler.poll(500).has_value());
	check("it is still waiting late in the hold", !scheduler.poll(1999).has_value());
	check("and it is deferred, not dropped", scheduler.hasPending());
	check("due at the end of the hold", scheduler.nextDueMs() == 2000);

	const auto applied = scheduler.poll(2000);
	check("it lands when the hold expires", applied && *applied == "B");
}

void testHoldAndDelayCompose()
{
	std::cout << "a delay that outlasts a hold\n";

	tmd::SwitchScheduler scheduler;
	scheduler.submit(decision("A", 0, 1000), 0);
	scheduler.poll(0);

	// The delay ends at 3000, the hold at 1000. The later of the two wins, so
	// neither silently overrides the other.
	scheduler.submit(decision("B", 2000, 0), 1000);
	check("the hold does not shorten the delay", !scheduler.poll(2000).has_value());
	check("due when the delay says", scheduler.nextDueMs() == 3000);
	check("switches then", scheduler.poll(3000).value_or("") == "B");
}

void testRepeatedScene()
{
	std::cout << "switching to the scene already showing\n";

	tmd::SwitchScheduler scheduler;
	scheduler.submit(decision("A", 0, 0), 0);
	scheduler.poll(0);

	// Not deduplicated on purpose: only the caller knows what OBS is actually
	// showing, and an operator who switched away by hand should be brought back.
	scheduler.submit(decision("A", 0, 0), 100);
	check("is still handed out", scheduler.poll(100).value_or("") == "A");
}

void testReset()
{
	std::cout << "reset\n";

	tmd::SwitchScheduler scheduler;
	scheduler.submit(decision("A", 0, 5000), 0);
	scheduler.poll(0);
	scheduler.submit(decision("B", 0, 0), 100);

	scheduler.reset();
	check("forgets what was pending", !scheduler.hasPending());
	check("forgets what it last did", scheduler.lastApplied().empty());

	// The hold is forgotten too, so re-enabling automation does not sit idle
	// waiting out a hold from before it was switched off.
	scheduler.submit(decision("C", 0, 0), 200);
	check("and owes no hold to anything", scheduler.poll(200).value_or("") == "C");
}

// --- match log -------------------------------------------------------------

tmd::FieldsetState queuedMatch(int fieldId, const std::string &round, int instance, int match)
{
	return tmd::reduceFieldsetState(tmd::FieldsetState{}, assigned(fieldId, round, instance, match), 0);
}

void testMatchLogPairing()
{
	std::cout << "match log: a match start paired with its stop\n";

	tmd::MatchLog log;
	const tmd::FieldsetState state = queuedMatch(2, "QUAL", 1, 12);

	check("starting opens a row, closing nothing", !log.started(state, 2, 1000, 5000, 9000).has_value());
	check("a match is open", log.hasOpenMatch());

	const auto row = log.stopped(106000);
	check("stopping yields the row", row.has_value());
	check("labelled", row && row->label == "Q12", row ? row->label : "(none)");
	check("carries the match number", row && row->match == 12);
	check("and the field", row && row->fieldId == 2);
	check("duration is the gap", row && row->endedAtMs - row->startedAtMs == 105000);
	check("offsets are kept", row && row->streamOffsetMs == 5000 && row->recordOffsetMs == 9000);
	check("nothing is left open", !log.hasOpenMatch());

	check("a second stop yields nothing", !log.stopped(120000).has_value());
}

void testMatchLogSupersede()
{
	std::cout << "match log: a match that never stopped\n";

	tmd::MatchLog log;
	log.started(queuedMatch(1, "QUAL", 1, 5), 1, 1000, -1, -1);

	// TM moved on without a stop. The first match is closed out rather than
	// dropped: a row with no end time is still evidence it ran.
	const auto superseded = log.started(queuedMatch(1, "QUAL", 1, 6), 1, 50000, -1, -1);
	check("the abandoned match is handed back", superseded.has_value());
	check("it is the first one", superseded && superseded->match == 5);
	check("with no end time", superseded && superseded->endedAtMs == 0);

	const auto row = log.stopped(160000);
	check("the newer match still logs normally", row && row->match == 6);
}

void testMatchLogIgnoresTimeouts()
{
	std::cout << "match log: timeouts are not matches\n";

	tmd::MatchLog log;

	tmd::TmEvent empty;
	empty.type = tmd::TmEventType::FieldMatchAssigned;
	empty.fieldId = 1;
	empty.hasMatch = false;
	const tmd::FieldsetState timeout = tmd::reduceFieldsetState(tmd::FieldsetState{}, empty, 0);

	check("a timeout opens nothing", !log.started(timeout, 1, 1000, -1, -1).has_value());
	check("nothing is open", !log.hasOpenMatch());
	check("its stop yields nothing", !log.stopped(60000).has_value());

	// A timeout called mid-match still closes the match out.
	log.started(queuedMatch(1, "QUAL", 1, 7), 1, 100000, -1, -1);
	const auto closed = log.started(timeout, 1, 150000, -1, -1);
	check("a timeout closes an open match", closed && closed->match == 7);
	check("and opens nothing itself", !log.hasOpenMatch());
}

void testMatchLogClose()
{
	std::cout << "match log: closing mid-match\n";

	tmd::MatchLog log;
	check("closing with nothing open yields nothing", !log.close().has_value());

	log.started(queuedMatch(3, "QF", 2, 1), 3, 1000, -1, -1);
	const auto row = log.close();
	check("hands back the open match", row && row->label == "QF 2-1", row ? row->label : "(none)");
	check("with no end time", row && row->endedAtMs == 0);
	check("and closes it", !log.hasOpenMatch());
}

void testCsvFormatting()
{
	std::cout << "match log: CSV\n";

	check("header names every column",
	      tmd::csvHeader() ==
		      "match,round,division,instance,match_number,field,started,ended,duration_s,stream_offset,record_offset");

	tmd::MatchLogRow row;
	row.label = "Q12";
	row.round = "QUAL";
	row.division = 1;
	row.instance = 1;
	row.match = 12;
	row.fieldId = 2;
	row.startedAtMs = 1600000000000;
	row.endedAtMs = 1600000105000;
	row.streamOffsetMs = 3661000; // 01:01:01
	row.recordOffsetMs = -1;

	const std::string line = tmd::csvRow(row);
	check("starts with the label", line.rfind("Q12,QUAL,1,1,12,2,", 0) == 0, line);
	check("duration in whole seconds", line.find(",105,") != std::string::npos, line);
	check("offset as hh:mm:ss for a chapter list", line.find("01:01:01") != std::string::npos, line);
	// An output that was not running leaves the cell empty rather than writing a
	// zero, which would read as "right at the start".
	check("an inactive output is an empty cell", line.substr(line.size() - 1) == ",", line);

	tmd::MatchLogRow awkward;
	awkward.label = "Field 1, red side";
	awkward.round = "He said \"go\"";
	const std::string escaped = tmd::csvRow(awkward);
	check("a comma is quoted", escaped.find("\"Field 1, red side\"") != std::string::npos, escaped);
	check("a quote is doubled", escaped.find("\"He said \"\"go\"\"\"") != std::string::npos, escaped);

	tmd::MatchLogRow unfinished;
	unfinished.label = "Q13";
	unfinished.startedAtMs = 1600000000000;
	const std::string open = tmd::csvRow(unfinished);
	check("a match with no end leaves end and duration empty", open.find(",,,") != std::string::npos, open);
}

} // namespace

int main()
{
	testMatchNames();
	testQueueThenRun();
	testTimeoutShapes();
	testStartWithNothingQueued();
	testIgnoredEvents();
	testRuleResolution();
	testRuleOverrides();
	testDelay();
	testSupersede();
	testMinimumHold();
	testHoldAndDelayCompose();
	testRepeatedScene();
	testReset();
	testMatchLogPairing();
	testMatchLogSupersede();
	testMatchLogIgnoresTimeouts();
	testMatchLogClose();
	testCsvFormatting();

	if (g_failures == 0) {
		std::cout << "\nall engine tests passed\n";
		return 0;
	}
	std::cout << "\n" << g_failures << " FAILED\n";
	return 1;
}
