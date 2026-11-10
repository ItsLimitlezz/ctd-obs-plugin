/*
 * The "Fix mapping" dialog (T13, design section 5). One card per device slot: the source's name, the device the
 * profile was saved with, a status chip and a dropdown of this machine's devices. Choosing a device applies it to the
 * live source at once (show_devices_choose); there is no OK button. Laid out to wrap, so it works in a narrow dock.
 */

#include "device-map-dialog.hpp"

#include "brand.hpp"
#include "../show/device-map.h"

#include <QComboBox>
#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStandardItemModel>
#include <QVBoxLayout>

namespace {

QString qs(const char *value)
{
	return QString::fromUtf8(value ? value : "");
}

/* the amber the dock already uses for warnings */
QColor amber()
{
	return QColor(0xe5, 0xa5, 0x0a);
}

void styleChip(QLabel *chip, const QString &text, const QColor &color)
{
	chip->setText(text);
	chip->setStyleSheet(
		QString("color: %1; border: 1px solid %1; border-radius: 3px; padding: 1px 6px;").arg(color.name()));
}

class DeviceMapDialog : public QDialog {
public:
	DeviceMapDialog(const QString &showId, QWidget *parent) : QDialog(parent), showId_(showId)
	{
		setAttribute(Qt::WA_DeleteOnClose);
		setWindowTitle("Devices");
		ctd::applyTheme(this);
		setMinimumSize(300, 240);
		resize(460, 420);

		auto *outer = new QVBoxLayout(this);
		title_ = new QLabel(this);
		title_->setFont(ctd::font(ctd::Face::Heading, 16, QFont::Bold, true, 0.6));
		title_->setProperty("ctdRole", "title");
		title_->setWordWrap(true);
		outer->addWidget(title_);

		auto *intro = new QLabel(
			"Pick which of this machine's devices each source uses. Saved for this machine only.", this);
		intro->setWordWrap(true);
		intro->setProperty("ctdRole", "muted");
		outer->addWidget(intro);

		scroll_ = new QScrollArea(this);
		scroll_->setObjectName("ctdScroll");
		scroll_->viewport()->setObjectName("ctdScrollViewport");
		scroll_->setFrameShape(QFrame::NoFrame);
		scroll_->setWidgetResizable(true);
		scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		outer->addWidget(scroll_, 1);

		status_ = new QLabel(this);
		status_->setWordWrap(true);
		status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
		outer->addWidget(status_);

		auto *buttons = new QHBoxLayout();
		auto *refresh = new QPushButton("Refresh", this);
		auto *close = new QPushButton("Close", this);
		ctd::button(refresh);
		ctd::button(close);
		buttons->addWidget(refresh);
		buttons->addStretch(1);
		buttons->addWidget(close);
		outer->addLayout(buttons);

		connect(refresh, &QPushButton::clicked, this, [this] { reload(); });
		connect(close, &QPushButton::clicked, this, &QDialog::close);
		reload();
	}

	const QString &showId() const { return showId_; }

	/* re-lists the slots and this machine's devices (a camera just plugged in) */
	void reload()
	{
		setStatus(QString(), false);
		show_device_rows_holder rows;
		auto *body = new QWidget();
		body->setObjectName("ctdBody");
		auto *list = new QVBoxLayout(body);
		list->setContentsMargins(0, 0, 4, 0);
		list->setSpacing(8);

		if (!show_devices_get(showId_.toUtf8().constData(), &rows.data)) {
			title_->setText("Devices");
			auto *none = new QLabel("This show profile isn't active, or it has no devices to map.", body);
			none->setWordWrap(true);
			list->addWidget(none);
		} else {
			rows.valid = true;
			title_->setText(QString::fromUtf8("Devices \xE2\x80\x94 %1").arg(qs(rows.data.profile_name)));
			setWindowTitle(title_->text());
			for (size_t i = 0; i < rows.data.count; i++)
				list->addWidget(makeRow(rows.data.rows[i], body));
		}
		list->addStretch(1);
		scroll_->setWidget(body); /* deletes the previous body */
	}

private:
	struct show_device_rows_holder {
		show_device_rows data{};
		bool valid = false;
		~show_device_rows_holder()
		{
			if (valid)
				show_devices_free(&data);
		}
	};

	QWidget *makeRow(const show_device_row &row, QWidget *parent)
	{
		auto *card = new QFrame(parent);
		card->setObjectName("ctdDeviceCard");
		card->setStyleSheet(
			QString("#ctdDeviceCard { background: %1; border: 1px solid %2; border-radius: 4px; }")
				.arg(ctd::surface().name(), ctd::line().name()));
		auto *v = new QVBoxLayout(card);
		v->setContentsMargins(10, 8, 10, 8);
		v->setSpacing(4);

		QString name = qs(row.source_name);
		const bool gone = name.isEmpty();
		if (gone)
			name = qs(row.label);
		if (gone)
			name += " (source deleted)";
		auto *nameLabel = new QLabel(name, card);
		nameLabel->setFont(ctd::font(ctd::Face::Mono, 12, QFont::Bold));
		nameLabel->setWordWrap(true);
		v->addWidget(nameLabel);

		if (*row.was) {
			auto *was = new QLabel("Was: " + qs(row.was), card);
			was->setWordWrap(true);
			was->setProperty("ctdRole", "muted");
			was->setFont(ctd::font(ctd::Face::Mono, 11));
			v->addWidget(was);
		}

		auto *chip = new QLabel(card);
		chip->setFont(ctd::font(ctd::Face::Mono, 10, QFont::Bold));
		const QString status = qs(row.status);
		if (row.remembered_missing)
			styleChip(chip, QString::fromUtf8("\xE2\x9C\x97 remembered device unplugged"), ctd::tallyRed());
		else if (status == "auto")
			styleChip(chip, QString::fromUtf8("\xE2\x9C\x93 auto"), ctd::cyan());
		else if (status == "remembered")
			styleChip(chip, QString::fromUtf8("\xE2\x9C\x93 remembered"), ctd::cyan());
		else if (status == "suggested")
			styleChip(chip, "? suggested", amber());
		else
			styleChip(chip, QString::fromUtf8("\xE2\x9C\x97 missing"), ctd::tallyRed());
		chip->setWordWrap(true);
		chip->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Minimum);
		v->addWidget(chip, 0, Qt::AlignLeft);

		auto *combo = new QComboBox(card);
		combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		combo->setMinimumContentsLength(12);
		combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		combo->addItem(QString::fromUtf8("\xE2\x80\x94 No device \xE2\x80\x94"), QString());
		const QString currentId = qs(row.current_id);
		int select = currentId.isEmpty() ? 0 : -1;
		for (size_t i = 0; i < row.option_count; i++) {
			const QString id = qs(row.options[i].id), label = qs(row.options[i].name);
			combo->addItem(label.isEmpty() ? id : label, id);
			combo->setItemData(combo->count() - 1, label.isEmpty() ? id : label, Qt::ToolTipRole);
			if (id == currentId)
				select = combo->count() - 1;
		}
		if (select < 0) {
			QString shown = qs(row.current_name);
			if (shown.isEmpty())
				shown = currentId;
			combo->addItem(shown + " (not connected)", currentId);
			select = combo->count() - 1;
			if (auto *model = qobject_cast<QStandardItemModel *>(combo->model()))
				model->item(select)->setEnabled(false);
		}
		combo->setCurrentIndex(select);
		combo->setToolTip(combo->currentText());
		v->addWidget(combo);

		const QString slot = qs(row.slot);
		QPointer<QLabel> chipPtr(chip);
		connect(combo, &QComboBox::activated, this, [this, combo, slot, chipPtr](int index) {
			const QString id = combo->itemData(index).toString();
			const QString deviceName = id.isEmpty() ? QString() : combo->itemText(index);
			char err[256] = {0};
			const bool ok = show_devices_choose(showId_.toUtf8().constData(), slot.toUtf8().constData(),
							    id.toUtf8().constData(), deviceName.toUtf8().constData(),
							    err, sizeof(err));
			if (ok) {
				if (chipPtr)
					styleChip(chipPtr, QString::fromUtf8("\xE2\x9C\x93 Set"), ctd::cyan());
				combo->setToolTip(combo->currentText());
				setStatus(QString(), false);
			} else {
				setStatus(QString::fromUtf8(err[0] ? err : "Couldn't change the device."), true);
			}
		});
		return card;
	}

	void setStatus(const QString &text, bool error)
	{
		status_->setText(text);
		status_->setStyleSheet(error ? QString("color: %1;").arg(ctd::tallyRed().name()) : QString());
	}

	QString showId_;
	QLabel *title_ = nullptr;
	QLabel *status_ = nullptr;
	QScrollArea *scroll_ = nullptr;
};

QPointer<DeviceMapDialog> g_dialog;
QPointer<QWidget> g_parent;

void openFromShow(void *, const char *id)
{
	ctd_device_map_dialog_open(g_parent, id);
}

} // namespace

void ctd_device_map_dialog_open(QWidget *parent, const char *showId)
{
	if (!showId || !*showId)
		return;
	const QString id = QString::fromUtf8(showId);
	if (g_dialog && g_dialog->showId() == id) {
		g_dialog->reload();
		g_dialog->show();
		g_dialog->raise();
		g_dialog->activateWindow();
		return;
	}
	if (g_dialog)
		g_dialog->close(); /* WA_DeleteOnClose */
	g_dialog = new DeviceMapDialog(id, parent);
	g_dialog->show();
	g_dialog->raise();
}

void ctd_device_map_dialog_install(QWidget *parent)
{
	g_parent = parent;
	show_devices_set_opener(openFromShow, nullptr);
}
