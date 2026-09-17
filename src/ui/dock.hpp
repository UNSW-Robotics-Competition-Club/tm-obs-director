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

#pragma once

#include "ui/director.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <atomic>
#include <thread>
#include <vector>

namespace tmd {

/// One field's row of scene pickers.
struct FieldRow {
	int fieldId = 0;
	QString name;

	/// Everything for this field, so a rebuild deletes one widget rather than
	/// having to find the right ancestor of a combo box.
	QWidget *container = nullptr;

	/// Click target that collapses the field. An event with six fields is six
	/// blocks of five pickers, and an operator watching one field should not have
	/// to scroll past the other five.
	QToolButton *header = nullptr;
	QWidget *body = nullptr;

	QComboBox *defaultScene = nullptr;
	QComboBox *triggerScene[kTriggerCount] = {};
	QSpinBox *delay = nullptr;
	QSpinBox *minHold = nullptr;
};

/// The operator's whole view of the plugin.
///
/// Three questions, in the order they get asked at a venue: is it talking to TM,
/// what should it do, and what has it just done. The last one is not decoration -
/// automation that cannot be watched is automation that cannot be trusted, and
/// an operator who does not trust it will turn it off.
class TmDirectorDock : public QWidget {
	Q_OBJECT

public:
	explicit TmDirectorDock(Director *director, QWidget *parent = nullptr);
	~TmDirectorDock() override;

	/// Repopulates every scene picker. Called when OBS's scene list changes, and
	/// on the scene collection changing, so a picker never offers a scene that
	/// has gone.
	void refreshScenes();

private slots:
	void onTestConnection();
	void onFieldSetChanged(int index);
	void onReloadFields();
	void onSave();
	void onEnabledToggled(bool enabled);

	void onLinkChanged();
	void onStateChanged();
	void onActivity(const QString &line, tmd::LogLevel level);

private:
	QWidget *buildConnectionGroup();
	QWidget *buildRulesGroup();
	QWidget *buildLiveGroup();

	void loadFromSettings();
	DirectorSettings collectSettings() const;

	void rebuildFieldRows(const std::vector<Field> &fields);
	void setAllFieldsCollapsed(bool collapsed);
	void populateSceneCombo(QComboBox *combo, const QString &keep) const;

	/// Puts the connection state on the Test connection button itself: the label
	/// becomes the state, washed green, amber or red, with the full message on
	/// the tooltip.
	///
	/// The button is the thing anyone actually looks at in this group, and the
	/// status line that used to sit under it was empty for most of its life while
	/// still reserving its row.
	void setConnectionState(tmd::LogLevel level, const QString &label, const QString &detail);

	/// Returns the button to its untested look.
	///
	/// Called when the address or key is edited, which is the moment an earlier
	/// result stops describing anything. This replaces a timer that used to blank
	/// the old status line after five seconds: a button that resets itself five
	/// seconds after you press it is a worse affordance than one that holds its
	/// answer until the question changes.
	void clearConnectionState();

	/// Appends one coloured line to the activity log.
	void appendLog(const QString &line, tmd::LogLevel level);

	/// Shows a connection result on the button and copies it to the log.
	void reportConnection(const QString &message, tmd::LogLevel level);

	/// Runs a blocking TM call off the UI thread.
	///
	/// A venue laptop with no uplink takes the full timeout to fail, and a dock
	/// frozen for twenty seconds looks like a crash to the person standing in
	/// front of it.
	void runAsync(std::function<void()> work);
	void setBusy(bool busy);

	Director *director_ = nullptr;

	QLineEdit *address_ = nullptr;
	QLineEdit *apiKey_ = nullptr;
	QComboBox *fieldSet_ = nullptr;
	QPushButton *test_ = nullptr;
	QPushButton *reloadFields_ = nullptr;
	QPushButton *save_ = nullptr;

	QCheckBox *enabled_ = nullptr;
	QSpinBox *defaultDelay_ = nullptr;
	QSpinBox *defaultMinHold_ = nullptr;
	QVBoxLayout *fieldsLayout_ = nullptr;
	QToolButton *collapseAll_ = nullptr;

	/// Stands in for the field rows before TM has been asked for any. An empty
	/// box says nothing about whether the plugin is broken or simply has not been
	/// pointed at an event yet.
	QLabel *noFields_ = nullptr;

	QLabel *linkLabel_ = nullptr;
	QLabel *matchLabel_ = nullptr;
	QLabel *sceneLabel_ = nullptr;
	QPlainTextEdit *log_ = nullptr;

	std::vector<FieldRow> rows_;

	/// Scene names the saved rules refer to that OBS does not have. Kept in the
	/// pickers and marked, because silently dropping one turns a typo or a
	/// not-yet-created scene into a rule that vanished without being edited.
	QStringList sceneNames_;

	std::thread worker_;
	std::atomic<bool> busy_{false};

	/// True while loadFromSettings is writing into widgets, so their change
	/// signals do not read half-populated state back out.
	bool loading_ = false;
};

} // namespace tmd
