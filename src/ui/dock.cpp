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

#include <QColor>
#include <QDateTime>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMetaObject>
#include <QScrollArea>
#include <QSplitter>
#include <QToolButton>

namespace tmd {

namespace {

/// Shown as the first entry of every picker. An empty scene name is how a field
/// or a trigger is excluded, so "do nothing" has to be selectable, not just
/// achievable by leaving something blank.
const char *kNoScene = "(no switch)";

/// Shown for a per-trigger picker that should follow the field's own scene.
/// Worded to match the row it refers to: the label above says "Default scene",
/// so this cannot say "field scene" and expect anyone to connect the two.
const char *kUseFieldScene = "(use default)";

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

/// Colours that hold up on both OBS's dark themes and its light ones. Picked for
/// contrast against either ground rather than taken from a palette role, because
/// a log line has to stay legible whichever theme the operator runs.
QString colourFor(tmd::LogLevel level)
{
	switch (level) {
	case tmd::LogLevel::Good:
		return QStringLiteral("#2ea043");
	case tmd::LogLevel::Warn:
		return QStringLiteral("#bb8009");
	case tmd::LogLevel::Bad:
		return QStringLiteral("#d1242f");
	case tmd::LogLevel::Info:
		break;
	}
	return QString();
}

/// What the Test connection button says once it is carrying the state.
///
/// Short enough to fit half the dock's width. The sentence explaining what to do
/// about it is too long for a button, so it goes to the tooltip and the log,
/// where it is not competing with the label for space.
QString testLabelFor(tmd::LogLevel level)
{
	switch (level) {
	case tmd::LogLevel::Good:
		return QObject::tr("Connected");
	// Warn and Bad differ in whether waiting will fix it, which is what the
	// amber-versus-red carries. Both mean the same thing to read: no link.
	case tmd::LogLevel::Warn:
		return QObject::tr("Not connected");
	case tmd::LogLevel::Bad:
		return QObject::tr("Connection failed");
	case tmd::LogLevel::Info:
		break;
	}
	return QObject::tr("Test connection");
}

/// A wash of the status colour behind the button, rather than a solid fill.
///
/// Overriding background-color on a QPushButton drops the theme's own hover and
/// pressed styling, so those states are restated here at stronger alphas -- a
/// button that does not respond to the pointer reads as disabled. Kept to a
/// light tint so it still looks like a button and not a status lamp, and left
/// entirely alone for Info so an untested button matches every other one.
QString testButtonStyle(tmd::LogLevel level)
{
	const QString colour = colourFor(level);
	if (colour.isEmpty())
		return QString();

	const QColor base(colour);
	const auto wash = [&base](int percent) {
		return QStringLiteral("rgba(%1, %2, %3, %4%)")
			.arg(base.red())
			.arg(base.green())
			.arg(base.blue())
			.arg(percent);
	};

	return QStringLiteral("QPushButton { background-color: %1; color: %2; }"
			      "QPushButton:hover { background-color: %3; }"
			      "QPushButton:pressed { background-color: %4; }"
			      // Held through the disabled state too, because the button is
			      // disabled for the whole of the test it is reporting on.
			      "QPushButton:disabled { background-color: %1; color: %2; }")
		.arg(wash(15), colour, wash(25), wash(35));
}

/// Which failures an operator can wait out, and which need somebody to act.
///
/// Anything upstream of the venue - no uplink, the sign-in service unreachable,
/// rate limiting - is a warning: it is not wrong, it is just not working yet.
/// A wrong key or a disabled API is red, because waiting will not fix it.
tmd::LogLevel levelFor(tmd::TmError error)
{
	switch (error) {
	case tmd::TmError::None:
		return tmd::LogLevel::Good;

	case tmd::TmError::HostUnreachable:
	case tmd::TmError::CerberusUnreachable:
	case tmd::TmError::AuthHostUnreachable:
	case tmd::TmError::CerberusRateLimited:
	case tmd::TmError::TokenExpired:
	case tmd::TmError::HttpError:
		return tmd::LogLevel::Warn;

	default:
		return tmd::LogLevel::Bad;
	}
}

QString sceneFromCombo(const QComboBox *combo)
{
	const QVariant data = combo->currentData();
	return data.isValid() ? data.toString() : QString();
}

/// Seconds in the UI, milliseconds in the engine. Operators think in seconds and
/// nobody wants to type 1500.
///
/// The unit is a suffix and the meaning is a label beside the box, rather than
/// both crammed into the suffix: a narrow dock elides the end of a suffix, and
/// "0 s de" is worse than useless. A per-field box also shows "default" instead
/// of a number, which hides the suffix entirely - so the label is the only thing
/// that can say which box is the delay and which is the hold.
QSpinBox *makeSecondsBox()
{
	auto *box = new QSpinBox();
	box->setRange(0, 60);
	box->setSuffix(QObject::tr(" s"));
	box->setSingleStep(1);
	box->setMinimumWidth(80);
	return box;
}

} // namespace

TmDirectorDock::TmDirectorDock(Director *director, QWidget *parent) : QWidget(parent), director_(director)
{
	auto *layout = new QVBoxLayout(this);

	// A splitter rather than fixed proportions. How much room the scene mapping
	// needs depends on how many fields the event has, and how much of the log an
	// operator wants to see depends on how much they trust the automation yet -
	// neither is something this code can pick correctly for everyone.
	auto *splitter = new QSplitter(Qt::Vertical);
	splitter->setChildrenCollapsible(false);

	QWidget *connection = buildConnectionGroup();
	QWidget *rules = buildRulesGroup();
	QWidget *live = buildLiveGroup();

	// Neither the connection form nor the live readout has anything to do with
	// extra height: a form row does not grow, so surplus space in those panes just
	// sits at the bottom of the box as a gap nobody asked for.
	connection->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
	live->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

	splitter->addWidget(connection);
	splitter->addWidget(rules);
	splitter->addWidget(live);

	splitter->setStretchFactor(0, 0);
	splitter->setStretchFactor(1, 1);
	splitter->setStretchFactor(2, 0);

	// Deliberately not real pixel sizes. A splitter clamps each pane to its
	// minimum, so asking for one pixel top and bottom means "as small as you can
	// be" and hands every remaining pixel to the mapping - which is the pane that
	// actually has more to show when the dock is tall.
	splitter->setSizes({1, 10000, 1});

	layout->addWidget(splitter);

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
	// Equal halves of the dock's width rather than their text width plus a
	// stretch. They are the two things anyone reaches for here, and a button that
	// fills its half is easier to hit than one hugging its label.
	test_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	reloadFields_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	buttons->addWidget(test_, 1);
	buttons->addWidget(reloadFields_, 1);
	form->addRow(buttons);

	clearConnectionState();

	// An earlier result describes the address and key that produced it, so editing
	// either one is exactly when it stops being true. textEdited rather than
	// textChanged, so loading saved settings does not count as an edit.
	connect(address_, &QLineEdit::textEdited, this, &TmDirectorDock::clearConnectionState);
	connect(apiKey_, &QLineEdit::textEdited, this, &TmDirectorDock::clearConnectionState);

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

	// The one control an operator reaches for mid-match, so it sits above
	// everything it governs rather than at the bottom of a scrolling list.
	enabled_ = new QCheckBox(tr("Automation on"));
	enabled_->setToolTip(tr("Stops all switching without changing any of the mapping below."));

	collapseAll_ = new QToolButton();
	collapseAll_->setCheckable(true);
	collapseAll_->setAutoRaise(true);
	collapseAll_->setText(tr("Collapse all"));
	collapseAll_->setToolTip(tr("Fold every field down to its name."));
	connect(collapseAll_, &QToolButton::toggled, this, [this](bool collapsed) {
		collapseAll_->setText(collapsed ? tr("Expand all") : tr("Collapse all"));
		setAllFieldsCollapsed(collapsed);
	});

	auto *top = new QHBoxLayout();
	top->addWidget(enabled_);
	top->addStretch(1);
	top->addWidget(collapseAll_);
	outer->addLayout(top);

	// A form rather than one row. A dock is narrow, and a label, a spin box,
	// another label, another spin box and a button do not fit across it - they
	// just silently clip the ends of the numbers, which is how "0 s" became
	// "0 s de" on the first attempt. Vertical space is the cheap axis here.
	auto *defaults = new QFormLayout();
	defaultDelay_ = makeSecondsBox();
	defaultMinHold_ = makeSecondsBox();
	defaults->addRow(tr("Default delay"), defaultDelay_);
	defaults->addRow(tr("Default hold"), defaultMinHold_);
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
	// Enough to see the last few things that happened without crowding out the
	// scene mapping. Drag the splitter for more.
	log_->setMinimumHeight(60);
	log_->setMaximumHeight(120);
	form->addRow(log_);

	return group;
}

void TmDirectorDock::setConnectionState(tmd::LogLevel level, const QString &label, const QString &detail)
{
	test_->setText(label.isEmpty() ? testLabelFor(level) : label);
	test_->setStyleSheet(testButtonStyle(level));
	// The button can only hold two or three words, so the sentence that says what
	// to do about it lives here. Falls back to naming the button's own job, which
	// is what it means before anyone has pressed it.
	test_->setToolTip(detail.isEmpty() ? tr("Check the connection to Tournament Manager.") : detail);
}

void TmDirectorDock::clearConnectionState()
{
	setConnectionState(tmd::LogLevel::Info, testLabelFor(tmd::LogLevel::Info), QString());
}

void TmDirectorDock::reportConnection(const QString &message, tmd::LogLevel level)
{
	setConnectionState(level, testLabelFor(level), message);
	// Copied rather than only shown: the button keeps two words and a colour, and
	// the log is what is still there when someone asks what actually happened.
	appendLog(message, level);
}

void TmDirectorDock::appendLog(const QString &line, tmd::LogLevel level)
{
	const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
	// Escaped, not interpolated: TM's own error bodies come through here and a
	// stray angle bracket would otherwise eat the rest of the line.
	const QString body = line.toHtmlEscaped();
	const QString colour = colourFor(level);

	if (colour.isEmpty())
		log_->appendHtml(QStringLiteral("%1 %2").arg(stamp, body));
	else
		log_->appendHtml(QStringLiteral("%1 <span style=\"color:%2;\">%3</span>").arg(stamp, colour, body));
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

void TmDirectorDock::setAllFieldsCollapsed(bool collapsed)
{
	for (FieldRow &row : rows_) {
		if (row.header != nullptr)
			row.header->setChecked(!collapsed);
	}
}

void TmDirectorDock::rebuildFieldRows(const std::vector<Field> &fields)
{
	for (FieldRow &row : rows_) {
		if (row.container != nullptr)
			row.container->deleteLater();
	}
	rows_.clear();

	const RuleSet &rules = director_->settings().rules;

	for (const Field &field : fields) {
		FieldRow row;
		row.fieldId = field.id;
		row.name = QString::fromStdString(field.name);

		const QString title = row.name.isEmpty() ? tr("Field %1").arg(field.id) : row.name;

		row.container = new QWidget();
		auto *stack = new QVBoxLayout(row.container);
		stack->setContentsMargins(0, 0, 0, 0);
		stack->setSpacing(0);

		// A header that folds the field away, rather than a QGroupBox. A checkable
		// QGroupBox would give the same arrow for free, but its checkbox means
		// "enabled", and an operator who collapsed a field to get it out of the way
		// would have switched it off instead.
		row.header = new QToolButton();
		row.header->setText(title);
		row.header->setCheckable(true);
		row.header->setChecked(true);
		row.header->setArrowType(Qt::DownArrow);
		row.header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		row.header->setAutoRaise(true);
		row.header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		stack->addWidget(row.header);

		row.body = new QWidget();
		auto *grid = new QGridLayout(row.body);
		stack->addWidget(row.body);

		QToolButton *header = row.header;
		QWidget *body = row.body;
		connect(header, &QToolButton::toggled, body, [header, body](bool expanded) {
			body->setVisible(expanded);
			header->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
		});

		const FieldRule *saved = rules.findField(field.id);

		row.defaultScene = new QComboBox();
		// "Scene" read as a peer of the four trigger rows rather than as the thing
		// they fall back to, and the fallback is the whole point of the row: the
		// common setup is one camera per field, set here once, with the triggers
		// left alone.
		auto *defaultLabel = new QLabel(tr("Default scene"));
		const QString fallbackHelp = tr("Used by every trigger below that is set to \"(use field scene)\". "
						"If this is \"(no switch)\" and a trigger has no scene of its own, "
						"that trigger does nothing.");
		defaultLabel->setToolTip(fallbackHelp);
		row.defaultScene->setToolTip(fallbackHelp);
		grid->addWidget(defaultLabel, 0, 0);
		grid->addWidget(row.defaultScene, 0, 1);
		populateSceneCombo(row.defaultScene, saved ? QString::fromStdString(saved->defaultScene) : QString());

		for (int t = 0; t < kTriggerCount; ++t) {
			auto *combo = new QComboBox();
			combo->setProperty("trigger", t);
			row.triggerScene[t] = combo;

			const QString label = QString::fromUtf8(triggerName(static_cast<Trigger>(t)));
			auto *triggerLabel = new QLabel(label);
			triggerLabel->setToolTip(fallbackHelp);
			combo->setToolTip(fallbackHelp);
			grid->addWidget(triggerLabel, t + 1, 0);
			grid->addWidget(combo, t + 1, 1);
			populateSceneCombo(combo, saved ? QString::fromStdString(saved->sceneFor[t]) : QString());
		}

		auto *timing = new QHBoxLayout();
		row.delay = makeSecondsBox();
		row.minHold = makeSecondsBox();
		// -1 in the model means "inherit". The UI says so in words instead, since
		// a spin box showing -1 reads as a bug.
		row.delay->setSpecialValueText(tr("default"));
		row.minHold->setSpecialValueText(tr("default"));
		row.delay->setMinimum(-1);
		row.minHold->setMinimum(-1);
		row.delay->setValue(saved && saved->delayMs >= 0 ? saved->delayMs / 1000 : -1);
		row.minHold->setValue(saved && saved->minHoldMs >= 0 ? saved->minHoldMs / 1000 : -1);
		timing->addWidget(new QLabel(tr("Delay")));
		timing->addWidget(row.delay);
		timing->addWidget(new QLabel(tr("Hold")));
		timing->addWidget(row.minHold);
		timing->addStretch(1);
		grid->addLayout(timing, kTriggerCount + 1, 0, 1, 2);

		// Ahead of the trailing stretch, so rows stack from the top.
		fieldsLayout_->insertWidget(fieldsLayout_->count() - 1, row.container);
		rows_.push_back(row);
	}

	if (fields.empty())
		appendLog(tr("TM reported no fields for this field set."), tmd::LogLevel::Warn);

	noFields_->setVisible(rows_.empty());
	if (rows_.empty() && !fields.empty())
		noFields_->setText(tr("TM reported no fields for this field set."));

	// A rebuild replaces every header, so the button would otherwise still claim
	// everything is collapsed while the new rows are all open.
	if (collapseAll_ != nullptr && collapseAll_->isChecked())
		setAllFieldsCollapsed(true);
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

	setConnectionState(tmd::LogLevel::Info, tr("Testing..."), tr("Asking Tournament Manager to answer."));

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
				reportConnection(remedy, levelFor(error));
				if (error != TmError::None) {
					// The remedy is for the operator; the detail is for
					// whoever they forward the log to, so it goes in plain
					// rather than competing with the coloured line above it.
					appendLog(QStringLiteral("%1 (%2)")
							  .arg(QString::fromUtf8(tmErrorName(error)), detailText),
						  tmd::LogLevel::Info);
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

				appendLog(tr("Connected. %1 field set(s).").arg(fieldSets.size()),
					  tmd::LogLevel::Good);
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
					reportConnection(remedy, levelFor(error));
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
		appendLog(tr("Could not write settings to %1").arg(QString::fromStdString(directorSettingsPath())),
			  tmd::LogLevel::Bad);

	director_->start();
}

void TmDirectorDock::onEnabledToggled(bool enabled)
{
	if (loading_)
		return;
	onSave();
	appendLog(enabled ? tr("Automation on") : tr("Automation off"), tmd::LogLevel::Info);
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

void TmDirectorDock::onActivity(const QString &line, tmd::LogLevel level)
{
	appendLog(line, level);
}

} // namespace tmd
