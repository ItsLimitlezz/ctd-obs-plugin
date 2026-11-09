#include "show-section.hpp"

#include "brand.hpp"
#include "../show/show.h"

#include <QDateTime>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
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

QString detailLine(const show_entry &e)
{
	QStringList chips;
	if (e.active)
		chips << "Active";
	if (e.dirty)
		chips << "Not backed up";
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

	ctd::eyebrowTitle(this);

	QObject::connect(list_, &QListWidget::itemSelectionChanged, this, [this] { updateButtons(); });
	QObject::connect(switch_, &QPushButton::clicked, this, [this] { askSwitch(); });
	QObject::connect(save_, &QPushButton::clicked, this, [this] { askSave(); });
	QObject::connect(update_, &QPushButton::clicked, this, [this] { askUpdate(); });
	QObject::connect(delete_, &QPushButton::clicked, this, [this] { askDelete(); });

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

	if (s.seq != lastSeq_) {
		lastSeq_ = s.seq;
		const QString keep = selectedId();
		{
			const QSignalBlocker block(list_);
			list_->clear();
			for (size_t i = 0; i < s.count; i++) {
				const show_entry &e = s.entries[i];
				const QString name = QString::fromUtf8(e.name);
				auto *item = new QListWidgetItem(name + "\n" + detailLine(e), list_);
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
