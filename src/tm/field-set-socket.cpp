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

// libcurl drags in windows.h, whose min/max macros then eat std::min. This must
// precede every include in the translation unit to have any effect.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "field-set-socket.hpp"
#include "tm-signer.hpp"

#include <curl/curl.h>
#include <curl/websockets.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <mutex>

#ifndef CURLWS_TEXT
#error "This libcurl was built without WebSocket support. \
obs-tm-director needs it for the TM field set event stream. \
Rebuild libcurl with --enable-websockets, or use the libcurl in the OBS \
dependency bundle, which has it."
#endif

namespace tmd {

namespace {

std::once_flag g_curlInit;

/// Refuse to accumulate an unbounded message. TM's events are a couple of
/// hundred bytes; anything approaching this is a broken or hostile peer, and
/// growing a string until the process dies is not a better outcome.
constexpr size_t kMaxMessageBytes = 1u << 20;

/// Rewrites an http(s) base URL to its websocket equivalent.
///
/// The signature is computed over the http form, because host and path are all
/// that get signed and the two schemes serialise them identically: ws defaults
/// to port 80 and wss to 443, exactly as http and https do, so the WHATWG rule
/// about omitting a default port lands the same either way.
std::string toWebSocketUrl(const std::string &httpUrl)
{
	if (httpUrl.rfind("https://", 0) == 0)
		return "wss://" + httpUrl.substr(sizeof("https://") - 1);
	if (httpUrl.rfind("http://", 0) == 0)
		return "ws://" + httpUrl.substr(sizeof("http://") - 1);

	// Already ws/wss, or something we do not recognise. Hand it to curl and let
	// it produce the error rather than silently mangling the address.
	return httpUrl;
}

/// One live curl handle, tied to one connection attempt.
class Connection {
public:
	Connection() = default;

	~Connection()
	{
		if (headers_)
			curl_slist_free_all(headers_);
		if (handle_)
			curl_easy_cleanup(handle_);
	}

	Connection(const Connection &) = delete;
	Connection &operator=(const Connection &) = delete;

	CURL *handle() const { return handle_; }

	bool open(const FieldSetSocketConfig &config, const FieldSetSocket::TokenProvider &tokenProvider,
		  std::string &detail)
	{
		std::string token;
		if (!tokenProvider(token, detail))
			return false;

		const std::string path = "/api/fieldsets/" + std::to_string(config.fieldSetId);
		const std::string signingUrl = config.baseUrl + path;
		const std::string tmDate = formatHttpDate(std::time(nullptr));
		const std::string signature = signRequest("GET", signingUrl, token, tmDate, config.apiKey);

		if (signature.empty()) {
			detail = "could not sign the upgrade request for " + signingUrl +
				 " (is the TM address a valid URL?)";
			return false;
		}

		handle_ = curl_easy_init();
		if (!handle_) {
			detail = "could not initialise libcurl";
			return false;
		}

		// The upgrade carries the same three auth headers as a REST call. This
		// is the capability that decided the websocket library: LibDataChannel
		// cannot set arbitrary headers on the handshake.
		headers_ = curl_slist_append(headers_, ("Authorization: Bearer " + token).c_str());
		headers_ = curl_slist_append(headers_, ("x-tm-date: " + tmDate).c_str());
		headers_ = curl_slist_append(headers_, ("x-tm-signature: " + signature).c_str());

		const std::string socketUrl = toWebSocketUrl(signingUrl);

		curl_easy_setopt(handle_, CURLOPT_URL, socketUrl.c_str());
		curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, headers_);

		// 2 means "do the websocket handshake, then hand me the connection".
		curl_easy_setopt(handle_, CURLOPT_CONNECT_ONLY, 2L);

		curl_easy_setopt(handle_, CURLOPT_CONNECTTIMEOUT_MS, config.connectTimeoutMs);

		// Deliberately no CURLOPT_TIMEOUT: this connection is meant to live for
		// the length of the event.

		// A redirect would invalidate the signature, which is bound to host and
		// path.
		curl_easy_setopt(handle_, CURLOPT_FOLLOWLOCATION, 0L);
		curl_easy_setopt(handle_, CURLOPT_NOSIGNAL, 1L);

		const CURLcode code = curl_easy_perform(handle_);

		long status = 0;
		curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &status);

		if (code != CURLE_OK) {
			detail = std::string(curl_easy_strerror(code));
			if (status != 0)
				detail += " (HTTP " + std::to_string(status) + ")";
			return false;
		}

		// A 401 here means the same things it means on a REST call: wrong API
		// key, wrong client credentials, or a skewed clock. The REST path
		// distinguishes them properly; repeating that logic here would duplicate
		// it, so say what we saw and let Test Connection do the diagnosis.
		if (status != 101) {
			detail = "server refused the websocket upgrade with HTTP " + std::to_string(status);
			return false;
		}

		return true;
	}

private:
	CURL *handle_ = nullptr;
	curl_slist *headers_ = nullptr;
};

} // namespace

const char *socketStateName(SocketState state)
{
	switch (state) {
	case SocketState::Disconnected:
		return "Disconnected";
	case SocketState::Connecting:
		return "Connecting";
	case SocketState::Connected:
		return "Connected";
	case SocketState::Reconnecting:
		return "Reconnecting";
	case SocketState::Stopped:
		return "Stopped";
	}
	return "Unknown";
}

FieldSetSocket::FieldSetSocket(FieldSetSocketConfig config, TokenProvider tokenProvider)
	: config_(std::move(config)),
	  tokenProvider_(std::move(tokenProvider))
{
	// curl_global_init is not thread-safe and must happen before any handle is
	// created on any thread. HttpClient does the same; the call is
	// reference-counted, so doing it in both places is correct, not wasteful.
	std::call_once(g_curlInit, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

FieldSetSocket::~FieldSetSocket()
{
	stop();
}

void FieldSetSocket::setEventHandler(EventHandler handler)
{
	eventHandler_ = std::move(handler);
}

void FieldSetSocket::setStateHandler(StateHandler handler)
{
	stateHandler_ = std::move(handler);
}

void FieldSetSocket::start()
{
	if (thread_.joinable())
		return;

	stopRequested_ = false;
	thread_ = std::thread([this]() { run(); });
}

void FieldSetSocket::stop()
{
	if (!thread_.joinable())
		return;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopRequested_ = true;
	}
	wake_.notify_all();

	thread_.join();
}

bool FieldSetSocket::sendCommand(const std::string &payload)
{
	if (stopRequested_)
		return false;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		outbox_.push_back(payload);
	}
	wake_.notify_all();
	return true;
}

void FieldSetSocket::setState(SocketState state, const std::string &detail)
{
	state_ = state;
	if (stateHandler_)
		stateHandler_(state, detail);
}

bool FieldSetSocket::sleepFor(int milliseconds)
{
	std::unique_lock<std::mutex> lock(mutex_);
	wake_.wait_for(lock, std::chrono::milliseconds(milliseconds), [this]() { return stopRequested_.load(); });
	return !stopRequested_;
}

void FieldSetSocket::run()
{
	int backoffMs = config_.initialBackoffMs;

	const auto backOff = [&](const std::string &reason) {
		setState(SocketState::Reconnecting, reason);
		const bool keepGoing = sleepFor(backoffMs);
		backoffMs = std::min(backoffMs * 2, config_.maxBackoffMs);
		return keepGoing;
	};

	while (!stopRequested_) {
		setState(SocketState::Connecting, {});

		Connection connection;
		std::string detail;

		if (!connection.open(config_, tokenProvider_, detail)) {
			if (stopRequested_ || !backOff(detail))
				break;
			continue;
		}

		connectionCount_.fetch_add(1);
		setState(SocketState::Connected, {});

		const auto connectedAt = std::chrono::steady_clock::now();
		std::string reason = pump(connection.handle());
		const auto lived = std::chrono::steady_clock::now() - connectedAt;

		if (stopRequested_)
			break;

		// Only a connection that actually held up resets the ladder. Otherwise a
		// server that accepts and immediately drops would be hammered at the
		// initial interval indefinitely.
		if (lived >= std::chrono::milliseconds(config_.healthyConnectionMs))
			backoffMs = config_.initialBackoffMs;

		if (!backOff(reason))
			break;
	}

	setState(SocketState::Stopped, {});
}

std::string FieldSetSocket::pump(void *handleAsVoid)
{
	CURL *handle = static_cast<CURL *>(handleAsVoid);

	std::string message;
	char buffer[4096];

	while (!stopRequested_) {
		// Sending has to happen here because only this thread may touch the curl
		// handle; sendCommand() merely queues.
		std::deque<std::string> pending;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			pending.swap(outbox_);
		}

		for (const std::string &payload : pending) {
			size_t sent = 0;
			const CURLcode code =
				curl_ws_send(handle, payload.data(), payload.size(), &sent, 0, CURLWS_TEXT);
			if (code != CURLE_OK)
				return std::string("could not send a field set command: ") + curl_easy_strerror(code);
		}

		size_t received = 0;
		const struct curl_ws_frame *meta = nullptr;
		const CURLcode code = curl_ws_recv(handle, buffer, sizeof(buffer), &received, &meta);

		if (code == CURLE_AGAIN) {
			// Nothing waiting. In connect-only mode libcurl's recv does not
			// block, so this poll interval is what keeps the thread idle — and
			// it bounds how long stop() takes to be noticed.
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait_for(lock, std::chrono::milliseconds(config_.pollIntervalMs),
				       [this]() { return stopRequested_.load() || !outbox_.empty(); });
			continue;
		}

		if (code != CURLE_OK)
			return std::string("connection lost: ") + curl_easy_strerror(code);

		if (!meta)
			return "connection closed by the server";

		if (meta->flags & CURLWS_CLOSE)
			return "server closed the websocket";

		// libcurl answers pings itself, so there is nothing to do but ignore
		// them. They arrive with no payload of interest.
		if (meta->flags & CURLWS_PING)
			continue;
#ifdef CURLWS_PONG
		if (meta->flags & CURLWS_PONG)
			continue;
#endif

		if (message.size() + received > kMaxMessageBytes) {
			message.clear();
			return "server sent an implausibly large websocket message";
		}

		message.append(buffer, received);

		// bytesleft counts the remainder of this frame; CURLWS_CONT says another
		// fragment of the same message follows. Both must be clear before the
		// message is whole. TM's events fit in one frame, but a proxy in the
		// middle is free to fragment them.
		if (meta->bytesleft != 0 || (meta->flags & CURLWS_CONT))
			continue;

		TmEvent event;
		if (parseTmEvent(message, event) && eventHandler_)
			eventHandler_(event);

		message.clear();
	}

	return "stopped";
}

} // namespace tmd
