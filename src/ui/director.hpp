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

#include "engine/fieldset-state.hpp"
#include "engine/match-log.hpp"
#include "engine/scheduler.hpp"
#include "tm/field-set-socket.hpp"
#include "tm/tm-api-client.hpp"
#include "ui/settings.hpp"

#include <QObject>
#include <QString>
#include <QTimer>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace tmd {

/// How much attention a log line deserves.
///
/// The split that matters is Warn against Bad: a warning clears on its own if
/// you wait (the socket is already retrying, the venue's uplink came back),
/// where a bad line stays bad until somebody does something. An operator
/// glancing at a wall of text needs to know which of those they are looking at
/// without reading it.
enum class LogLevel {
	Info, ///< Routine: an event arrived, a scene was switched.
	Good, ///< Something that was broken now works.
	Warn, ///< Broken, but retrying, and it may fix itself.
	Bad,  ///< Broken, and it will stay broken until someone acts.
};

/// Joins the three worlds: TM's socket, the engine's decisions, and OBS.
///
/// Everything here after the socket handler runs on the Qt main thread. That is
/// not a style preference: obs_frontend_set_current_scene may only be called
/// from the UI thread, and FieldSetSocket calls its handlers on its own thread,
/// so the hand-off has to happen somewhere. It happens here, once, rather than
/// being scattered through the dock.
class Director : public QObject {
	Q_OBJECT

public:
	explicit Director(QObject *parent = nullptr);
	~Director() override;

	const DirectorSettings &settings() const { return settings_; }

	/// Applies and saves settings. Reconnects if anything the connection depends
	/// on changed; leaves the socket alone if only the rules did, so editing a
	/// scene mapping mid-event does not drop the connection.
	bool applySettings(const DirectorSettings &settings);

	/// Opens the field set socket, if there is enough configuration to try.
	void start();
	void stop();

	bool isConfigured() const;

	SocketState linkState() const { return linkState_; }
	const QString &linkDetail() const { return linkDetail_; }
	const FieldsetState &fieldsetState() const { return state_; }

	/// The scene the automation last switched to. Empty until it switches one.
	QString lastScene() const;

	/// Told by the frontend when an output starts or stops, so a match row can
	/// say where in the stream and in the recording the match falls. Offsets are
	/// measured from these rather than asked of the output, because an output that
	/// has just stopped no longer has an answer.
	void noteStreamingActive(bool active);
	void noteRecordingActive(bool active);

	/// Blocking calls for the dock's own worker thread. They share one
	/// TmApiClient with the socket thread, so they take the same lock.
	TmError testConnection(std::string &detail);
	std::vector<FieldSet> fetchFieldSets(TmError &error, std::string &detail);
	std::vector<Field> fetchFields(int fieldSetId, TmError &error, std::string &detail);

signals:
	/// The socket's connection state changed.
	void linkChanged();

	/// The inferred match state changed.
	void stateChanged();

	/// One line for the dock's activity log: an event seen, a switch made, a
	/// switch skipped and why.
	void activity(const QString &line, tmd::LogLevel level);

private slots:
	/// Hands whatever is due to OBS. Driven by a single-shot timer armed for the
	/// next due moment rather than by a poll, so an idle plugin is genuinely
	/// idle.
	void pumpScheduler();

private:
	/// Runs on the Qt main thread, having been marshalled off the socket thread.
	void handleEvent(const TmEvent &event);
	void handleSocketState(SocketState state, const QString &detail);

	void armTimer();
	void switchScene(const QString &scene);

	/// Hands a results switch to the checker, which holds it until the match
	/// just played has been saved. Anything submitted since replaces it.
	void checkResultsThenSwitch(const SceneDecision &decision);

	/// Drops a results switch still waiting on its check.
	void cancelResultsCheck();

	/// What the checker has to report.
	enum class ResultsNews {
		Waiting,  ///< Not saved yet; said once per check.
		Saved,    ///< Saved: switch now.
		NotFound, ///< Cannot be checked: switch anyway.
		GaveUp,   ///< Never saved within the wait: do not switch.
	};

	/// The checker's report, back on the Qt main thread. Ignored if the check
	/// has been replaced or cancelled since.
	void onResultsChecked(uint64_t generation, ResultsNews news, const QString &match, const QString &note);

	/// The checker thread's loop. See resultsJob_.
	void runResultsChecker();

	/// Writes one finished match to the CSV, if that is switched on.
	void recordMatch(const MatchLogRow &row);

	/// Milliseconds into each output, or -1 when it is not running.
	int64_t streamOffsetMs() const;
	int64_t recordOffsetMs() const;

	TmConfig tmConfigFor(const DirectorSettings &settings) const;

	DirectorSettings settings_;

	/// Guards api_. The socket thread calls into it for a bearer token, and the
	/// dock's worker thread calls into it for Test Connection and field lists.
	mutable std::mutex apiMutex_;
	TmApiClient api_;

	/// Recreated on every connect, because FieldSetSocketConfig is fixed at
	/// construction and the field set id can change.
	std::unique_ptr<FieldSetSocket> socket_;

	FieldsetState state_;
	SwitchScheduler scheduler_;
	MatchLog matchLog_;

	/// Wall clock when each output started, zero when it is not running.
	int64_t streamStartedMs_ = 0;
	int64_t recordStartedMs_ = 0;

	/// Said once per session rather than once per match: a CSV that cannot be
	/// written fails identically every time, and forty identical red lines bury
	/// everything else in the log.
	bool matchLogFailed_ = false;
	QTimer timer_;

	SocketState linkState_ = SocketState::Disconnected;
	QString linkDetail_;
	QString lastScene_;

	/// The match that last started - the one whose results should come up next.
	/// Not the one queued: TM often queues the next match before anyone shows the
	/// results of this one, and checking that would wait for a match not yet
	/// played.
	std::optional<MatchTuple> lastPlayed_;

	/// One long-lived thread for results checks, rather than one per check.
	/// A check makes blocking HTTP calls, and a thread started per check would
	/// have to be joined by the next one - on the UI thread, waiting out a
	/// request. Instead the UI thread only ever posts a job and moves on; a job
	/// posted while another runs replaces it, and a stale answer is recognised
	/// by its generation and dropped.
	struct ResultsJob {
		uint64_t generation = 0;
		MatchTuple played;
		SceneDecision decision;
	};
	std::mutex resultsMutex_;
	std::condition_variable resultsWake_;
	std::optional<ResultsJob> resultsJob_;        ///< Waiting to be picked up.
	uint64_t resultsGeneration_ = 0;              ///< The job that counts.
	std::optional<SceneDecision> resultsPending_; ///< UI thread only.
	bool resultsQuit_ = false;
	std::thread resultsThread_;
};

} // namespace tmd
