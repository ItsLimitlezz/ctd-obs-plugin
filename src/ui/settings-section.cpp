/*
 * The CTD plugin's part of CTD OBS's Settings ▸ CTD page (integration gap 7): connecting and pairing with CTD Relay,
 * which profile this CTD OBS goes to, sharing the WebSocket settings, and the CTD Hub servers. It works on the same
 * state as the CTD Relay dock (src/multistream.c), so both always agree; the dock keeps the live status and the show
 * actions.
 *
 * Nothing here waits for Settings' OK: Connect / Disconnect, the profile and sharing act at once, like the dock. The
 * address only takes effect through Connect (it's read-only while connected), so editing it and pressing Cancel never
 * unpairs, and Return in it means Connect, not the dialog's OK.
 *
 * void ctd_settings_widget(in ptr parent, out ptr widget)
 *     Makes the section as a child of parent (a QWidget *). UI thread only. CTD OBS's settings page calls it.
 */

#include <obs-module.h>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QEvent>
#include <QKeyEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "../multistream.h"
#include "../obs/servers.h"

namespace {

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString str(obs_data_t *data, const char *key)
{
	return QString::fromUtf8(obs_data_get_string(data, key));
}

class CtdSettingsSection : public QWidget {
public:
	explicit CtdSettingsSection(QWidget *parent) : QWidget(parent)
	{
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);

		/* CTD Relay: address, pairing, profile, WebSocket sharing */
		auto *relay = new QGroupBox(text("Settings.Relay.Title"), this);
		auto *form = new QFormLayout(relay);
		form->setRowWrapPolicy(QFormLayout::WrapLongRows);
		server_ = new QLineEdit(relay);
		server_->setPlaceholderText(MS_SERVER_EXAMPLE);
		connect_ = new QPushButton(relay);
		auto *row = new QHBoxLayout();
		row->addWidget(server_, 1);
		row->addWidget(connect_);
		form->addRow(text("Settings.Relay.Address"), row);
		status_ = new QLabel(relay);
		status_->setWordWrap(true);
		form->addRow(text("Settings.Relay.Status"), status_);
		approve_ = new QPushButton(text("Settings.Relay.Approve"), relay);
		form->addRow(QString(), approve_);
		profile_ = new QComboBox(relay);
		profile_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		profile_->setMinimumContentsLength(20);
		form->addRow(text("Settings.Relay.Profile"), profile_);
		share_ = new QCheckBox(text("Settings.Relay.Share"), relay);
		share_->setToolTip(text("Settings.Relay.ShareTip"));
		form->addRow(QString(), share_);
		shareStatus_ = new QLabel(relay);
		shareStatus_->setWordWrap(true);
		form->addRow(QString(), shareStatus_);
		layout->addWidget(relay);

		/* CTD Hub: the MediaMTX servers the CTD Live sources read from */
		auto *hub = new QGroupBox(text("Settings.Hub.Title"), this);
		auto *hubLayout = new QVBoxLayout(hub);
		auto *hubText = new QLabel(text("Settings.Hub.Text"), hub);
		hubText->setWordWrap(true);
		hubLayout->addWidget(hubText);
		auto *servers = new QPushButton(text("Settings.Hub.Servers"), hub);
		hubLayout->addWidget(servers, 0, Qt::AlignLeft);
		layout->addWidget(hub);

		QObject::connect(connect_, &QPushButton::clicked, this, [this] { connectClicked(); });
		server_->installEventFilter(this); /* Return = Connect, not Settings' OK */
		QObject::connect(approve_, &QPushButton::clicked, this,
				 [this] { QDesktopServices::openUrl(QUrl(approveUrl_)); });
		QObject::connect(profile_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
			ms_set_profile(profile_->itemData(index).toString().toUtf8().constData());
		});
		QObject::connect(share_, &QCheckBox::clicked, this, [this](bool on) { shareClicked(on); });
		QObject::connect(servers, &QPushButton::clicked, this, [] { ctd_servers_dialog_open(); });

		auto *timer = new QTimer(this);
		QObject::connect(timer, &QTimer::timeout, this, [this] { refresh(); });
		timer->start(500);
		refresh();
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (watched == server_ && event->type() == QEvent::KeyPress) {
			const int key = static_cast<QKeyEvent *>(event)->key();
			if (key == Qt::Key_Return || key == Qt::Key_Enter) {
				if (!server_->isReadOnly())
					connectClicked();
				return true;
			}
		}
		return QWidget::eventFilter(watched, event);
	}

private:
	void refresh()
	{
		/* an open profile drop-down is left alone; the change shows once it closes */
		if (profile_->view()->isVisible())
			return;
		struct ms_snapshot s;
		ms_snapshot_get(&s);
		if (s.seq != lastSeq_) {
			lastSeq_ = s.seq;
			showConnection(s);
			showProfiles(s);
		}
		ms_snapshot_free(&s);
	}

	void showConnection(const ms_snapshot &s)
	{
		if (!server_->hasFocus() || s.link != MS_DISCONNECTED)
			server_->setText(QString::fromUtf8(s.server));
		server_->setReadOnly(s.link != MS_DISCONNECTED);
		status_->setText(QString::fromUtf8(s.message));
		approveUrl_ = QString::fromUtf8(s.approve_url);
		approve_->setVisible(s.link == MS_PAIRING && !approveUrl_.isEmpty());
		connect_->setText(
			text(s.link != MS_DISCONNECTED ? "Settings.Relay.Disconnect" : "Settings.Relay.Connect"));
		profile_->setEnabled(s.link == MS_CONNECTED && !s.profile_locked);
		share_->setChecked(s.share_obs_websocket);
		shareStatus_->setText(QString::fromUtf8(s.obs_websocket_message));
		shareStatus_->setVisible(s.link != MS_DISCONNECTED && *s.obs_websocket_message);
	}

	void showProfiles(const ms_snapshot &s)
	{
		/* rebuilt only when the list changed, and never under an open drop-down */
		QStringList labels, ids;
		const size_t count = s.profiles ? obs_data_array_count(s.profiles) : 0;
		for (size_t i = 0; i < count; i++) {
			obs_data_t *p = obs_data_array_item(s.profiles, i);
			const QString status = str(p, "status");
			labels << text("Settings.Relay.ProfileItem")
					  .arg(str(p, "name"),
					       status.isEmpty() ? text("Settings.Relay.Offline") : status);
			ids << str(p, "id");
			obs_data_release(p);
		}
		const QString current = QString::fromUtf8(s.profile_id);
		if (labels == labels_ && ids == ids_ && current == current_)
			return;
		labels_ = labels;
		ids_ = ids;
		current_ = current;
		profile_->blockSignals(true);
		profile_->clear();
		for (int i = 0; i < labels.size(); i++) {
			profile_->addItem(labels[i], ids[i]);
			if (ids[i] == current)
				profile_->setCurrentIndex(i);
		}
		profile_->blockSignals(false);
	}

	void connectClicked()
	{
		struct ms_snapshot s;
		ms_snapshot_get(&s);
		const bool linked = s.link != MS_DISCONNECTED;
		ms_snapshot_free(&s);
		if (linked) {
			ms_disconnect();
			return;
		}
		const QString server = server_->text().trimmed();
		if (server.isEmpty()) {
			/* there is no default server: pairing without one would wait forever */
			status_->setText(text("Settings.Relay.AddressFirst").arg(MS_SERVER_EXAMPLE));
			server_->setFocus();
			return;
		}
		ms_set_server(server.toUtf8().constData());
		ms_start_pairing();
	}

	void shareClicked(bool on)
	{
		if (on) {
			const QString server = server_->text().trimmed();
			QString detail = text("Settings.Relay.ShareDetail")
						 .arg(server.isEmpty() ? text("Settings.Relay.ThisServer") : server);
			detail += "\n\n";
			detail += text(server.startsWith("https://", Qt::CaseInsensitive) ? "Settings.Relay.ShareHttps"
											  : "Settings.Relay.ShareHttp");
			if (QMessageBox::question(this, text("Settings.Relay.Title"),
						  detail + "\n\n" + text("Settings.Relay.ShareQuestion")) !=
			    QMessageBox::Yes) {
				share_->setChecked(false);
				return;
			}
		}
		ms_set_share_obs_websocket(on);
	}

	QLineEdit *server_;
	QPushButton *connect_;
	QLabel *status_;
	QPushButton *approve_;
	QComboBox *profile_;
	QCheckBox *share_;
	QLabel *shareStatus_;
	QString approveUrl_;
	QStringList labels_, ids_;
	QString current_;
	uint64_t lastSeq_ = UINT64_MAX;
};

void procSettingsWidget(void *, calldata_t *cd)
{
	auto *parent = static_cast<QWidget *>(calldata_ptr(cd, "parent"));
	calldata_set_ptr(cd, "widget", new CtdSettingsSection(parent));
}

} // namespace

extern "C" void ctd_settings_section_register(void)
{
	proc_handler_add(obs_get_proc_handler(), "void ctd_settings_widget(in ptr parent, out ptr widget)",
			 procSettingsWidget, nullptr);
}
