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

#include "ui/dock.hpp"

#include <obs-frontend-api.h>
#include <obs.h>

#include <QDateTime>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMetaObject>
#include <QScrollArea>

namespace tmd {

namespace {

/// Shown as the first entry of every picker. An empty scene name is how a field
/// or a trigger is excluded, so "do nothing" has to be selectable, not just
/// achievable by leaving something blank.
const char *kNoScene = "(no switch)";

/// Shown for a per-trigger picker that should follow the field's own scene.
const char *kUseFieldScene = "(use field scene)";

QStringList currentSceneNames()
{
	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);

	QStringList names;
	for (size_t i = 0; i < scenes.sources.num; ++i)
		names << QString::fromUtf8(obs_source_get_name(scenes.sources.array[i]));

	obs_frontend_source_list_free(&scenes);
	names.sort(Qt::CaseInsensitive);
	return names;
}

QString sceneFromCombo(const QComboBox *combo)
{
	const QVariant data = combo->currentData();
	return data.isValid() ? data.toString() : QString();
}

/// Seconds in the UI, milliseconds in the engine. Operators think in seconds and
/// nobody wants to type 1500.
QSpinBox *makeSecondsBox(const QString &suffix)
{
	auto *box = new QSpinBox();
	box->setRange(0, 60);
	box->setSuffix(suffix);
	box->setSingleStep(1);
	return box;
}

} // namespace

TmDirectorDock::TmDirectorDock(Director *director, QWidget *parent) : QWidget(parent), director_(director)
{
	auto *layout = new QVBoxLayout(this);
	layout->addWidget(buildConnectionGroup());
	layout->addWidget(buildRulesGroup(), 1);
	layout->addWidget(buildLiveGroup());

	connect(director_, &Director::linkChanged, this, &TmDirectorDock::onLinkChanged);
	connect(director_, &Director::stateChanged, this, &TmDirectorDock::onStateChanged);
	connect(director_, &Director::activity, this, &TmDirectorDock::onActivity);

	refreshScenes();
	loadFromSettings();
	onLinkChanged();
	onStateChanged();
}

TmDirectorDock::~TmDirectorDock()
{
	// Joined before any member is torn down, so a worker cannot deliver a result
	// into a half-destroyed widget.
	if (worker_.joinable())
		worker_.join();
}

QWidget *TmDirectorDock::buildConnectionGroup()
{
	auto *group = new QGroupBox(tr("Tournament Manager"));
	auto *form = new QFormLayout(group);

	address_ = new QLineEdit();
	address_->setPlaceholderText(QStringLiteral("http://127.0.0.1"));
	form->addRow(tr("Address"), address_);

	apiKey_ = new QLineEdit();
	apiKey_->setEchoMode(QLineEdit::Password);
	// Named rather than described, because the key is regenerated per event and
	// "it worked last week" is the most common way this goes wrong.
	apiKey_->setPlaceholderText(tr("This event's Web Publishing key"));
	apiKey_->setToolTip(tr("In Tournament Manager: Tools > Options > Web Publishing. TM generates a new "
			       "key for every event."));
	form->addRow(tr("API key"), apiKey_);

	fieldSet_ = new QComboBox();
	form->addRow(tr("Field set"), fieldSet_);

	auto *buttons = new QHBoxLayout();
	test_ = new QPushButton(tr("Test connection"));
	reloadFields_ = new QPushButton(tr("Reload fields"));
	buttons->addWidget(test_);
	buttons->addWidget(reloadFields_);
	buttons->addStretch(1);
	form->addRow(buttons);

	connectionStatus_ = new QLabel();
	connectionStatus_->setWordWrap(true);
	form->addRow(connectionStatus_);

	connect(test_, &QPushButton::clicked, this, &TmDirectorDock::onTestConnection);
	connect(reloadFields_, &QPushButton::clicked, this, &TmDirectorDock::onReloadFields);
	connect(fieldSet_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&TmDirectorDock::onFieldSetChanged);

	return group;
}

QWidget *TmDirectorDock::buildRulesGroup()
{
	auto *group = new QGroupBox(tr("Scenes"));
	auto *outer = new QVBoxLayout(group);

	enabled_ = new QCheckBox(tr("Automation on"));
	// The one control an operator reaches for mid-match, so it sits above
	// everything it governs rather than at the bottom of a scrolling list.
	enabled_->setToolTip(tr("Stops all switching without changing any of the mapping below."));
	outer->addWidget(enabled_);

	auto *defaults = new QHBoxLayout();
	defaultDelay_ = makeSecondsBox(tr(" s delay"));
	defaultMinHold_ = makeSecondsBox(tr(" s hold"));
	defaults->addWidget(new QLabel(tr("Default")));
	defaults->addWidget(defaultDelay_);
	defaults->addWidget(defaultMinHold_);
	defaults->addStretch(1);
	outer->addLayout(defaults);

	auto *scroll = new QScrollArea();
	scroll->setWidgetResizable(true);
	auto *inner = new QWidget();
	fieldsLayout_ = new QVBoxLayout(inner);

	noFields_ = new QLabel(tr("No fields yet. Enter the address and API key above, then "
				  "press Test connection."));
	noFields_->setWordWrap(true);
	noFields_->setAlignment(Qt::AlignCenter);
	noFields_->setEnabled(false);
	fieldsLayout_->addWidget(noFields_);

	fieldsLayout_->addStretch(1);
	scroll->setWidget(inner);
	outer->addWidget(scroll, 1);

	save_ = new QPushButton(tr("Save"));
	outer->addWidget(save_);

	connect(save_, &QPushButton::clicked, this, &TmDirectorDock::onSave);
	connect(enabled_, &QCheckBox::toggled, this, &TmDirectorDock::onEnabledToggled);

	return group;
}

QWidget *TmDirectorDock::buildLiveGroup()
{
	auto *group = new QGroupBox(tr("Live"));
	auto *form = new QFormLayout(group);

	linkLabel_ = new QLabel();
	linkLabel_->setWordWrap(true);
	form->addRow(tr("Link"), linkLabel_);

	matchLabel_ = new QLabel();
	form->addRow(tr("Match"), matchLabel_);

	sceneLabel_ = new QLabel();
	form->addRow(tr("Last switch"), sceneLabel_);

	log_ = new QPlainTextEdit();
	log_->setReadOnly(true);
	log_->setMaximumBlockCount(200);
	form->addRow(log_);

	return group;
}

void TmDirectorDock::refreshScenes()
{
	sceneNames_ = currentSceneNames();

	// Each picker keeps its own selection across the repopulate, including a name
	// OBS no longer has.
	for (FieldRow &row : rows_) {
		populateSceneCombo(row.defaultScene, sceneFromCombo(row.defaultScene));
		for (QComboBox *combo : row.triggerScene)
			populateSceneCombo(combo, sceneFromCombo(combo));
	}
}

void TmDirectorDock::populateSceneCombo(QComboBox *combo, const QString &keep) const
{
	const QSignalBlocker blocker(combo);
	combo->clear();

	const bool isTrigger = combo->property("trigger").isValid();
	combo->addItem(tr(isTrigger ? kUseFieldScene : kNoScene), QString());

	for (const QString &name : sceneNames_)
		combo->addItem(name, name);

	if (keep.isEmpty()) {
		combo->setCurrentIndex(0);
		return;
	}

	const int index = combo->findData(keep);
	if (index >= 0) {
		combo->setCurrentIndex(index);
		return;
	}

	// Configured but missing. Kept and marked rather than dropped: a scene not
	// created yet is an ordinary state on setup day, and silently clearing the
	// rule would be discovered mid-event.
	combo->addItem(tr("%1 (missing)").arg(keep), keep);
	combo->setCurrentIndex(combo->count() - 1);
}

void TmDirectorDock::rebuildFieldRows(const std::vector<Field> &fields)
{
	for (FieldRow &row : rows_) {
		if (row.defaultScene != nullptr)
			row.defaultScene->parentWidget()->deleteLater();
	}
	rows_.clear();

	const RuleSet &rules = director_->settings().rules;

	for (const Field &field : fields) {
		FieldRow row;
		row.fieldId = field.id;
		row.name = QString::fromStdString(field.name);

		auto *box = new QGroupBox(row.name.isEmpty() ? tr("Field %1").arg(field.id) : row.name);
		auto *grid = new QGridLayout(box);

		const FieldRule *saved = rules.findField(field.id);

		row.defaultScene = new QComboBox();
		grid->addWidget(new QLabel(tr("Scene")), 0, 0);
		grid->addWidget(row.defaultScene, 0, 1);
		populateSceneCombo(row.defaultScene, saved ? QString::fromStdString(saved->defaultScene) : QString());

		for (int t = 0; t < kTriggerCount; ++t) {
			auto *combo = new QComboBox();
			combo->setProperty("trigger", t);
			row.triggerScene[t] = combo;

			const QString label = QString::fromUtf8(triggerName(static_cast<Trigger>(t)));
			grid->addWidget(new QLabel(label), t + 1, 0);
			grid->addWidget(combo, t + 1, 1);
			populateSceneCombo(combo, saved ? QString::fromStdString(saved->sceneFor[t]) : QString());
		}

		auto *timing = new QHBoxLayout();
		row.delay = makeSecondsBox(tr(" s delay"));
		row.minHold = makeSecondsBox(tr(" s hold"));
		// -1 in the model means "inherit". The UI says so in words instead, since
		// a spin box showing -1 reads as a bug.
		row.delay->setSpecialValueText(tr("default"));
		row.minHold->setSpecialValueText(tr("default"));
		row.delay->setMinimum(-1);
		row.minHold->setMinimum(-1);
		row.delay->setValue(saved && saved->delayMs >= 0 ? saved->delayMs / 1000 : -1);
		row.minHold->setValue(saved && saved->minHoldMs >= 0 ? saved->minHoldMs / 1000 : -1);
		timing->addWidget(row.delay);
		timing->addWidget(row.minHold);
		timing->addStretch(1);
		grid->addLayout(timing, kTriggerCount + 1, 0, 1, 2);

		// Ahead of the trailing stretch, so rows stack from the top.
		fieldsLayout_->insertWidget(fieldsLayout_->count() - 1, box);
		rows_.push_back(row);
	}

	if (fields.empty())
		onActivity(tr("TM reported no fields for this field set."));

	noFields_->setVisible(rows_.empty());
	if (rows_.empty() && !fields.empty())
		noFields_->setText(tr("TM reported no fields for this field set."));
}

void TmDirectorDock::loadFromSettings()
{
	loading_ = true;

	const DirectorSettings &settings = director_->settings();
	address_->setText(QString::fromStdString(settings.tmAddress));
	apiKey_->setText(QString::fromStdString(settings.tmApiKey));
	enabled_->setChecked(settings.rules.enabled);
	defaultDelay_->setValue(settings.rules.defaultDelayMs / 1000);
	defaultMinHold_->setValue(settings.rules.defaultMinHoldMs / 1000);

	// The field set list needs TM, so until Test connection or Reload fields has
	// run there is only the saved id to show.
	if (fieldSet_->count() == 0 && settings.fieldSetId != 0) {
		const QSignalBlocker blocker(fieldSet_);
		fieldSet_->addItem(tr("Field set %1 (saved)").arg(settings.fieldSetId), settings.fieldSetId);
	}

	// Rows for the saved rules, so a restart shows the mapping without needing
	// TM to answer first.
	std::vector<Field> saved;
	for (const FieldRule &rule : settings.rules.fields)
		saved.push_back(Field{rule.fieldId, rule.name});
	if (!saved.empty())
		rebuildFieldRows(saved);

	loading_ = false;
}

DirectorSettings TmDirectorDock::collectSettings() const
{
	DirectorSettings settings = director_->settings();
	settings.tmAddress = address_->text().trimmed().toStdString();
	settings.tmApiKey = apiKey_->text().trimmed().toStdString();

	const QVariant fieldSet = fieldSet_->currentData();
	settings.fieldSetId = fieldSet.isValid() ? fieldSet.toInt() : 0;

	settings.rules.enabled = enabled_->isChecked();
	settings.rules.defaultDelayMs = defaultDelay_->value() * 1000;
	settings.rules.defaultMinHoldMs = defaultMinHold_->value() * 1000;

	settings.rules.fields.clear();
	for (const FieldRow &row : rows_) {
		FieldRule rule;
		rule.fieldId = row.fieldId;
		rule.name = row.name.toStdString();
		rule.defaultScene = sceneFromCombo(row.defaultScene).toStdString();
		for (int t = 0; t < kTriggerCount; ++t)
			rule.sceneFor[t] = sceneFromCombo(row.triggerScene[t]).toStdString();
		rule.delayMs = row.delay->value() < 0 ? -1 : row.delay->value() * 1000;
		rule.minHoldMs = row.minHold->value() < 0 ? -1 : row.minHold->value() * 1000;
		settings.rules.fields.push_back(rule);
	}

	return settings;
}

void TmDirectorDock::runAsync(std::function<void()> work)
{
	if (busy_.load())
		return;

	if (worker_.joinable())
		worker_.join();

	setBusy(true);
	worker_ = std::thread([this, work]() {
		work();
		QMetaObject::invokeMethod(
			this, [this]() { setBusy(false); }, Qt::QueuedConnection);
	});
}

void TmDirectorDock::setBusy(bool busy)
{
	busy_.store(busy);
	test_->setEnabled(!busy);
	reloadFields_->setEnabled(!busy);
}

void TmDirectorDock::onTestConnection()
{
	// Save first: testing settings the operator can see but the client has not
	// been given is a good way to debug the wrong thing.
	onSave();

	connectionStatus_->setText(tr("Testing..."));

	runAsync([this]() {
		std::string detail;
		const TmError error = director_->testConnection(detail);

		TmError fieldSetError = TmError::None;
		std::string fieldSetDetail;
		std::vector<FieldSet> fieldSets;
		if (error == TmError::None)
			fieldSets = director_->fetchFieldSets(fieldSetError, fieldSetDetail);

		const QString remedy = QString::fromUtf8(tmErrorRemedy(error));
		const QString detailText = QString::fromStdString(detail);

		QMetaObject::invokeMethod(
			this,
			[this, error, remedy, detailText, fieldSets]() {
				connectionStatus_->setText(remedy);
				if (error != TmError::None) {
					// The remedy is for the operator; the detail is for
					// whoever they forward the log to.
					onActivity(QStringLiteral("%1 (%2)")
							   .arg(QString::fromUtf8(tmErrorName(error)), detailText));
					return;
				}

				const QSignalBlocker blocker(fieldSet_);
				const int previous = fieldSet_->currentData().toInt();
				fieldSet_->clear();
				for (const FieldSet &set : fieldSets) {
					fieldSet_->addItem(QString::fromStdString(set.name), set.id);
				}
				const int index = fieldSet_->findData(previous);
				if (index >= 0)
					fieldSet_->setCurrentIndex(index);

				onActivity(tr("Connected. %1 field set(s).").arg(fieldSets.size()));
				onReloadFields();
			},
			Qt::QueuedConnection);
	});
}

void TmDirectorDock::onFieldSetChanged(int index)
{
	if (loading_ || index < 0)
		return;
	onSave();
	onReloadFields();
}

void TmDirectorDock::onReloadFields()
{
	const int fieldSetId = fieldSet_->currentData().toInt();
	if (fieldSetId == 0)
		return;

	runAsync([this, fieldSetId]() {
		TmError error = TmError::None;
		std::string detail;
		const std::vector<Field> fields = director_->fetchFields(fieldSetId, error, detail);

		const QString remedy = QString::fromUtf8(tmErrorRemedy(error));

		QMetaObject::invokeMethod(
			this,
			[this, error, remedy, fields]() {
				if (error != TmError::None) {
					connectionStatus_->setText(remedy);
					return;
				}
				rebuildFieldRows(fields);
				onSave();
			},
			Qt::QueuedConnection);
	});
}

void TmDirectorDock::onSave()
{
	if (loading_)
		return;

	if (!director_->applySettings(collectSettings()))
		onActivity(tr("Could not write settings to %1").arg(QString::fromStdString(directorSettingsPath())));

	director_->start();
}

void TmDirectorDock::onEnabledToggled(bool enabled)
{
	if (loading_)
		return;
	onSave();
	onActivity(enabled ? tr("Automation on") : tr("Automation off"));
}

void TmDirectorDock::onLinkChanged()
{
	QString text = QString::fromUtf8(socketStateName(director_->linkState()));
	if (!director_->linkDetail().isEmpty())
		text += QStringLiteral(" - ") + director_->linkDetail();
	linkLabel_->setText(text);
}

void TmDirectorDock::onStateChanged()
{
	const FieldsetState &state = director_->fieldsetState();

	QString match = state.label.empty() ? tr("nothing queued") : QString::fromStdString(state.label);
	match += QStringLiteral(" - ") + QString::fromUtf8(queueStateName(state.queue));
	if (state.activeField != 0)
		match += tr(" on field %1").arg(state.activeField);
	matchLabel_->setText(match);

	sceneLabel_->setText(director_->lastScene().isEmpty() ? tr("none yet") : director_->lastScene());
}

void TmDirectorDock::onActivity(const QString &line)
{
	log_->appendPlainText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss ")) + line);
}

} // namespace tmd
