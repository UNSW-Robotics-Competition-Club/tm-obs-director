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

// Integration test for FieldSetSocket against test/mock-tm-server.mjs.
//
// The mock implements RFC 6455 by hand and rejects an unmasked client frame, so
// passing here means we are speaking the real protocol, authorising the upgrade
// with the same signature a REST call uses, and surviving a yanked connection.
//
// Launched by test/run-socket-test.mjs, which supplies the base URL, the
// scenario, and the event script. Not meant to be run by hand.

#include "src/tm/field-set-socket.hpp"
#include "src/tm/tm-api-client.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(const std::string &name, bool condition, const std::string &detail = {})
{
	if (condition) {
		std::cout << "  ok   " << name << "\n";
	} else {
		++g_failures;
		std::cout << "  FAIL " << name;
		if (!detail.empty())
			std::cout << "\n         " << detail;
		std::cout << "\n";
	}
}

/// Collects what the socket thread reports so the test thread can assert on it.
/// Every member is guarded: handlers run on the socket's own thread.
class Recorder {
public:
	void recordEvent(const tmd::TmEvent &event)
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			events_.push_back(event);
		}
		changed_.notify_all();
	}

	void recordState(tmd::SocketState state, const std::string &detail)
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			states_.push_back(state);
			if (!detail.empty())
				lastDetail_ = detail;
		}
		changed_.notify_all();
	}

	/// Waits until at least `count` events have arrived. False on timeout.
	bool waitForEvents(size_t count, int timeoutMs = 15000)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		return changed_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
					 [&]() { return events_.size() >= count; });
	}

	bool waitForState(tmd::SocketState wanted, int timeoutMs = 15000)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		return changed_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]() {
			for (const tmd::SocketState state : states_) {
				if (state == wanted)
					return true;
			}
			return false;
		});
	}

	std::vector<tmd::TmEvent> events() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return events_;
	}

	size_t eventCount() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return events_.size();
	}

	std::string lastDetail() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return lastDetail_;
	}

	bool sawState(tmd::SocketState wanted) const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (const tmd::SocketState state : states_) {
			if (state == wanted)
				return true;
		}
		return false;
	}

private:
	mutable std::mutex mutex_;
	std::condition_variable changed_;
	std::vector<tmd::TmEvent> events_;
	std::vector<tmd::SocketState> states_;
	std::string lastDetail_;
};

tmd::TmConfig configFor(const std::string &base)
{
	tmd::TmConfig config;
	config.baseUrl = base;
	config.apiKey = "TESTAPIKEY0123456789";
	config.cerberus.endpoint = base;
	// Matches CERBERUS_KEY in test/mock-tm-server.mjs.
	config.cerberus.apiKey = "ctm_0123456789abcdef_0123456789abcdef0123456789abcdef0123456789abcdef";
	config.cerberus.buildVersion = "0.1.0";
	return config;
}

tmd::FieldSetSocketConfig socketConfigFor(const std::string &base)
{
	tmd::FieldSetSocketConfig config;
	config.baseUrl = base;
	config.apiKey = "TESTAPIKEY0123456789";
	config.fieldSetId = 1;

	// Short enough that a reconnect happens inside a test's patience, long
	// enough that a refused upgrade does not spin.
	config.initialBackoffMs = 200;
	config.maxBackoffMs = 1000;
	config.healthyConnectionMs = 250;
	return config;
}

/// Wires a socket to a shared TmApiClient, the way the dock will in Phase 4.
/// The mutex is the point: the socket thread calls this while the UI thread may
/// be using the same client for REST polling.
struct Harness {
	explicit Harness(const std::string &base)
		: client(configFor(base)),
		  socket(socketConfigFor(base), [this](std::string &token, std::string &detail) {
			  std::lock_guard<std::mutex> lock(clientMutex);
			  return client.acquireToken(token, detail) == tmd::TmError::None;
		  })
	{
		socket.setEventHandler([this](const tmd::TmEvent &event) { recorder.recordEvent(event); });
		socket.setStateHandler([this](tmd::SocketState state, const std::string &detail) {
			recorder.recordState(state, detail);
		});
	}

	std::mutex clientMutex;
	tmd::TmApiClient client;
	tmd::FieldSetSocket socket;
	Recorder recorder;
};

// --- scenario: parse -------------------------------------------------------

void testEventParsing()
{
	std::cout << "event parsing\n";

	tmd::TmEvent event;

	check("fieldMatchAssigned parses",
	      tmd::parseTmEvent(R"({"type":"fieldMatchAssigned","fieldID":2,)"
				R"("match":{"division":1,"session":0,"round":"QUAL","match":7,"instance":1}})",
				event));
	check("assigned type", event.type == tmd::TmEventType::FieldMatchAssigned);
	check("fieldID read from TM's spelling", event.fieldId == 2, std::to_string(event.fieldId));
	check("match tuple carried", event.hasMatch && event.match.match == 7 && event.match.round == "QUAL");
	check("tuple label", event.match.label() == "QUAL 7", event.match.label());

	check("fieldActivated parses", tmd::parseTmEvent(R"({"type":"fieldActivated","fieldID":1})", event));
	check("activated type and field", event.type == tmd::TmEventType::FieldActivated && event.fieldId == 1);
	check("no match on fieldActivated", !event.hasMatch);

	check("matchStarted parses", tmd::parseTmEvent(R"({"type":"matchStarted","fieldID":3})", event));
	check("started type", event.type == tmd::TmEventType::MatchStarted && event.fieldId == 3);

	check("matchStopped parses", tmd::parseTmEvent(R"({"type":"matchStopped","fieldID":3})", event));
	check("stopped type", event.type == tmd::TmEventType::MatchStopped);

	check("audienceDisplayChanged parses",
	      tmd::parseTmEvent(R"({"type":"audienceDisplayChanged","display":"IN_MATCH"})", event));
	check("display decoded", event.type == tmd::TmEventType::AudienceDisplayChanged && event.hasDisplay &&
					 event.display == tmd::AudienceDisplay::InMatch);

	// A screen TM adds later must not silently read as Blank, which the rule
	// engine would act on as a real instruction to blank the display.
	check("unknown display leaves hasDisplay false",
	      tmd::parseTmEvent(R"({"type":"audienceDisplayChanged","display":"HOLOGRAM"})", event) &&
		      !event.hasDisplay);

	// Same reasoning one level up: an unmodelled event type is surfaced as
	// Unknown with its name intact, so it shows up in the log instead of vanishing.
	check("unknown event type is kept, not dropped",
	      tmd::parseTmEvent(R"({"type":"someFutureEvent","fieldID":1})", event) &&
		      event.type == tmd::TmEventType::Unknown && event.rawType == "someFutureEvent");

	check("raw payload retained", !event.raw.empty());

	check("non-JSON rejected", !tmd::parseTmEvent("not json at all", event));
	check("JSON without a type rejected", !tmd::parseTmEvent(R"({"fieldID":1})", event));
	check("JSON array rejected", !tmd::parseTmEvent(R"([{"type":"matchStarted"}])", event));

	std::cout << "command building\n";

	check("plain command", tmd::buildCommand("start") == R"({"cmd":"start"})", tmd::buildCommand("start"));
	check("setAudienceDisplay",
	      tmd::buildSetAudienceDisplayCommand(tmd::AudienceDisplay::Rankings) ==
		      R"({"cmd":"setAudienceDisplay","display":"RANKINGS"})",
	      tmd::buildSetAudienceDisplayCommand(tmd::AudienceDisplay::Rankings));
	check("queueSkills", tmd::buildQueueSkillsCommand(1) == R"({"cmd":"queueSkills","skillsID":1})",
	      tmd::buildQueueSkillsCommand(1));
}

// --- scenario: stream ------------------------------------------------------

void testEventStream(const std::string &base)
{
	std::cout << "field set event stream\n";

	Harness harness(base);
	harness.socket.start();

	check("socket connects", harness.recorder.waitForState(tmd::SocketState::Connected));

	// The runner emits a full match cycle once a client is attached: assigned,
	// activated, INTRO, started, IN_MATCH, stopped, RESULTS.
	check("all seven scripted events arrive", harness.recorder.waitForEvents(7),
	      std::to_string(harness.recorder.eventCount()) + " arrived");

	const std::vector<tmd::TmEvent> events = harness.recorder.events();

	if (events.size() >= 7) {
		check("1: fieldMatchAssigned", events[0].type == tmd::TmEventType::FieldMatchAssigned);
		check("1: carries the queued match",
		      events[0].hasMatch && events[0].match.match == 1 && events[0].match.round == "QUAL");
		check("2: fieldActivated",
		      events[1].type == tmd::TmEventType::FieldActivated && events[1].fieldId == 1);
		check("3: display INTRO", events[2].type == tmd::TmEventType::AudienceDisplayChanged &&
						  events[2].display == tmd::AudienceDisplay::Intro);
		check("4: matchStarted", events[3].type == tmd::TmEventType::MatchStarted);
		check("5: display IN_MATCH", events[4].display == tmd::AudienceDisplay::InMatch);
		check("6: matchStopped", events[5].type == tmd::TmEventType::MatchStopped);
		check("7: display RESULTS", events[6].display == tmd::AudienceDisplay::Results);
	}

	check("one connection, no spurious reconnects", harness.socket.connectionCount() == 1,
	      std::to_string(harness.socket.connectionCount()) + " connections");

	harness.socket.stop();
	check("stop() reaches the terminal state", harness.socket.state() == tmd::SocketState::Stopped,
	      tmd::socketStateName(harness.socket.state()));
}

// --- scenario: command -----------------------------------------------------

void testOutboundCommand(const std::string &base)
{
	std::cout << "outbound command\n";

	Harness harness(base);
	harness.socket.start();

	check("socket connects", harness.recorder.waitForState(tmd::SocketState::Connected));

	check("command queued",
	      harness.socket.sendCommand(tmd::buildSetAudienceDisplayCommand(tmd::AudienceDisplay::Rankings)));

	// The mock answers a setAudienceDisplay by broadcasting the change, which is
	// what real TM does: everyone listening learns about it, not just the sender.
	check("server echoed audienceDisplayChanged", harness.recorder.waitForEvents(1));

	const std::vector<tmd::TmEvent> events = harness.recorder.events();
	if (!events.empty()) {
		check("echo is the display we asked for",
		      events[0].type == tmd::TmEventType::AudienceDisplayChanged &&
			      events[0].display == tmd::AudienceDisplay::Rankings,
		      events[0].raw);
	}

	harness.socket.stop();

	check("sendCommand refuses once stopped", !harness.socket.sendCommand(tmd::buildCommand("start")));
}

// --- scenario: reconnect ---------------------------------------------------

void testReconnect(const std::string &base)
{
	std::cout << "reconnect after the connection is yanked\n";

	Harness harness(base);
	harness.socket.start();

	check("socket connects", harness.recorder.waitForState(tmd::SocketState::Connected));

	// The runner emits one event, destroys the TCP connection with no close
	// frame, then emits another once we come back. A plugin that gives up here
	// goes silent mid-event with no indication anything is wrong, which is the
	// failure this whole mechanism exists to prevent.
	check("event before the drop", harness.recorder.waitForEvents(1));
	check("drop is reported as Reconnecting", harness.recorder.waitForState(tmd::SocketState::Reconnecting));
	check("event after the reconnect", harness.recorder.waitForEvents(2),
	      std::to_string(harness.recorder.eventCount()) + " arrived");

	check("reconnected rather than reusing the dead socket", harness.socket.connectionCount() >= 2,
	      std::to_string(harness.socket.connectionCount()) + " connections");

	const std::vector<tmd::TmEvent> events = harness.recorder.events();
	if (events.size() >= 2) {
		check("first event was matchStarted", events[0].type == tmd::TmEventType::MatchStarted);
		check("second event survived the reconnect", events[1].type == tmd::TmEventType::MatchStopped);
	}

	harness.socket.stop();
}

// --- scenarios: the upgrade is refused -------------------------------------

void testUpgradeRefused(const std::string &base, const std::string &expectInDetail)
{
	std::cout << "upgrade refused\n";

	Harness harness(base);
	harness.socket.start();

	check("failure is reported, not swallowed", harness.recorder.waitForState(tmd::SocketState::Reconnecting));
	check("never reports a connection", harness.socket.connectionCount() == 0,
	      std::to_string(harness.socket.connectionCount()) + " connections");
	check("never claims to be Connected", !harness.recorder.sawState(tmd::SocketState::Connected));

	const std::string detail = harness.recorder.lastDetail();
	check("detail names the HTTP status", detail.find(expectInDetail) != std::string::npos,
	      "expected \"" + expectInDetail + "\" in \"" + detail + "\"");

	// Backoff must not prevent a prompt stop: an OBS shutdown cannot wait out a
	// 30-second retry timer.
	const auto before = std::chrono::steady_clock::now();
	harness.socket.stop();
	const auto elapsed = std::chrono::steady_clock::now() - before;

	check("stop() interrupts the backoff wait", elapsed < std::chrono::seconds(2),
	      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()) + "ms");
}

/// Wrong TM API key against a healthy server.
///
/// Unlike the bad-signature scenario, nothing here is forced: the mock rebuilds
/// the canonical string for the websocket path and compares it. That is what
/// proves the upgrade is genuinely signed rather than merely carrying headers
/// the server never looks at.
void testWrongApiKey(const std::string &base)
{
	std::cout << "wrong TM API key on the upgrade\n";

	tmd::TmConfig clientConfig = configFor(base);
	tmd::TmApiClient client(clientConfig);

	tmd::FieldSetSocketConfig config = socketConfigFor(base);
	config.apiKey = "WRONG-KEY-FROM-LAST-EVENT";

	std::mutex clientMutex;
	Recorder recorder;

	tmd::FieldSetSocket socket(config, [&](std::string &token, std::string &detail) {
		std::lock_guard<std::mutex> lock(clientMutex);
		return client.acquireToken(token, detail) == tmd::TmError::None;
	});
	socket.setStateHandler(
		[&](tmd::SocketState state, const std::string &detail) { recorder.recordState(state, detail); });

	socket.start();

	check("upgrade is refused", recorder.waitForState(tmd::SocketState::Reconnecting));
	check("never connects with a bad key", socket.connectionCount() == 0,
	      std::to_string(socket.connectionCount()) + " connections");
	check("refusal is a 401", recorder.lastDetail().find("401") != std::string::npos, recorder.lastDetail());

	socket.stop();
}

void testHostUnreachable()
{
	std::cout << "host unreachable\n";

	Harness harness("http://127.0.0.1:1");
	harness.socket.start();

	check("failure is reported", harness.recorder.waitForState(tmd::SocketState::Reconnecting));
	check("no connection claimed", harness.socket.connectionCount() == 0);
	check("detail is not empty", !harness.recorder.lastDetail().empty(), harness.recorder.lastDetail());

	harness.socket.stop();
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::cerr << "usage: test-field-set-socket <scenario> [base-url]\n";
		return 2;
	}

	const std::string scenario = argv[1];
	const std::string base = argc > 2 ? argv[2] : std::string();

	if (scenario == "parse") {
		testEventParsing();
	} else if (scenario == "stream") {
		testEventStream(base);
	} else if (scenario == "command") {
		testOutboundCommand(base);
	} else if (scenario == "reconnect") {
		testReconnect(base);
	} else if (scenario == "bad-key") {
		testWrongApiKey(base);
	} else if (scenario == "bad-signature") {
		testUpgradeRefused(base, "401");
	} else if (scenario == "api-disabled") {
		testUpgradeRefused(base, "503");
	} else if (scenario == "unreachable") {
		testHostUnreachable();
	} else {
		std::cerr << "unknown scenario: " << scenario << "\n";
		return 2;
	}

	if (g_failures == 0) {
		std::cout << "  (" << scenario << " passed)\n";
		return 0;
	}

	std::cout << "  " << g_failures << " FAILED in " << scenario << "\n";
	return 1;
}
