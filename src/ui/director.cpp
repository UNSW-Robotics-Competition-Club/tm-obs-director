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

#include "ui/director.hpp"

#include <obs-frontend-api.h>
#include <obs.h>
#include <plugin-support.h>

#include <QDateTime>
#include <QMetaObject>

#include <algorithm>

namespace tmd {

namespace {

int64_t nowMs()
{
	return QDateTime::currentMSecsSinceEpoch();
}

/// Finds a scene by name.
///
/// obs_get_source_by_name would be shorter and wrong: a filter or an input may
/// share a scene's name, and handing a non-scene to
/// obs_frontend_set_current_scene is not something OBS is obliged to survive.
obs_source_t *findScene(const QString &name)
{
	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);

	obs_source_t *found = nullptr;
	for (size_t i = 0; i < scenes.sources.num; ++i) {
		obs_source_t *source = scenes.sources.array[i];
		if (name == QString::fromUtf8(obs_source_get_name(source))) {
			found = obs_source_get_ref(source);
			break;
		}
	}

	obs_frontend_source_list_free(&scenes);
	return found;
}

QString describeEvent(const TmEvent &event)
{
	QString text = QString::fromUtf8(tmEventTypeName(event.type));
	if (event.type == TmEventType::Unknown && !event.rawType.empty())
		text = QString::fromStdString(event.rawType);
	if (event.fieldId != 0)
		text += QStringLiteral(" on field %1").arg(event.fieldId);
	return text;
}

} // namespace

Director::Director(QObject *parent) : QObject(parent), api_(TmConfig{})
{
	loadDirectorSettings(settings_);

	{
		std::lock_guard<std::mutex> lock(apiMutex_);
		api_.setConfig(tmConfigFor(settings_));
	}

	timer_.setSingleShot(true);
	connect(&timer_, &QTimer::timeout, this, &Director::pumpScheduler);
}

Director::~Director()
{
	// Before anything else: the socket calls handlers that capture `this`, and
	// stop() joins its thread, so nothing can be in flight after this returns.
	stop();
}

TmConfig Director::tmConfigFor(const DirectorSettings &settings) const
{
	TmConfig config;
	config.baseUrl = settings.tmAddress;
	config.apiKey = settings.tmApiKey;
	if (!settings.cerberusEndpoint.empty())
		config.cerberus.endpoint = settings.cerberusEndpoint;
	return config;
}

bool Director::isConfigured() const
{
	return !settings_.tmAddress.empty() && !settings_.tmApiKey.empty() && settings_.fieldSetId != 0;
}

QString Director::lastScene() const
{
	return lastScene_;
}

bool Director::applySettings(const DirectorSettings &settings)
{
	const bool connectionChanged =
		settings.tmAddress != settings_.tmAddress || settings.tmApiKey != settings_.tmApiKey ||
		settings.fieldSetId != settings_.fieldSetId || settings.cerberusEndpoint != settings_.cerberusEndpoint;

	// Only what the API client is built from. The field set id is not part of
	// that - it goes to the socket - so changing it alone leaves the client be.
	const bool clientChanged = settings.tmAddress != settings_.tmAddress ||
				   settings.tmApiKey != settings_.tmApiKey ||
				   settings.cerberusEndpoint != settings_.cerberusEndpoint;

	settings_ = settings;

	// Locked only when there is something to hand the client. A Test connection
	// holds this mutex for its whole request - up to the full timeout on a venue
	// with no uplink - and the dock saves on the UI thread for everything from
	// folding a section to turning automation off. Taking the lock every time
	// froze the whole dock for as long as the test took.
	if (clientChanged) {
		std::lock_guard<std::mutex> lock(apiMutex_);
		api_.setConfig(tmConfigFor(settings_));
	}

	// Editing a scene mapping mid-event must not drop the connection: the
	// reconnect would be invisible in the dock but a real gap in coverage.
	if (connectionChanged) {
		stop();
		start();
	}

	return saveDirectorSettings(settings_);
}

void Director::start()
{
	if (socket_)
		return;

	if (!isConfigured()) {
		handleSocketState(SocketState::Disconnected,
				  tr("Not configured yet: set the address, the API key and a field set."));
		return;
	}

	FieldSetSocketConfig config;
	config.baseUrl = settings_.tmAddress;
	config.apiKey = settings_.tmApiKey;
	config.fieldSetId = settings_.fieldSetId;

	socket_ = std::make_unique<FieldSetSocket>(config, [this](std::string &token, std::string &detail) {
		// Called on the socket thread before every connection attempt. One
		// client, one token, for the whole plugin - which is why it locks.
		std::lock_guard<std::mutex> lock(apiMutex_);
		return api_.acquireToken(token, detail) == TmError::None;
	});

	socket_->setEventHandler([this](const TmEvent &event) {
		// Hop to the Qt main thread. Everything downstream of here touches
		// engine state or libobs, and neither is safe from the socket thread.
		TmEvent copy = event;
		QMetaObject::invokeMethod(this, [this, copy]() { handleEvent(copy); }, Qt::QueuedConnection);
	});

	socket_->setStateHandler([this](SocketState state, const std::string &detail) {
		const QString text = QString::fromStdString(detail);
		QMetaObject::invokeMethod(
			this, [this, state, text]() { handleSocketState(state, text); }, Qt::QueuedConnection);
	});

	socket_->start();
}

void Director::stop()
{
	// A match still running when OBS closes is still a match that happened; it
	// just has no end time.
	if (const auto open = matchLog_.close())
		recordMatch(*open);

	if (socket_) {
		socket_->stop();
		socket_.reset();
	}

	timer_.stop();
	scheduler_.reset();
	state_ = FieldsetState{};
	handleSocketState(SocketState::Stopped, QString());
}

void Director::handleSocketState(SocketState state, const QString &detail)
{
	if (linkState_ == state && linkDetail_ == detail)
		return;

	linkState_ = state;
	linkDetail_ = detail;

	QString line = QString::fromUtf8(socketStateName(state));
	if (!detail.isEmpty())
		line += QStringLiteral(": ") + detail;

	// Also to OBS's log, not just the dock. The dock is cleared by a restart and
	// nobody screenshots it, so when someone asks after an event why switching
	// stopped at 2pm, the log is the only thing that can answer.
	obs_log(LOG_INFO, "link %s", line.toUtf8().constData());

	LogLevel level = LogLevel::Info;
	if (state == SocketState::Connected)
		level = LogLevel::Good;
	else if (state == SocketState::Reconnecting)
		// Not Bad: the socket is already retrying with backoff, and a blip that
		// recovers on its own should not read the same as a wrong API key.
		level = LogLevel::Warn;

	emit activity(line, level);
	emit linkChanged();
}

void Director::handleEvent(const TmEvent &event)
{
	const int64_t now = nowMs();
	state_ = reduceFieldsetState(state_, event, now);
	emit stateChanged();

	// After the reducer, not before: the row wants the match the state machine
	// has just settled on, including the timeout inference.
	if (event.type == TmEventType::MatchStarted) {
		if (const auto superseded =
			    matchLog_.started(state_, event.fieldId, now, streamOffsetMs(), recordOffsetMs()))
			recordMatch(*superseded);
	} else if (event.type == TmEventType::MatchStopped) {
		if (const auto finished = matchLog_.stopped(now))
			recordMatch(*finished);
	}

	const auto decision = settings_.rules.resolve(event);
	if (!decision) {
		emit activity(describeEvent(event) + tr(" - no rule"), LogLevel::Info);
		return;
	}

	scheduler_.submit(*decision, nowMs());

	QString note = describeEvent(event) + tr(" - switching to \"%1\"").arg(QString::fromStdString(decision->scene));
	if (decision->delayMs > 0)
		note += tr(" after %1 ms").arg(decision->delayMs);
	emit activity(note, LogLevel::Info);

	armTimer();
	pumpScheduler();
}

void Director::armTimer()
{
	if (!scheduler_.hasPending()) {
		timer_.stop();
		return;
	}

	const int64_t wait = std::max<int64_t>(0, scheduler_.nextDueMs() - nowMs());
	timer_.start(static_cast<int>(std::min<int64_t>(wait, 60000)));
}

void Director::pumpScheduler()
{
	const auto scene = scheduler_.poll(nowMs());
	if (scene)
		switchScene(QString::fromStdString(*scene));

	armTimer();
}

void Director::switchScene(const QString &scene)
{
	obs_source_t *source = findScene(scene);
	if (source == nullptr) {
		// A renamed or deleted scene. Said out loud rather than swallowed: the
		// automation looks identical to a working one right up until it does
		// nothing.
		const QString line = tr("Scene \"%1\" does not exist - nothing switched").arg(scene);
		obs_log(LOG_WARNING, "%s", line.toUtf8().constData());
		// Bad, not Warn: a renamed scene does not come back by waiting.
		emit activity(line, LogLevel::Bad);
		return;
	}

	obs_frontend_set_current_scene(source);
	obs_source_release(source);

	lastScene_ = scene;
	obs_log(LOG_INFO, "switched to scene '%s'", scene.toUtf8().constData());
	emit activity(tr("Switched to \"%1\"").arg(scene), LogLevel::Info);
	emit stateChanged();
}

void Director::noteStreamingActive(bool active)
{
	streamStartedMs_ = active ? nowMs() : 0;
}

void Director::noteRecordingActive(bool active)
{
	recordStartedMs_ = active ? nowMs() : 0;
}

int64_t Director::streamOffsetMs() const
{
	return streamStartedMs_ > 0 ? nowMs() - streamStartedMs_ : -1;
}

int64_t Director::recordOffsetMs() const
{
	return recordStartedMs_ > 0 ? nowMs() - recordStartedMs_ : -1;
}

void Director::recordMatch(const MatchLogRow &row)
{
	if (!settings_.matchLogEnabled)
		return;

	const std::string path = settings_.matchLogPath.empty() ? defaultMatchLogPath() : settings_.matchLogPath;

	std::string error;
	if (appendMatchCsv(path, row, error)) {
		matchLogFailed_ = false;
		emit activity(tr("Logged %1 to the match CSV").arg(QString::fromStdString(row.label)), LogLevel::Info);
		return;
	}

	obs_log(LOG_WARNING, "match log: %s", error.c_str());
	if (matchLogFailed_)
		return;

	// Once, then quiet. A path that cannot be written fails the same way for
	// every match, and a red line per match would bury the rest of the log.
	matchLogFailed_ = true;
	emit activity(tr("Could not write the match CSV: %1").arg(QString::fromStdString(error)), LogLevel::Bad);
}

TmError Director::testConnection(std::string &detail)
{
	std::lock_guard<std::mutex> lock(apiMutex_);
	return api_.testConnection(detail);
}

std::vector<FieldSet> Director::fetchFieldSets(TmError &error, std::string &detail)
{
	std::lock_guard<std::mutex> lock(apiMutex_);
	const TmResult<std::vector<FieldSet>> result = api_.getFieldSets();
	error = result.error;
	detail = result.detail;
	return result.value;
}

std::vector<Field> Director::fetchFields(int fieldSetId, TmError &error, std::string &detail)
{
	std::lock_guard<std::mutex> lock(apiMutex_);
	const TmResult<std::vector<Field>> result = api_.getFields(fieldSetId);
	error = result.error;
	detail = result.detail;
	return result.value;
}

} // namespace tmd
