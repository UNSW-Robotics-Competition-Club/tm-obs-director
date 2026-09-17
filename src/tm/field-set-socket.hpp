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

#include "tm-events.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace tmd {

/// Where the field set connection currently stands. Shown in the dock, so the
/// user can tell "TM is quiet" from "we lost the socket ten minutes ago".
enum class SocketState {
	Disconnected, ///< Never started, or start() has not run yet.
	Connecting,   ///< Attempting the upgrade right now.
	Connected,    ///< Upgraded; events are flowing.
	Reconnecting, ///< Attempt failed or the peer went away; waiting out a backoff.
	Stopped,      ///< stop() ran. Terminal.
};

const char *socketStateName(SocketState state);

struct FieldSetSocketConfig {
	/// Same base address as the REST client, e.g. "http://192.168.1.50:8080".
	/// The scheme is rewritten to ws/wss internally; callers pass http/https so
	/// there is exactly one address in the config file and in the dock.
	std::string baseUrl;

	/// TM API key. Same key the REST client signs with.
	std::string apiKey;

	int fieldSetId = 1;

	/// Reconnect ladder: first wait, doubling to the cap. Starts where
	/// tm-client's DEFAULT_BACKOFF_MS starts, so a dropped cable behaves the same
	/// whichever implementation an event happens to be running.
	int initialBackoffMs = 1000;
	int maxBackoffMs = 30000;

	/// A connection that survives at least this long is treated as good, which
	/// resets the ladder. Without it, a server that accepts and instantly drops
	/// would be retried at the initial interval forever.
	int healthyConnectionMs = 5000;

	/// How long to idle between empty reads. libcurl's websocket recv is
	/// non-blocking in connect-only mode, so this is the poll interval — and
	/// also the worst-case latency of stop().
	int pollIntervalMs = 50;

	long connectTimeoutMs = 10000;
};

/// Persistent websocket to one TM field set.
///
/// Owns a thread that connects, reads events, and reconnects with backoff until
/// stop(). One instance per field set; TM exposes one socket per field set and
/// an event with more than one field set needs more than one of these.
///
/// **Handlers run on the socket thread**, not the caller's. Anything touching
/// libobs or Qt must marshal across — the dock does this in Phase 4.
class FieldSetSocket {
public:
	/// Supplies a current OAuth bearer token, returning false (with a reason) if
	/// one cannot be obtained. Called on the socket thread before every
	/// connection attempt, so an implementation sharing a TmApiClient with other
	/// threads must lock. Returning false is treated as a failed attempt and
	/// backs off, which is the right behaviour when the venue's internet drops.
	using TokenProvider = std::function<bool(std::string &token, std::string &detail)>;

	using EventHandler = std::function<void(const TmEvent &)>;

	/// `detail` explains the transition when there is anything to explain: the
	/// curl error behind a failed connect, or why the socket dropped.
	using StateHandler = std::function<void(SocketState state, const std::string &detail)>;

	FieldSetSocket(FieldSetSocketConfig config, TokenProvider tokenProvider);
	~FieldSetSocket();

	FieldSetSocket(const FieldSetSocket &) = delete;
	FieldSetSocket &operator=(const FieldSetSocket &) = delete;

	/// Set handlers before start(); they are not guarded against concurrent
	/// replacement while the thread is running.
	void setEventHandler(EventHandler handler);
	void setStateHandler(StateHandler handler);

	void start();

	/// Stops the thread and joins it. Safe to call more than once, and called by
	/// the destructor.
	void stop();

	/// Queues a command for the socket thread to send, e.g. the output of
	/// buildSetAudienceDisplayCommand(). Returns false if the socket is stopped.
	///
	/// Queued, not sent: only the socket thread may touch the curl handle. A
	/// command queued while disconnected is **dropped** on the next reconnect
	/// rather than replayed — every field set command is time-critical, and
	/// firing a stale "start" at a match that already began would be worse than
	/// losing it.
	bool sendCommand(const std::string &payload);

	SocketState state() const { return state_.load(); }

	/// Number of successful upgrades since start(). Lets a caller — or a test —
	/// tell a reconnect from a connection that never dropped.
	int connectionCount() const { return connectionCount_.load(); }

private:
	void run();

	/// Reads events until the connection drops or stop() is called, sending any
	/// queued commands along the way. Returns a human-readable reason for the
	/// exit, which becomes the detail on the Reconnecting transition.
	///
	/// Takes the curl handle as void* purely so that libcurl stays out of this
	/// header; the only caller is run(), which has the real type.
	std::string pump(void *curlHandle);

	void setState(SocketState state, const std::string &detail);

	/// Waits out a backoff, returning false if stop() intervened.
	bool sleepFor(int milliseconds);

	FieldSetSocketConfig config_;
	TokenProvider tokenProvider_;
	EventHandler eventHandler_;
	StateHandler stateHandler_;

	std::thread thread_;
	std::atomic<bool> stopRequested_{false};
	std::atomic<SocketState> state_{SocketState::Disconnected};
	std::atomic<int> connectionCount_{0};

	mutable std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<std::string> outbox_;
};

} // namespace tmd
