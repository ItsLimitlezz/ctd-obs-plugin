#include "show-section.hpp"

#include "brand.hpp"
#include "device-map-dialog.hpp"
#include "../show/show.h"
#include "../show/sync.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

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

/* user-visible text, from data/locale */
QString uiText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

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
		return uiText("Show.NameEmpty");
	if (name.size() > kMaxNameLength)
		return uiText("Show.NameTooLong").arg(kMaxNameLength);
	static const QString bad = QStringLiteral("/\\:*?\"<>|");
	for (const QChar c : name)
		if (bad.contains(c))
			return uiText("Show.NameBadChars").arg(bad);
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
		return uiText("Show.JustNow");
	if (secs < 3600)
		return uiText("Show.MinutesAgo").arg(secs / 60);
	if (secs < 86400)
		return uiText("Show.HoursAgo").arg(secs / 3600);
	if (secs < 7 * 86400)
		return uiText("Show.DaysAgo").arg(secs / 86400);
	return when.toLocalTime().toString("yyyy-MM-dd");
}

QString detailLine(const show_entry &e, const show_sync_entry *sync, bool connected)
{
	QStringList chips;
	if (e.active)
		chips << uiText("Show.Chip.Active");
	const show_sync_status status = sync ? sync->status : SHOW_SYNC_LOCAL_ONLY;
	if (status == SHOW_SYNC_CONFLICT)
		chips << uiText("Show.Chip.Conflict").arg(sync->remote_head);
	else if (e.dirty)
		chips << uiText("Show.Chip.NotBackedUp");
	if (sync && sync->queued)
		chips << uiText("Show.Chip.Waiting");
	if (sync && sync->update_ready)
		chips << uiText("Show.Chip.UpdateReady");
	else if (status == SHOW_SYNC_UPDATE_READY)
		chips << uiText("Show.Chip.Newer").arg(sync->remote_head);
	if (!connected && e.relay_profile_id && *e.relay_profile_id)
		chips << uiText("Show.Chip.RelayOffline");
	chips << (e.revision > 0 ? uiText("Show.Chip.Revision").arg(e.revision) : uiText("Show.Chip.LocalOnly"));
	if (e.needs_reentry > 0)
		chips << uiText("Show.Chip.NeedsReentry").arg(e.needs_reentry);
	if (e.saved_at && *e.saved_at)
		chips << uiText("Show.Chip.Saved").arg(relativeTime(QString::fromUtf8(e.saved_at)));
	return chips.join(QString::fromUtf8("  \xC2\xB7  "));
}

} // namespace

ShowProfilesSection::ShowProfilesSection(QWidget *parent) : QGroupBox(uiText("Show.Title"), parent)
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
	switch_ = new QPushButton(uiText("Show.Switch"), this);
	save_ = new QPushButton(uiText("Show.SaveAs"), this);
	update_ = new QPushButton(uiText("Show.Update"), this);
	delete_ = new QPushButton(uiText("Show.Delete"), this);
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
	saveUp_ = new QPushButton(uiText("Show.SaveUp"), this);
	updateNow_ = new QPushButton(uiText("Show.UpdateNow"), this);
	devices_ = new QPushButton(uiText("Show.Devices"), this);
	ctd::button(saveUp_);
	ctd::button(updateNow_);
	ctd::button(devices_);
	updateNow_->hide();
	sync->addWidget(saveUp_);
	sync->addWidget(updateNow_);
	sync->addWidget(devices_);
	layout->addLayout(sync);

	/* show profiles on CTD Relay that aren't on this machine yet */
	remoteTitle_ = new QLabel(uiText("Show.RemoteTitle"), this);
	remoteTitle_->setWordWrap(true);
	remoteTitle_->setProperty("ctdRole", "muted");
	remote_ = new QListWidget(this);
	remote_->setMinimumHeight(48);
	remote_->setMaximumHeight(110);
	wrapLines(remote_);
	pull_ = new QPushButton(uiText("Show.Pull"), this);
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
		ctd::setWarning(status_, s.message_is_error, "muted"); /* status_ is muted normally */
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
	const QString working = uiText("Show.Working");

	/* the reason is shown in the status line too, not only in a tooltip */
	QString line = status_->text();
	const QString liveLine = uiText("Show.LiveLine").arg(liveReason_);
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
			    : id.isEmpty() ? uiText("Show.Tip.SelectFirst")
			    : active       ? uiText("Show.Tip.Reload")
					   : QString());
	save_->setEnabled(idle);
	save_->setToolTip(idle ? QString() : working);
	update_->setEnabled(idle && active);
	update_->setToolTip(!idle          ? working
			    : id.isEmpty() ? uiText("Show.Tip.SelectActive")
			    : !active      ? uiText("Show.Tip.OnlyActive")
					   : QString());
	const auto syncFound = sync_.constFind(id);
	const bool linked = syncFound != sync_.constEnd() && syncFound->status != SHOW_SYNC_LOCAL_ONLY;
	saveUp_->setEnabled(idle && !id.isEmpty() && relayConnected_ && (linked || !relayProfileName_.isEmpty()));
	saveUp_->setToolTip(!idle                         ? working
			    : id.isEmpty()                ? uiText("Show.Tip.SelectFirst")
			    : !relayConnected_            ? uiText("Show.Tip.ConnectRelay")
			    : linked                      ? uiText("Show.Tip.BackUp")
			    : relayProfileName_.isEmpty() ? uiText("Show.Tip.PickRelayProfile")
							  : uiText("Show.Tip.Links").arg(relayProfileName_));
	const auto activeSync = sync_.constFind(activeId_);
	const bool ready = activeSync != sync_.constEnd() && activeSync->update_ready;
	updateNow_->setVisible(ready);
	updateNow_->setEnabled(idle && !live_);
	updateNow_->setToolTip(live_ ? liveReason_ : QString());
	devices_->setEnabled(!activeId_.isEmpty());
	devices_->setToolTip(activeId_.isEmpty() ? uiText("Show.Tip.SwitchFirst") : QString());
	pull_->setEnabled(idle && relayConnected_ && remote_->currentItem() && remote_->currentItem()->isSelected());
	delete_->setEnabled(idle && !id.isEmpty() && !active);
	delete_->setToolTip(!idle          ? working
			    : id.isEmpty() ? uiText("Show.Tip.SelectFirst")
			    : active       ? uiText("Show.Tip.ActiveNoDelete")
					   : QString());
}

void ShowProfilesSection::askSwitch()
{
	const QString id = selectedId();
	if (id.isEmpty())
		return;
	const auto answer = QMessageBox::question(this, uiText("Common.DialogTitle"),
						  uiText("Show.Ask.Switch").arg(selectedName()));
	if (answer != QMessageBox::Yes)
		return;
	if (!show_switch(id.toUtf8().constData(), 0))
		refresh(); /* the status line says why */
}

void ShowProfilesSection::askSave()
{
	bool ok = false;
	QString name = QInputDialog::getText(this, uiText("Common.DialogTitle"), uiText("Show.Ask.Name"),
					     QLineEdit::Normal, QString(), &ok);
	while (ok) {
		name = name.trimmed();
		const QString problem = nameProblem(name);
		if (problem.isEmpty())
			break;
		QMessageBox::warning(this, uiText("Common.DialogTitle"), problem);
		name = QInputDialog::getText(this, uiText("Common.DialogTitle"), uiText("Show.Ask.Name"),
					     QLineEdit::Normal, name, &ok);
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
	const auto answer = QMessageBox::question(this, uiText("Common.DialogTitle"),
						  uiText("Show.Ask.Update").arg(selectedName()));
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
	const auto answer = QMessageBox::question(this, uiText("Common.DialogTitle"),
						  uiText("Show.Ask.Delete").arg(selectedName()));
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
			progress_->setFormat(uiText("Show.Progress.Megabytes")
						     .arg(step)
						     .arg((double)sync.done / 1048576.0, 0, 'f', 1)
						     .arg((double)sync.total / 1048576.0, 0, 'f', 1));
		else
			progress_->setFormat(uiText("Show.Progress.Count").arg(step).arg(sync.done).arg(sync.total));
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
		QString line = QString::fromUtf8(r.name) + "\n" + uiText("Show.Chip.Revision").arg(r.head);
		if (r.saved_by && *r.saved_by)
			line += QString::fromUtf8("  \xC2\xB7  ") +
				uiText("Show.Remote.SavedOn").arg(QString::fromUtf8(r.saved_by));
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
	QString text = uiText(id == activeId_ ? "Show.Ask.SaveUpActive" : "Show.Ask.SaveUp");
	text = text.arg(selectedName());
	if (!linked)
		text += "\n\n" + uiText("Show.Ask.SaveUpLink").arg(relayProfileName_);
	if (QMessageBox::question(this, uiText("Common.DialogTitle"), text) != QMessageBox::Yes)
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
	QMessageBox box(QMessageBox::Question, uiText("Common.DialogTitle"), uiText("Show.Ask.Pull").arg(name),
			QMessageBox::NoButton, this);
	QPushButton *download = box.addButton(uiText("Show.Download"), QMessageBox::AcceptRole);
	QPushButton *andSwitch = live_ ? nullptr
				       : box.addButton(uiText("Show.DownloadAndSwitch"), QMessageBox::AcceptRole);
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
	const auto answer = QMessageBox::question(this, uiText("Common.DialogTitle"), uiText("Show.Ask.UpdateNow"));
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
	box.setWindowTitle(uiText("Common.DialogTitle"));
	box.setText(uiText("Show.Conflict.Text")
			    .arg(name, by.isEmpty() ? QString() : " " + uiText("Show.Conflict.From").arg(by))
			    .arg(head));
	box.setInformativeText(uiText("Show.Conflict.Info"));
	QPushButton *mine = box.addButton(uiText("Show.Conflict.KeepMine"), QMessageBox::AcceptRole);
	QPushButton *theirs = box.addButton(uiText("Show.Conflict.TakeTheirs"), QMessageBox::AcceptRole);
	QPushButton *asNew = box.addButton(uiText("Show.Conflict.SaveAsNew"), QMessageBox::AcceptRole);
	QPushButton *later = box.addButton(uiText("Show.Conflict.Later"), QMessageBox::RejectRole);
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
