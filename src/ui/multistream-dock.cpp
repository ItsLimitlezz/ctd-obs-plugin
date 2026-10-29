/*
 * The CTD Relay dock (SPEC section 5.5): connect by pairing, pick this OBS's profile, see its destinations,
 * alerts and chat live, send OBS's stream to the profile, pull its overlays in as browser sources on the right canvas,
 * ask CTD Overlay for new ones, and read the hub's shared timecode.
 *
 * All network work happens on the client's threads (multistream.c); this widget only reads its snapshot twice a second
 * and does the OBS-side changes, on the UI thread where the frontend API wants them.
 */

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <cstring>
#include <string>
#include <vector>

#include "../multistream.h"
#include "icons.hpp"

namespace {

/* the private settings keys that mark a browser source as one of the hub's overlays (H21) */
constexpr const char *kOverlaySlot = "ctd_overlay_slot";
constexpr const char *kOverlayCanvas = "ctd_overlay_canvas";
constexpr const char *kOverlayName = "ctd_overlay_name";
constexpr const char *kOverlayApp = "ctd_overlay_app";

QString str(obs_data_t *data, const char *key)
{
	return QString::fromUtf8(data ? obs_data_get_string(data, key) : "");
}

/* landscape or vertical: meta.canvas, else the shape */
QString overlayCanvas(obs_data_t *overlay)
{
	obs_data_t *meta = obs_data_get_obj(overlay, "meta");
	QString canvas = str(meta, "canvas");
	obs_data_release(meta);
	if (canvas.isEmpty())
		canvas = obs_data_get_int(overlay, "height") > obs_data_get_int(overlay, "width") ? "vertical"
												  : "landscape";
	return canvas;
}

QString overlaySlot(obs_data_t *overlay)
{
	obs_data_t *meta = obs_data_get_obj(overlay, "meta");
	const QString slot = str(meta, "slot");
	obs_data_release(meta);
	return slot;
}

/* stream keys are secrets: only their start is ever shown */
QString maskKey(const QString &key)
{
	return key.size() <= 6 ? QString("••••••") : key.left(6) + "••••••";
}

/* The vertical canvas Aitum Stream Suite (or anything else) made with OBS's canvas API: the first canvas other than
 * the main one whose picture is taller than wide. A strong reference, or nullptr. */
obs_canvas_t *findVerticalCanvas()
{
	struct Find {
		obs_canvas_t *main;
		obs_canvas_t *found = nullptr;
	} find{obs_get_main_canvas()};
	obs_enum_canvases(
		[](void *param, obs_canvas_t *canvas) {
			auto *f = static_cast<Find *>(param);
			struct obs_video_info ovi = {};
			if (canvas != f->main && obs_canvas_get_video_info(canvas, &ovi) &&
			    ovi.base_height > ovi.base_width) {
				f->found = obs_canvas_get_ref(canvas);
				return false;
			}
			return true;
		},
		&find);
	obs_canvas_release(find.main);
	return find.found;
}

/* The scene showing on a canvas's output: channel 0, through its transition. A strong reference, or nullptr. */
obs_source_t *canvasCurrentScene(obs_canvas_t *canvas)
{
	obs_source_t *source = obs_canvas_get_channel(canvas, 0);
	if (source && obs_source_get_type(source) == OBS_SOURCE_TYPE_TRANSITION) {
		obs_source_t *active = obs_transition_get_active_source(source);
		obs_source_release(source);
		source = active;
	}
	if (source && !obs_scene_from_source(source)) {
		obs_source_release(source);
		source = nullptr;
	}
	return source;
}

/* Long platform messages wrap onto more lines instead of running off the side. */
void wrapLines(QListWidget *list)
{
	list->setWordWrap(true);
	list->setResizeMode(QListView::Adjust);
	list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
}

class MultiStreamDock : public QWidget {
public:
	explicit MultiStreamDock(QWidget *parent) : QWidget(parent)
	{
		ctd::applyTheme(this);
		auto *layout = new QVBoxLayout(this);
		layout->setSpacing(8);
		layout->addLayout(ctdHeader(this, "relay-64.png", "CTD Relay"));

		/* connection (H18) */
		auto *connection = new QHBoxLayout();
		server_ = new QLineEdit(this);
		server_->setPlaceholderText(MS_DEFAULT_SERVER);
		connect_ = ctd::button(new QPushButton("Connect", this)); /* also Disconnect: never the bright one */
		connection->addWidget(server_, 1);
		connection->addWidget(connect_);
		layout->addLayout(connection);
		status_ = new QLabel(this);
		status_->setWordWrap(true);
		layout->addWidget(status_);
		action_ = new QLabel(this); /* the outcome of the last thing the user did, kept until the next one */
		action_->setWordWrap(true);
		action_->setProperty("ctdRole", "muted");
		layout->addWidget(action_);
		approve_ = ctd::button(new QPushButton("Open the approval page →", this), ctd::Role::Primary);
		layout->addWidget(approve_);

		/* profile (H19) */
		auto *profileRow = new QFormLayout();
		profile_ = new QComboBox(this);
		profileRow->addRow("This OBS goes to", profile_);
		layout->addLayout(profileRow);
		/* API v1.6: CTD Live mobile reaches OBS through the hub, only when the user says so (it shares a password) */
		shareObsws_ = new QCheckBox("Let CTD Live mobile control this OBS through CTD Relay", this);
		shareObsws_->setToolTip(
			"Sends OBS's WebSocket settings (Tools > WebSocket Server Settings), password "
			"included, to CTD Relay, and keeps them current. Untick to make CTD Relay forget them.");
		layout->addWidget(shareObsws_);
		shareStatus_ = new QLabel(this);
		shareStatus_->setWordWrap(true);
		layout->addWidget(shareStatus_);
		session_ = new QLabel(this);
		session_->setWordWrap(true);
		layout->addWidget(session_);
		clock_ = new QLabel(this);                                    /* the hub's shared timecode */
		clock_->setFont(ctd::font(ctd::Face::Mono, 12, QFont::Bold)); /* a HUD readout */
		clock_->setProperty("ctdRole", "title");
		clock_->setTextInteractionFlags(Qt::TextSelectableByMouse);
		clock_->hide();
		layout->addWidget(clock_);

		/* destinations and alerts (H20, H23, H25) */
		auto *destinationsBox = new QGroupBox("Destinations", this);
		auto *destinationsLayout = new QVBoxLayout(destinationsBox);
		destinations_ = new QListWidget(destinationsBox);
		destinations_->setMinimumHeight(120);
		wrapLines(destinations_);
		destinationsLayout->addWidget(destinations_);
		alerts_ = new QListWidget(destinationsBox);
		alerts_->setMaximumHeight(110);
		wrapLines(alerts_);
		destinationsLayout->addWidget(alerts_);
		route_ = new QPushButton("Send OBS's stream to this profile →", destinationsBox);
		ctd::button(route_, ctd::Role::Primary);
		destinationsLayout->addWidget(route_);
		ctd::eyebrowTitle(destinationsBox);
		layout->addWidget(destinationsBox);

		/* overlays (H21, H22) */
		auto *overlaysBox = new QGroupBox("Overlays", this);
		auto *overlaysLayout = new QVBoxLayout(overlaysBox);
		overlays_ = new QListWidget(overlaysBox);
		overlays_->setMinimumHeight(70);
		overlaysLayout->addWidget(overlays_);
		auto *overlayButtons = new QHBoxLayout();
		addOverlay_ = new QPushButton("Add to its canvas", overlaysBox);
		switchOverlays_ = new QPushButton("Switch existing to this profile", overlaysBox);
		ctd::button(addOverlay_);
		ctd::button(switchOverlays_);
		overlayButtons->addWidget(addOverlay_);
		overlayButtons->addWidget(switchOverlays_);
		overlaysLayout->addLayout(overlayButtons);
		auto *askRow = new QHBoxLayout();
		template_ = new QLineEdit(overlaysBox);
		template_->setPlaceholderText("CTD Overlay template, e.g. takeover");
		askCanvas_ = new QComboBox(overlaysBox);
		askCanvas_->addItem("landscape");
		askCanvas_->addItem("vertical");
		ask_ = new QPushButton("Ask CTD Overlay", overlaysBox);
		askRow->addWidget(template_, 1);
		askRow->addWidget(askCanvas_);
		ctd::button(ask_);
		askRow->addWidget(ask_);
		overlaysLayout->addLayout(askRow);
		askStatus_ = new QLabel(overlaysBox);
		askStatus_->setWordWrap(true);
		askStatus_->setProperty("ctdRole", "muted");
		overlaysLayout->addWidget(askStatus_);
		ctd::eyebrowTitle(overlaysBox);
		layout->addWidget(overlaysBox);

		/* chat (H24) */
		auto *chatBox = new QGroupBox("Chat", this);
		auto *chatLayout = new QVBoxLayout(chatBox);
		chat_ = new QTextBrowser(chatBox);
		chat_->setOpenExternalLinks(true);
		chat_->setMinimumHeight(120);
		chatLayout->addWidget(chat_);
		ctd::eyebrowTitle(chatBox);
		layout->addWidget(chatBox, 1);

		QObject::connect(connect_, &QPushButton::clicked, this, [this] { connectClicked(); });
		QObject::connect(server_, &QLineEdit::editingFinished, this,
				 [this] { ms_set_server(server_->text().trimmed().toUtf8().constData()); });
		QObject::connect(approve_, &QPushButton::clicked, this,
				 [this] { QDesktopServices::openUrl(QUrl(approveUrl_)); });
		QObject::connect(shareObsws_, &QCheckBox::clicked, this, [this](bool on) { shareClicked(on); });
		QObject::connect(profile_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
			ms_set_profile(profile_->itemData(index).toString().toUtf8().constData());
		});
		QObject::connect(route_, &QPushButton::clicked, this, [this] {
			wantIngest_ = true;
			ms_request_ingest();
			route_->setEnabled(false);
		});
		QObject::connect(addOverlay_, &QPushButton::clicked, this, [this] {
			obs_data_t *overlay = selectedOverlay();
			if (overlay)
				addOverlay(overlay);
			else
				QMessageBox::information(this, "CTD Relay", "Pick an overlay in the list first.");
		});
		QObject::connect(switchOverlays_, &QPushButton::clicked, this, [this] { switchOverlays(); });
		QObject::connect(ask_, &QPushButton::clicked, this, [this] {
			const QString name = template_->text().trimmed();
			if (name.isEmpty()) {
				QMessageBox::information(this, "CTD Relay", "Type the CTD Overlay template to use.");
				return;
			}
			wantCreated_ = true;
			ms_request_overlay(name.toUtf8().constData(), askCanvas_->currentText().toUtf8().constData());
		});

		auto *timer = new QTimer(this);
		QObject::connect(timer, &QTimer::timeout, this, [this] { refresh(); });
		timer->start(500);
		auto *clockTimer = new QTimer(this);
		QObject::connect(clockTimer, &QTimer::timeout, this, [this] { showClock(); });
		clockTimer->start(40); /* a little faster than the frames, so the frame count doesn't stutter */
		refresh();
	}

private:
	void connectClicked()
	{
		struct ms_snapshot s;
		ms_snapshot_get(&s);
		const bool linked = s.link != MS_DISCONNECTED;
		ms_snapshot_free(&s);
		if (linked) {
			ms_disconnect();
		} else {
			ms_set_server(server_->text().trimmed().toUtf8().constData());
			ms_start_pairing();
		}
	}

	void shareClicked(bool on)
	{
		if (on) {
			const QString server = server_->text().trimmed();
			QString detail =
				QString("CTD Relay (%1) gets OBS's WebSocket password and this PC's address, so CTD "
					"Live mobile can switch scenes and start or stop the stream through it. "
					"Anyone who can sign in to CTD Relay can then control this OBS.")
					.arg(server.isEmpty() ? QString(MS_DEFAULT_SERVER) : server);
			if (server.startsWith("https://", Qt::CaseInsensitive))
				detail +=
					"\n\nThe address is https://, so the password travels encrypted, to a server whose "
					"certificate this PC trusts.";
			else
				detail +=
					"\n\nThe address doesn't start with https://, so the password crosses the network "
					"unencrypted. Only do this on a network you trust, or connect to CTD Relay's https:// "
					"address instead.";
			if (QMessageBox::question(this, "CTD Relay", detail + "\n\nShare OBS's WebSocket settings?") !=
			    QMessageBox::Yes) {
				shareObsws_->setChecked(false);
				return;
			}
		}
		ms_set_share_obs_websocket(on);
	}

	/* Run time since the session went live, and the hub's time of day, both as the hub's timecode. */
	void showClock()
	{
		if (!isVisible())
			return;
		struct ms_clock c;
		ms_clock_get(&c);
		if (!c.valid) {
			clock_->hide();
			return;
		}
		char run[32] = "--:--:--:--", utc[32], local[32];
		if (c.run_started_ms > 0)
			ms_timecode(c.now_ms - c.run_started_ms, c.fps, false, run, sizeof(run));
		ms_timecode(c.now_ms, c.fps, true, utc, sizeof(utc));
		ms_timecode(c.now_ms + (int64_t)c.utc_offset_min * 60000, c.fps, true, local, sizeof(local));
		clock_->setText(QString("Run %1   UTC %2   Local %3").arg(run, utc, local));
		clock_->setToolTip(QString("CTD Relay's shared clock at %1 fps, synced to within about %2 ms")
					   .arg(c.fps, 0, 'g', 4)
					   .arg(c.delay_ms / 2, 0, 'f', 0));
		clock_->show();
	}

	void refresh()
	{
		struct ms_snapshot s;
		ms_snapshot_get(&s);
		if (s.seq != lastSeq_) {
			lastSeq_ = s.seq;
			showConnection(s);
			/* each list is rebuilt only when it changed, so selections and open menus survive the live updates */
			const QString profileId = QString::fromUtf8(s.profile_id);
			if (lastProfiles_.changed(s.profiles) || profileId != lastProfileId_) {
				lastProfileId_ = profileId;
				showProfiles(s);
			}
			if (lastStatus_.changed(s.status))
				showStatus(s);
			if (lastOverlays_.changed(s.overlays))
				showOverlays(s);
		}
		if (s.chat_seq != lastChatSeq_) {
			lastChatSeq_ = s.chat_seq;
			showChat(s);
		}
		if (s.alerts_seq != lastAlertsSeq_) {
			lastAlertsSeq_ = s.alerts_seq;
			showAlerts(s);
		}
		if (s.overlay_seq != lastOverlaySeq_) {
			lastOverlaySeq_ = s.overlay_seq;
			askStatus_->setText(QString::fromUtf8(s.overlay_message));
			ask_->setEnabled(s.overlay_request != MS_OVERLAY_SENDING &&
					 s.overlay_request != MS_OVERLAY_WAITING);
			/* H22: the overlay CTD Overlay just made goes straight onto its canvas */
			if (wantCreated_ && s.overlay_request == MS_OVERLAY_DONE && s.overlay_created) {
				wantCreated_ = false;
				addOverlay(s.overlay_created);
			} else if (s.overlay_request == MS_OVERLAY_FAILED) {
				wantCreated_ = false;
			}
		}
		if (wantIngest_ && s.ingest_seq != lastIngestSeq_) {
			wantIngest_ = false;
			route_->setEnabled(true);
			applyIngest(s.ingest);
		}
		lastIngestSeq_ = s.ingest_seq;
		ms_snapshot_free(&s);
	}

	void showConnection(const ms_snapshot &s)
	{
		if (!server_->hasFocus())
			server_->setText(QString::fromUtf8(s.server));
		QString text = QString::fromUtf8(s.message);
		if (s.link == MS_CONNECTED) {
			text += s.events_live ? " · live updates" : " · reconnecting live updates…";
			/* API v1.4: OBS's own events going to the hub */
			if (s.events_state == MS_EVENTS_OK)
				text += QString(" · %1 OBS events sent").arg(s.events_sent);
			else if (s.events_state == MS_EVENTS_FAILING)
				text += " · OBS events: retrying";
			else if (s.events_state == MS_EVENTS_UNSUPPORTED)
				text += " · this CTD Relay doesn't take OBS events yet (needs v1.4)";
			else if (s.events_state == MS_EVENTS_NEED_SCOPE)
				text += "\nTo send OBS events to CTD Relay, disconnect and connect again (it needs the "
					"\"Send OBS events\" permission).";
		}
		status_->setText(text);
		approveUrl_ = QString::fromUtf8(s.approve_url);
		approve_->setVisible(s.link == MS_PAIRING && !approveUrl_.isEmpty());
		connect_->setText(s.link != MS_DISCONNECTED ? "Disconnect" : "Connect");
		const bool connected = s.link == MS_CONNECTED;
		shareObsws_->setChecked(s.share_obs_websocket);
		shareStatus_->setText(QString::fromUtf8(s.obs_websocket_message));
		shareStatus_->setVisible(s.link != MS_DISCONNECTED && *s.obs_websocket_message);
		profile_->setEnabled(connected && !s.profile_locked);
		route_->setEnabled(connected && !wantIngest_);
		addOverlay_->setEnabled(connected);
		switchOverlays_->setEnabled(connected);
		ask_->setEnabled(connected && s.overlay_request != MS_OVERLAY_SENDING &&
				 s.overlay_request != MS_OVERLAY_WAITING);
	}

	void showProfiles(const ms_snapshot &s)
	{
		profile_->blockSignals(true);
		profile_->clear();
		const size_t count = s.profiles ? obs_data_array_count(s.profiles) : 0;
		for (size_t i = 0; i < count; i++) {
			obs_data_t *p = obs_data_array_item(s.profiles, i);
			const QString status = str(p, "status");
			const QString label = QString("%1 — %2 (%3/%4 live)")
						      .arg(str(p, "name"), status.isEmpty() ? "offline" : status)
						      .arg(obs_data_get_int(p, "live"))
						      .arg(obs_data_get_int(p, "destinations"));
			profile_->addItem(label, str(p, "id"));
			if (str(p, "id") == QString::fromUtf8(s.profile_id))
				profile_->setCurrentIndex(profile_->count() - 1);
			obs_data_release(p);
		}
		profile_->blockSignals(false);
	}

	void showStatus(const ms_snapshot &s)
	{
		destinations_->clear();
		if (!s.status) {
			session_->clear();
			return;
		}
		obs_data_t *session = obs_data_get_obj(s.status, "session");
		/* the title in ink; LIVE in magenta (on air); TEST in cyan */
		const QString state = str(session, "status");
		QString text = QString("<b style='color:#f2fbff'>%1</b> · %2")
				       .arg(str(session, "title").toHtmlEscaped(),
					    state == "live" ? QString("<b style='color:#ff00b8'>● LIVE</b>")
							    : state.toHtmlEscaped());
		if (obs_data_get_bool(session, "testMode"))
			text += " · <b style='color:#39e7ff'>TEST</b>";
		/* API v1.3: every platform's viewers added up, kept current by stream.status; absent while unknown */
		if (obs_data_has_user_value(session, "viewers"))
			text += QString(" · <b>%1</b> watching").arg(obs_data_get_int(session, "viewers"));
		obs_data_release(session);

		/* per-feed bitrates, kept current by feed.status */
		obs_data_array_t *feeds = obs_data_get_array(s.status, "feeds");
		for (size_t i = 0; feeds && i < obs_data_array_count(feeds); i++) {
			obs_data_t *f = obs_data_array_item(feeds, i);
			text += QString("<br>%1: %2")
					.arg(str(f, "feed").toHtmlEscaped(),
					     obs_data_get_bool(f, "online")
						     ? QString("%1 kbps").arg(obs_data_get_int(f, "bitrateKbps"))
						     : QString("offline"));
			obs_data_release(f);
		}
		obs_data_array_release(feeds);
		session_->setText(text);

		obs_data_array_t *destinations = obs_data_get_array(s.status, "destinations");
		for (size_t i = 0; destinations && i < obs_data_array_count(destinations); i++) {
			obs_data_t *d = obs_data_array_item(destinations, i);
			const bool down = obs_data_get_bool(d, "down");
			const bool live = obs_data_get_bool(d, "live");
			QString line = QString("%1 (%2) — ").arg(str(d, "name"), str(d, "feed"));
			line += down ? QString("DOWN") : live ? QString("LIVE") : str(d, "phase");
			obs_data_t *stats = obs_data_get_obj(d, "stats");
			if (stats)
				line += QString(" · %1 kbps · %2 fps")
						.arg(obs_data_get_int(stats, "bitrateKbps"))
						.arg(obs_data_get_double(stats, "fps"));
			obs_data_release(stats);
			if (obs_data_get_int(d, "viewers") > 0)
				line += QString(" · %1 viewers").arg(obs_data_get_int(d, "viewers"));
			const QString message = str(d, "message");
			if (!message.isEmpty())
				line += "\n" + message;
			auto *item = new QListWidgetItem(line, destinations_);
			if (down)
				item->setForeground(ctd::tallyRed()); /* beside the word DOWN */
			else if (live)
				item->setForeground(ctd::magenta()); /* on air */
			item->setToolTip(message);
			obs_data_release(d);
		}
		obs_data_array_release(destinations);
	}

	/* H25: "Instagram went down, recovering: …" / "Instagram is back after 41 s", newest first */
	void showAlerts(const ms_snapshot &s)
	{
		alerts_->clear();
		const size_t count = s.alerts ? obs_data_array_count(s.alerts) : 0;
		for (size_t i = count; i > 0; i--) {
			obs_data_t *a = obs_data_array_item(s.alerts, i - 1);
			const QString level = str(a, "level");
			auto *item = new QListWidgetItem(QString("%1  %2").arg(alertTime(a), str(a, "text")), alerts_);
			if (level == "down")
				item->setForeground(ctd::tallyRed());
			else if (level == "up")
				item->setForeground(ctd::cyan()); /* back: verified */
			obs_data_release(a);
		}
		alerts_->setVisible(count > 0);
	}

	/* the alert's time of day, from when it arrived */
	QString alertTime(obs_data_t *alert)
	{
		const qint64 arrived_ms = obs_data_get_int(alert, "ts"); /* os_gettime_ns based */
		const qint64 now_ms = (qint64)(os_gettime_ns() / 1000000);
		return QDateTime::currentDateTime().addMSecs(arrived_ms - now_ms).toString("HH:mm:ss");
	}

	void showOverlays(const ms_snapshot &s)
	{
		const QString selected =
			overlays_->currentItem() ? overlays_->currentItem()->data(Qt::UserRole).toString() : QString();
		overlays_->clear();
		overlayData_.clear();
		const size_t count = s.overlays ? obs_data_array_count(s.overlays) : 0;
		for (size_t i = 0; i < count; i++) {
			obs_data_t *o = obs_data_array_item(s.overlays, i);
			QString line = QString("%1 (%2×%3, %4) — %5")
					       .arg(str(o, "name"))
					       .arg(obs_data_get_int(o, "width"))
					       .arg(obs_data_get_int(o, "height"))
					       .arg(overlayCanvas(o), str(o, "appName"));
			if (!obs_data_get_bool(o, "appOnline"))
				line += " (offline)";
			auto *item = new QListWidgetItem(line, overlays_);
			item->setData(Qt::UserRole, str(o, "id"));
			if (str(o, "id") == selected)
				overlays_->setCurrentItem(item);
			overlayData_.emplace_back(o); /* owned until the next refresh */
		}
	}

	void showChat(const ms_snapshot &s)
	{
		QString html;
		const size_t count = s.chat ? obs_data_array_count(s.chat) : 0;
		for (size_t i = 0; i < count; i++) {
			obs_data_t *envelope = obs_data_array_item(s.chat, i);
			obs_data_t *data = obs_data_get_obj(envelope, "data");
			obs_data_t *user = obs_data_get_obj(data, "user");
			QString color = str(user, "color");
			if (color.isEmpty())
				color = "#8ba3af";
			if (obs_data_get_bool(data, "join") || str(data, "kind") == "event") {
				/* API v1.3: "<name> joined" from Instagram/TikTok, and v1.7's kind "event" lines ("<name>
				 * followed"): dimmed so they don't drown the chat */
				QString joined = str(data, "text");
				if (joined.isEmpty())
					joined = str(user, "name") + " joined";
				html += QString("<div style='color:#4a5a63'>[%1] %2</div>")
						.arg(str(data, "platform").toHtmlEscaped(), joined.toHtmlEscaped());
			} else {
				html += QString("<div><span style='color:#8ba3af'>[%1]</span> <b style='color:%2'>%3</b>: "
						"%4</div>")
						.arg(str(data, "platform").toHtmlEscaped(), color.toHtmlEscaped(),
						     str(user, "name").toHtmlEscaped(),
						     str(data, "text").toHtmlEscaped());
			}
			obs_data_release(user);
			obs_data_release(data);
			obs_data_release(envelope);
		}
		chat_->setHtml(html);
		chat_->moveCursor(QTextCursor::End);
	}

	/* H20: OBS's stream set from the profile's ingest, on the user's say-so, never while streaming */
	void applyIngest(obs_data_t *ingest)
	{
		if (!ingest) {
			QMessageBox::warning(this, "CTD Relay",
					     "Couldn't read this profile's stream settings. The connection needs the "
					     "ingest:read permission (approve it again in CTD Relay).");
			return;
		}
		if (obs_frontend_streaming_active()) {
			QMessageBox::information(
				this, "CTD Relay",
				"OBS is streaming. Stop the stream first, then send it to this profile.");
			return;
		}
		const QString server = str(ingest, "serverUrl");
		QString landscapeKey, verticalKey, verticalUrl;
		obs_data_array_t *feeds = obs_data_get_array(ingest, "feeds");
		for (size_t i = 0; feeds && i < obs_data_array_count(feeds); i++) {
			obs_data_t *f = obs_data_array_item(feeds, i);
			if (str(f, "feed") == "landscape") {
				landscapeKey = str(f, "streamKey");
			} else if (str(f, "feed") == "vertical") {
				verticalKey = str(f, "streamKey");
				verticalUrl = str(f, "fullUrl");
			}
			obs_data_release(f);
		}
		obs_data_array_release(feeds);
		if (server.isEmpty() || landscapeKey.isEmpty()) {
			QMessageBox::warning(this, "CTD Relay", "This profile has no landscape stream key.");
			return;
		}

		QString question = QString("Set OBS's main stream to this profile?\n\nServer: %1\nStream key: %2")
					   .arg(server, maskKey(landscapeKey));
		if (!verticalKey.isEmpty())
			question += QString("\n\nVertical (Aitum): %1, key %2\nAitum Stream Suite doesn't let other "
					    "plugins set its outputs, so the vertical stream URL will be copied to the "
					    "clipboard: paste it into Aitum's \"Vertical Stream\" output.")
					    .arg(server, maskKey(verticalKey));
		if (QMessageBox::question(this, "CTD Relay", question) != QMessageBox::Yes)
			return;

		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "server", server.toUtf8().constData());
		obs_data_set_string(settings, "key", landscapeKey.toUtf8().constData());
		obs_service_t *service = obs_service_create("rtmp_custom", "default_service", settings, nullptr);
		obs_data_release(settings);
		if (!service) {
			QMessageBox::warning(this, "CTD Relay", "OBS couldn't create the stream service.");
			return;
		}
		obs_frontend_set_streaming_service(service);
		obs_frontend_save_streaming_service();
		obs_service_release(service);
		if (!verticalUrl.isEmpty())
			QApplication::clipboard()->setText(verticalUrl);
		blog(LOG_INFO, "[ctd-live] CTD Relay: OBS's main stream now goes to the chosen profile");
		action_->setText(
			"OBS's main stream now goes to this profile." +
			QString(verticalUrl.isEmpty() ? "" : " The vertical URL is on the clipboard for Aitum."));
	}

	obs_data_t *selectedOverlay() const
	{
		const int row = overlays_->currentRow();
		return row >= 0 && row < (int)overlayData_.size() ? overlayData_[row].get() : nullptr;
	}

	static void setOverlaySettings(obs_source_t *source, obs_data_t *overlay)
	{
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "url", obs_data_get_string(overlay, "url"));
		obs_data_set_int(settings, "width", obs_data_get_int(overlay, "width"));
		obs_data_set_int(settings, "height", obs_data_get_int(overlay, "height"));
		obs_source_update(source, settings);
		obs_data_release(settings);

		obs_data_t *priv = obs_source_get_private_settings(source);
		obs_data_set_string(priv, kOverlaySlot, overlaySlot(overlay).toUtf8().constData());
		obs_data_set_string(priv, kOverlayCanvas, overlayCanvas(overlay).toUtf8().constData());
		obs_data_set_string(priv, kOverlayName, obs_data_get_string(overlay, "name"));
		obs_data_set_string(priv, kOverlayApp, obs_data_get_string(overlay, "appId"));
		obs_data_release(priv);
	}

	/* H21: a browser source at the overlay's size, in the current scene of the canvas meta.canvas names (the main
	 * canvas for landscape, the vertical canvas for vertical) */
	void addOverlay(obs_data_t *overlay)
	{
		const bool vertical = overlayCanvas(overlay) == "vertical";
		obs_source_t *sceneSource = nullptr;
		QString where = "the current scene";
		if (vertical) {
			obs_canvas_t *canvas = findVerticalCanvas();
			if (canvas) {
				sceneSource = canvasCurrentScene(canvas);
				where = QString("the %1 canvas's scene")
						.arg(QString::fromUtf8(obs_canvas_get_name(canvas)));
				obs_canvas_release(canvas);
			}
			if (!sceneSource) {
				action_->setText(
					"No vertical canvas found (Aitum Stream Suite): added to the main scene.");
				where = "the main scene";
			}
		}
		if (!sceneSource)
			sceneSource = obs_frontend_get_current_scene();
		obs_scene_t *scene = obs_scene_from_source(sceneSource);
		if (!scene) {
			obs_source_release(sceneSource);
			return;
		}
		/* a name no other source has */
		const std::string base = obs_data_get_string(overlay, "name");
		std::string name = base.empty() ? "CTD overlay" : base;
		for (int n = 2;; n++) {
			obs_source_t *existing = obs_get_source_by_name(name.c_str());
			if (!existing)
				break;
			obs_source_release(existing);
			name = (base.empty() ? std::string("CTD overlay") : base) + " " + std::to_string(n);
		}
		obs_data_t *settings = obs_data_create();
		obs_source_t *source = obs_source_create("browser_source", name.c_str(), settings, nullptr);
		obs_data_release(settings);
		if (source) {
			setOverlaySettings(source, overlay);
			/* top-left at 1:1, so an overlay the canvas's size covers it exactly. Left to OBS, a browser
			 * source that hasn't loaded yet is 0×0 and gets centred, leaving most of it off the canvas. */
			obs_sceneitem_t *item = obs_scene_add(scene, source);
			if (item) {
				const vec2 origin = {0.0f, 0.0f};
				obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
				obs_sceneitem_set_pos(item, &origin);
			}
			obs_source_release(source);
			blog(LOG_INFO, "[ctd-live] CTD Relay: added overlay '%s' to %s", name.c_str(),
			     where.toUtf8().constData());
			if (!vertical || where != "the main scene")
				action_->setText(
					QString("Added \"%1\" to %2.").arg(QString::fromStdString(name), where));
		}
		obs_source_release(sceneSource);
	}

	/* H21: browser sources showing one of the hub's overlays now show this profile's equivalent (meta.slot +
	 * meta.canvas; by name when the overlay has no slot). Never deletes anything. */
	void switchOverlays()
	{
		struct Match {
			MultiStreamDock *dock;
			int switched = 0;
		} match{this};
		obs_enum_sources(
			[](void *param, obs_source_t *source) {
				auto *m = static_cast<Match *>(param);
				if (strcmp(obs_source_get_unversioned_id(source), "browser_source") != 0)
					return true;
				obs_data_t *priv = obs_source_get_private_settings(source);
				const QString slot = str(priv, kOverlaySlot);
				const QString canvas = str(priv, kOverlayCanvas);
				const QString name = str(priv, kOverlayName);
				obs_data_release(priv);
				if (slot.isEmpty() && name.isEmpty())
					return true; /* not one of ours */
				for (const auto &o : m->dock->overlayData_) {
					const QString oSlot = overlaySlot(o.get());
					const bool same = !slot.isEmpty() && !oSlot.isEmpty()
								  ? oSlot == slot && overlayCanvas(o.get()) == canvas
								  : str(o.get(), "name") == name;
					if (same) {
						setOverlaySettings(source, o.get());
						m->switched++;
						break;
					}
				}
				return true;
			},
			&match);
		action_->setText(match.switched
					 ? QString("Switched %1 overlay source(s) to this profile.").arg(match.switched)
					 : QString("No overlay sources to switch: add some with \"Add to its "
						   "canvas\" first."));
	}

	struct DataRef {
		obs_data_t *data;
		explicit DataRef(obs_data_t *d) : data(d) {}
		DataRef(const DataRef &) = delete;
		DataRef(DataRef &&other) noexcept : data(other.data) { other.data = nullptr; }
		~DataRef() { obs_data_release(data); }
		obs_data_t *get() const { return data; }
	};

	QLineEdit *server_;
	QPushButton *connect_;
	QLabel *status_;
	QLabel *action_;
	QPushButton *approve_;
	QCheckBox *shareObsws_;
	QLabel *shareStatus_;
	QComboBox *profile_;
	QLabel *session_;
	QLabel *clock_;
	QListWidget *destinations_;
	QListWidget *alerts_;
	QPushButton *route_;
	QListWidget *overlays_;
	QPushButton *addOverlay_;
	QPushButton *switchOverlays_;
	QLineEdit *template_;
	QComboBox *askCanvas_;
	QPushButton *ask_;
	QLabel *askStatus_;
	QTextBrowser *chat_;
	QString approveUrl_;
	std::vector<DataRef> overlayData_;
	/* what each list was last built from, held so its address can't be reused by a newer one */
	template<typename T, void (*AddRef)(T *), void (*Release)(T *)> struct Held {
		T *held = nullptr;
		bool changed(T *now)
		{
			if (now == held)
				return false;
			AddRef(now);
			Release(held);
			held = now;
			return true;
		}
		~Held() { Release(held); }
	};
	Held<obs_data_array_t, obs_data_array_addref, obs_data_array_release> lastProfiles_, lastOverlays_;
	Held<obs_data_t, obs_data_addref, obs_data_release> lastStatus_;
	QString lastProfileId_;
	uint64_t lastSeq_ = UINT64_MAX;
	uint64_t lastChatSeq_ = UINT64_MAX;
	uint64_t lastAlertsSeq_ = UINT64_MAX;
	uint64_t lastOverlaySeq_ = UINT64_MAX;
	uint64_t lastIngestSeq_ = 0;
	bool wantIngest_ = false;
	bool wantCreated_ = false;
};

} // namespace

extern "C" void ctd_multistream_dock_register(void)
{
	auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *dock = new MultiStreamDock(main);
	if (!obs_frontend_add_dock_by_id("ctd-multistream", "CTD Relay", dock))
		delete dock;
}
