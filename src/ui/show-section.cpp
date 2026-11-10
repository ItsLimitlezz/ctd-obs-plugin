#include "show-section.hpp"

#include "brand.hpp"
#include "device-map-dialog.hpp"
#include "../show/show.h"
#include "../show/sync.h"

#include <obs-frontend-api.h>

#include <QDateTime>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {

constexpr int kMaxNameLength = 64;

/* the same wrapping as the dock's other lists */
void wrapLines(QListWidget *list)
{
	list->setWordWrap(true);
	list->setResizeMode(QListView::Adjust);
	list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
}

/* "" when the name is fine, else why not */
QString nameProblem(const QString &name)
{
	if (name.isEmpty())
		return "Give the show profile a name.";
	if (name.size() > kMaxNameLength)
		return QString("Use %1 characters or fewer.").arg(kMaxNameLength);
	static const QString bad = QStringLiteral("/\\:*?\"<>|");
	for (const QChar c : name)
		if (bad.contains(c))
			return QString("A name can't contain any of these: %1").arg(bad);
	return QString();
}

QString relativeTime(const QString &iso)
{
	const QDateTime when = QDateTime::fromString(iso, Qt::ISODate);
	if (!when.isValid())
		return iso;
	const qint64 secs = when.secsTo(QDateTime::currentDateTime());
	if (secs < 0)
		return when.toLocalTime().toString(Qt::ISODate);
	if (secs < 60)
		return "just now";
	if (secs < 3600)
		return QString("%1 min ago").arg(secs / 60);
	if (secs < 86400)
		return QString("%1 h ago").arg(secs / 3600);
	if (secs < 7 * 86400)
		return QString("%1 d ago").arg(secs / 86400);
	return when.toLocalTime().toString("yyyy-MM-dd");
}

QString detailLine(const show_entry &e, const show_sync_entry *sync, bool connected)
{
	QStringList chips;
	if (e.active)
		chips << "Active";
	const show_sync_status status = sync ? sync->status : SHOW_SYNC_LOCAL_ONLY;
	if (status == SHOW_SYNC_CONFLICT)
		chips << QString("Changed on Relay (rev %1)").arg(sync->remote_head);
	else if (e.dirty)
		chips << "Not backed up";
	if (sync && sync->queued)
		chips << "Waiting for Relay";
	if (sync && sync->update_ready)
		chips << "Update ready";
	else if (status == SHOW_SYNC_UPDATE_READY)
		chips << QString("Newer on Relay (rev %1)").arg(sync->remote_head);
	if (!connected && e.relay_profile_id && *e.relay_profile_id)
		chips << "Relay offline \xE2\x80\x94 local copy";
	chips << (e.revision > 0 ? QString("rev %1").arg(e.revision) : QString("Local only"));
	if (e.needs_reentry > 0)
		chips << QString("%1 needs re-entry").arg(e.needs_reentry);
	if (e.saved_at && *e.saved_at)
		chips << QString("Saved %1").arg(relativeTime(QString::fromUtf8(e.saved_at)));
	return chips.join(QString::fromUtf8("  \xC2\xB7  "));
}

} // namespace

ShowProfilesSection::ShowProfilesSection(QWidget *parent) : QGroupBox("Show Profiles", parent)
{
	auto *layout = new QVBoxLayout(this);
	layout->setSpacing(8);

	status_ = new QLabel(this);
	status_->setWordWrap(true);
	status_->setProperty("ctdRole", "muted");
	layout->addWidget(status_);

	list_ = new QListWidget(this);
	list_->setMinimumHeight(90);
	wrapLines(list_);
	layout->addWidget(list_, 1);

	/* stacked, not side by side, so the dock stays narrow enough for a side dock area */
	auto *buttons = new QVBoxLayout();
	buttons->setSpacing(6);
	switch_ = new QPushButton("Switch to selected", this);
	save_ = new QPushButton("Save current setup as\xE2\x80\xA6", this);
	update_ = new QPushButton("Update selected from current setup", this);
	delete_ = new QPushButton("Delete selected", this);
	ctd::button(switch_, ctd::Role::Primary);
	ctd::button(save_);
	ctd::button(update_);
	ctd::button(delete_);
	/* room for the primary button's 3 px offset shadow */
	buttons->addSpacing(4);
	buttons->addWidget(switch_);
	buttons->addSpacing(3);
	buttons->addWidget(save_);
	buttons->addWidget(update_);
	buttons->addWidget(delete_);
	layout->addLayout(buttons);

	/* M3: CTD Relay sync (design section 4) */
	progress_ = new QProgressBar(this);
	progress_->setTextVisible(true);
	progress_->setMaximumHeight(18);
	progress_->hide();
	layout->addWidget(progress_);
	auto *sync = new QVBoxLayout();
	sync->setSpacing(6);
	saveUp_ = new QPushButton("Save selected to CTD Relay", this);
	updateNow_ = new QPushButton("Update now (apply the downloaded revision)", this);
	devices_ = new QPushButton("Devices\xE2\x80\xA6", this);
	ctd::button(saveUp_);
	ctd::button(updateNow_);
	ctd::button(devices_);
	updateNow_->hide();
	sync->addWidget(saveUp_);
	sync->addWidget(updateNow_);
	sync->addWidget(devices_);
	layout->addLayout(sync);

	/* show profiles on CTD Relay that aren't on this machine yet */
	remoteTitle_ = new QLabel("On CTD Relay, not on this machine", this);
	remoteTitle_->setWordWrap(true);
	remoteTitle_->setProperty("ctdRole", "muted");
	remote_ = new QListWidget(this);
	remote_->setMinimumHeight(48);
	remote_->setMaximumHeight(110);
	wrapLines(remote_);
	pull_ = new QPushButton("Download selected from CTD Relay", this);
	ctd::button(pull_);
	layout->addWidget(remoteTitle_);
	layout->addWidget(remote_);
	layout->addWidget(pull_);
	remoteTitle_->hide();
	remote_->hide();
	pull_->hide();

	ctd::eyebrowTitle(this);

	QObject::connect(list_, &QListWidget::itemSelectionChanged, this, [this] { updateButtons(); });
	QObject::connect(switch_, &QPushButton::clicked, this, [this] { askSwitch(); });
	QObject::connect(save_, &QPushButton::clicked, this, [this] { askSave(); });
	QObject::connect(update_, &QPushButton::clicked, this, [this] { askUpdate(); });
	QObject::connect(delete_, &QPushButton::clicked, this, [this] { askDelete(); });
	QObject::connect(saveUp_, &QPushButton::clicked, this, [this] { askSaveUp(); });
	QObject::connect(updateNow_, &QPushButton::clicked, this, [this] { askUpdateNow(); });
	QObject::connect(devices_, &QPushButton::clicked, this, [this] { openDevices(); });
	QObject::connect(pull_, &QPushButton::clicked, this, [this] { askPull(); });
	QObject::connect(remote_, &QListWidget::itemSelectionChanged, this, [this] { updateButtons(); });

	/* M4: the "Fix mapping" dialog opens itself after a switch when a device needs a look */
	ctd_device_map_dialog_install(static_cast<QWidget *>(obs_frontend_get_main_window()));

	hide(); /* until the first refresh says CTD OBS is here */
}

QString ShowProfilesSection::selectedId() const
{
	const QListWidgetItem *item = list_->currentItem();
	return item && item->isSelected() ? item->data(Qt::UserRole).toString() : QString();
}

QString ShowProfilesSection::selectedName() const
{
	const QListWidgetItem *item = list_->currentItem();
	return item ? item->data(Qt::UserRole + 1).toString() : QString();
}

void ShowProfilesSection::refresh()
{
	struct show_state s;
	show_state_get(&s);

	if (!s.available) {
		setVisible(false);
		show_state_free(&s);
		return;
	}

	busy_ = s.busy;
	live_ = s.live;
	liveReason_ = QString::fromUtf8(s.live_reason);
	activeId_ = QString::fromUtf8(s.active_id);

	struct show_sync_state sync;
	show_sync_state_get(&sync);
	const bool syncChanged = sync.seq != lastSyncSeq_;
	if (syncChanged) {
		lastSyncSeq_ = sync.seq;
		sync_.clear();
		for (size_t i = 0; i < sync.count; i++)
			sync_.insert(QString::fromUtf8(sync.entries[i].id), sync.entries[i]);
		relayConnected_ = sync.relay_connected;
		relayProfileName_ = QString::fromUtf8(sync.relay_profile_name);
		showProgress(sync);
		showRemotes(sync);
	}
	if (sync.conflict && sync.conflict_seq != lastConflictSeq_ && !conflictOpen_) {
		lastConflictSeq_ = sync.conflict_seq;
		QTimer::singleShot(0, this, [this] { askConflict(); });
	}
	show_sync_state_free(&sync);

	if (s.seq != lastSeq_ || syncChanged) {
		lastSeq_ = s.seq;
		const QString keep = selectedId();
		{
			const QSignalBlocker block(list_);
			list_->clear();
			for (size_t i = 0; i < s.count; i++) {
				const show_entry &e = s.entries[i];
				const QString name = QString::fromUtf8(e.name);
				const QString id = QString::fromUtf8(e.id);
				const auto found = sync_.constFind(id);
				const show_sync_entry *se = found == sync_.constEnd() ? nullptr : &found.value();
				auto *item =
					new QListWidgetItem(name + "\n" + detailLine(e, se, relayConnected_), list_);
				item->setData(Qt::UserRole, QString::fromUtf8(e.id));
				item->setData(Qt::UserRole + 1, name);
				if (!keep.isEmpty() && keep == QString::fromUtf8(e.id)) {
					list_->setCurrentItem(item);
					item->setSelected(true);
				}
			}
		}

		QString message = QString::fromUtf8(s.message);
		/* what the user should know from the last action: missing files, keys to re-enter, encoder fallback */
		for (size_t i = 0; i < cj_size(s.warnings); i++) {
			const char *w = cj_str(cj_at(s.warnings, i));
			if (w)
				message += (message.isEmpty() ? QString() : QString("\n")) +
					   QString::fromUtf8("\xE2\x80\xA2 ") + QString::fromUtf8(w);
		}
		if (s.message_is_error)
			status_->setStyleSheet("color: #e5a50a; font-weight: bold;");
		else
			status_->setStyleSheet(QString());
		status_->setText(message);
	}

	setVisible(true);
	updateButtons();
	show_state_free(&s);
}

void ShowProfilesSection::updateButtons()
{
	const bool idle = busy_ == SHOW_IDLE;
	const QString id = selectedId();
	const bool active = !id.isEmpty() && id == activeId_;
	const QString working = "Working\xE2\x80\xA6";

	/* the reason is shown in the status line too, not only in a tooltip */
	QString line = status_->text();
	const QString liveLine = QString("Switching is off while live: %1").arg(liveReason_);
	const QString marker = "\n" + liveLine;
	if (line.endsWith(marker))
		line.chop(marker.size());
	else if (line == liveLine)
		line.clear();
	if (live_)
		line += (line.isEmpty() ? QString() : QString("\n")) + liveLine;
	if (line != status_->text())
		status_->setText(line);
	status_->setVisible(!line.isEmpty());

	switch_->setEnabled(idle && !live_ && !id.isEmpty());
	switch_->setToolTip(!idle          ? working
			    : live_        ? liveReason_
			    : id.isEmpty() ? QString("Select a show profile first")
			    : active       ? QString("Reload this show profile")
					   : QString());
	save_->setEnabled(idle);
	save_->setToolTip(idle ? QString() : working);
	update_->setEnabled(idle && active);
	update_->setToolTip(!idle          ? working
			    : id.isEmpty() ? QString("Select the active show profile first")
			    : !active      ? QString("Only the active show profile can be updated; switch to it first")
					   : QString());
	const auto syncFound = sync_.constFind(id);
	const bool linked = syncFound != sync_.constEnd() && syncFound->status != SHOW_SYNC_LOCAL_ONLY;
	saveUp_->setEnabled(idle && !id.isEmpty() && relayConnected_ && (linked || !relayProfileName_.isEmpty()));
	saveUp_->setToolTip(!idle              ? working
			    : id.isEmpty()     ? QString("Select a show profile first")
			    : !relayConnected_ ? QString("Connect the dock to CTD Relay first")
			    : linked           ? QString("Back up the newest saved revision to CTD Relay")
			    : relayProfileName_.isEmpty()
				    ? QString("Pick a CTD Relay profile above first")
				    : QString("Links it to the CTD Relay profile %1").arg(relayProfileName_));
	const auto activeSync = sync_.constFind(activeId_);
	const bool ready = activeSync != sync_.constEnd() && activeSync->update_ready;
	updateNow_->setVisible(ready);
	updateNow_->setEnabled(idle && !live_);
	updateNow_->setToolTip(live_ ? liveReason_ : QString());
	devices_->setEnabled(!activeId_.isEmpty());
	devices_->setToolTip(activeId_.isEmpty() ? QString("Switch to a show profile first") : QString());
	pull_->setEnabled(idle && relayConnected_ && remote_->currentItem() && remote_->currentItem()->isSelected());
	delete_->setEnabled(idle && !id.isEmpty() && !active);
	delete_->setToolTip(!idle          ? working
			    : id.isEmpty() ? QString("Select a show profile first")
			    : active       ? QString("The active show profile can't be deleted")
					   : QString());
}

void ShowProfilesSection::askSwitch()
{
	const QString id = selectedId();
	if (id.isEmpty())
		return;
	const auto answer = QMessageBox::question(
		this, "CTD Relay",
		QString("Switch to %1?\n\nOBS loads its scenes and settings; the current ones stay saved.")
			.arg(selectedName()));
	if (answer != QMessageBox::Yes)
		return;
	if (!show_switch(id.toUtf8().constData(), 0))
		refresh(); /* the status line says why */
}

void ShowProfilesSection::askSave()
{
	bool ok = false;
	QString name = QInputDialog::getText(this, "CTD Relay", "Name for this show profile:", QLineEdit::Normal,
					     QString(), &ok);
	while (ok) {
		name = name.trimmed();
		const QString problem = nameProblem(name);
		if (problem.isEmpty())
			break;
		QMessageBox::warning(this, "CTD Relay", problem);
		name = QInputDialog::getText(this, "CTD Relay", "Name for this show profile:", QLineEdit::Normal, name,
					     &ok);
	}
	if (!ok)
		return;
	if (!show_save_current_as(name.toUtf8().constData()))
		refresh(); /* the status line says why */
}

void ShowProfilesSection::askUpdate()
{
	const QString id = selectedId();
	if (id.isEmpty() || id != activeId_)
		return;
	const auto answer = QMessageBox::question(
		this, "CTD Relay", QString("Save OBS's current setup as a new revision of %1?").arg(selectedName()));
	if (answer != QMessageBox::Yes)
		return;
	if (!show_update(id.toUtf8().constData()))
		refresh(); /* the status line says why */
}

void ShowProfilesSection::askDelete()
{
	const QString id = selectedId();
	if (id.isEmpty() || id == activeId_)
		return;
	const auto answer = QMessageBox::question(
		this, "CTD Relay",
		QString("Delete %1?\n\nThis removes its saved copy here. OBS's own scene collection and profile "
			"files are not deleted.")
			.arg(selectedName()));
	if (answer != QMessageBox::Yes)
		return;
	if (!show_delete(id.toUtf8().constData()))
		refresh(); /* the status line says why */
}

/* ------------------------------------------------------- M3: CTD Relay sync --- */

void ShowProfilesSection::showProgress(const show_sync_state &sync)
{
	const QString step = QString::fromUtf8(sync.step);
	if (step.isEmpty()) {
		progress_->hide();
		return;
	}
	if (sync.total == 0) {
		progress_->setRange(0, 0); /* busy */
		progress_->setFormat(step + QString::fromUtf8("\xE2\x80\xA6"));
	} else {
		const double fraction = (double)sync.done / (double)sync.total;
		progress_->setRange(0, 1000);
		progress_->setValue((int)(fraction > 1.0 ? 1000 : fraction * 1000));
		if (sync.total > 100000)
			progress_->setFormat(QString("%1  %2 / %3 MB")
						     .arg(step)
						     .arg((double)sync.done / 1048576.0, 0, 'f', 1)
						     .arg((double)sync.total / 1048576.0, 0, 'f', 1));
		else
			progress_->setFormat(QString("%1  %2 of %3").arg(step).arg(sync.done).arg(sync.total));
	}
	progress_->show();
}

void ShowProfilesSection::showRemotes(const show_sync_state &sync)
{
	const QString keep = remote_->currentItem() && remote_->currentItem()->isSelected()
				     ? remote_->currentItem()->data(Qt::UserRole).toString()
				     : QString();
	const QSignalBlocker block(remote_);
	remote_->clear();
	for (size_t i = 0; i < sync.remote_count; i++) {
		const show_sync_remote &r = sync.remotes[i];
		if (r.local_id[0])
			continue;
		QString line = QString::fromUtf8(r.name) + "\n" + QString("rev %1").arg(r.head);
		if (r.saved_by && *r.saved_by)
			line += QString::fromUtf8("  \xC2\xB7  saved on ") + QString::fromUtf8(r.saved_by);
		auto *item = new QListWidgetItem(line, remote_);
		item->setData(Qt::UserRole, QString::fromUtf8(r.relay_profile_id));
		item->setData(Qt::UserRole + 1, QString::fromUtf8(r.name));
		if (!keep.isEmpty() && keep == QString::fromUtf8(r.relay_profile_id)) {
			remote_->setCurrentItem(item);
			item->setSelected(true);
		}
	}
	const bool any = remote_->count() > 0;
	remoteTitle_->setVisible(any);
	remote_->setVisible(any);
	pull_->setVisible(any);
}

void ShowProfilesSection::askSaveUp()
{
	const QString id = selectedId();
	if (id.isEmpty())
		return;
	const auto found = sync_.constFind(id);
	const bool linked = found != sync_.constEnd() && found->status != SHOW_SYNC_LOCAL_ONLY;
	QString text = id == activeId_ ? QString("Save the current setup of %1 and back it up to CTD Relay?")
				       : QString("Back up the newest saved revision of %1 to CTD Relay?");
	text = text.arg(selectedName());
	if (!linked)
		text += QString("\n\nIt is linked to the CTD Relay profile %1: other machines can then download it.")
				.arg(relayProfileName_);
	if (QMessageBox::question(this, "CTD Relay", text) != QMessageBox::Yes)
		return;
	if (!show_sync_save_up(id.toUtf8().constData()))
		refresh(); /* the status line says why */
}

void ShowProfilesSection::askPull()
{
	const QListWidgetItem *item = remote_->currentItem();
	if (!item || !item->isSelected())
		return;
	const QString relay = item->data(Qt::UserRole).toString();
	const QString name = item->data(Qt::UserRole + 1).toString();
	QMessageBox box(QMessageBox::Question, "CTD Relay",
			QString("Download %1's show profile to this machine?").arg(name), QMessageBox::NoButton, this);
	QPushButton *download = box.addButton("Download", QMessageBox::AcceptRole);
	QPushButton *andSwitch = live_ ? nullptr : box.addButton("Download and switch", QMessageBox::AcceptRole);
	box.addButton(QMessageBox::Cancel);
	box.exec();
	const QAbstractButton *clicked = box.clickedButton();
	if (clicked != download && (!andSwitch || clicked != andSwitch))
		return;
	if (!show_sync_pull(relay.toUtf8().constData(), andSwitch && clicked == andSwitch))
		refresh();
}

void ShowProfilesSection::askUpdateNow()
{
	if (activeId_.isEmpty())
		return;
	const auto answer = QMessageBox::question(
		this, "CTD Relay",
		"Apply the revision downloaded from CTD Relay now?\n\nOBS reloads this show profile's scenes and settings.");
	if (answer != QMessageBox::Yes)
		return;
	if (!show_switch(activeId_.toUtf8().constData(), 0))
		refresh();
}

void ShowProfilesSection::openDevices()
{
	if (!activeId_.isEmpty())
		ctd_device_map_dialog_open(static_cast<QWidget *>(obs_frontend_get_main_window()),
					   activeId_.toUtf8().constData());
}

void ShowProfilesSection::askConflict()
{
	struct show_sync_state sync;
	show_sync_state_get(&sync);
	if (!sync.conflict) {
		show_sync_state_free(&sync);
		return;
	}
	const QString name = QString::fromUtf8(sync.conflict_name);
	const QString by = QString::fromUtf8(sync.conflict_by);
	const qint64 head = sync.conflict_head;
	show_sync_state_free(&sync);

	conflictOpen_ = true;
	QMessageBox box(this);
	box.setIcon(QMessageBox::Warning);
	box.setWindowTitle("CTD Relay");
	box.setText(QString("%1 changed on CTD Relay%2 (revision %3) since this machine's copy.")
			    .arg(name, by.isEmpty() ? QString() : QString(" from %1").arg(by))
			    .arg(head));
	box.setInformativeText("Nothing is lost whichever you pick: every revision stays in CTD Relay's history.\n\n"
			       "Keep mine: your version becomes the newest on CTD Relay.\n"
			       "Take theirs: this machine gets theirs; yours stays as an older local revision.\n"
			       "Save as new profile: yours becomes a separate local show profile, and this one takes "
			       "theirs.");
	QPushButton *mine = box.addButton("Keep mine", QMessageBox::AcceptRole);
	QPushButton *theirs = box.addButton("Take theirs", QMessageBox::AcceptRole);
	QPushButton *asNew = box.addButton("Save as new profile", QMessageBox::AcceptRole);
	QPushButton *later = box.addButton("Decide later", QMessageBox::RejectRole);
	box.setDefaultButton(later);
	box.exec();
	conflictOpen_ = false;
	const QAbstractButton *clicked = box.clickedButton();
	bool ok = true;
	if (clicked == mine)
		ok = show_sync_resolve(SHOW_CONFLICT_KEEP_MINE);
	else if (clicked == theirs)
		ok = show_sync_resolve(SHOW_CONFLICT_TAKE_THEIRS);
	else if (clicked == asNew)
		ok = show_sync_resolve(SHOW_CONFLICT_SAVE_AS_NEW);
	else
		show_sync_dismiss_conflict();
	if (!ok)
		refresh();
}
