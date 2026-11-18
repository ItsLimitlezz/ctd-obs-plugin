/*
 * The CTD Relay dock (SPEC section 5.5): connect by pairing, pick this OBS's profile, see its destinations,
 * alerts and chat live, send OBS's stream to the profile, pull its overlays in as browser sources on the right canvas,
 * ask CTD Overlay for new ones, and read the hub's shared timecode.
 *
 * All network work happens on the client's threads (multistream.c); this widget only reads its snapshot twice a second
 * and does the OBS-side changes, on the UI thread where the frontend API wants them.
 */

#include <obs-frontend-api.h>
#include <util/config-file.h>
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

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../multistream.h"
#include "../overlay-scenes.h"
#include "icons.hpp"
#include "show-section.hpp"

namespace {

/* user-visible text, from data/locale */
QString uiText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* the private settings keys that mark a browser source as one of the hub's overlays (H21) */
constexpr const char *kOverlaySlot = "ctd_overlay_slot";
constexpr const char *kOverlayCanvas = "ctd_overlay_canvas";
constexpr const char *kOverlayName = "ctd_overlay_name";
constexpr const char *kOverlayApp = "ctd_overlay_app";
/* the overlay's scene the source is pinned to (meta.scenes id, SKP 04 §4.9); empty or absent = the whole overlay */
constexpr const char *kOverlayScene = "ctd_overlay_scene";

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

/* One of the scenes CTD Overlay says a URL may pin (meta.scenes: [{ id, name, core? }]), for a source per scene. */
struct OverlayScene {
	QString id;
	QString name;
	std::string url; /* its own browser-source URL when CTD Overlay gives one; else the overlay's with scene=<id> */
};

/* meta.scenes, the ones with an id; empty when the overlay offers no choice (one scene, or an older CTD Overlay) */
std::vector<OverlayScene> overlayScenes(obs_data_t *overlay)
{
	std::vector<OverlayScene> out;
	obs_data_t *meta = obs_data_get_obj(overlay, "meta");
	obs_data_array_t *scenes = meta ? obs_data_get_array(meta, "scenes") : nullptr;
	const size_t count = scenes ? obs_data_array_count(scenes) : 0;
	for (size_t i = 0; i < count; i++) {
		obs_data_t *scene = obs_data_array_item(scenes, i);
		const QString id = str(scene, "id");
		const QString name = str(scene, "name");
		if (!id.isEmpty())
			out.push_back({id, name.isEmpty() ? id : name, obs_data_get_string(scene, "url")});
		obs_data_release(scene);
	}
	obs_data_array_release(scenes);
	obs_data_release(meta);
	return out;
}

/* the overlay's URL showing only `scene` (the whole overlay when it is empty): the scene's own url from meta.scenes,
 * else the overlay's url with scene=<id> set (SKP 04 §4.9) */
std::string overlaySceneUrl(obs_data_t *overlay, const QString &scene)
{
	if (!scene.isEmpty())
		for (const OverlayScene &s : overlayScenes(overlay))
			if (s.id == scene && !s.url.empty())
				return s.url;
	const QByteArray id = scene.toUtf8();
	char *url = ctd_overlay_scene_url(obs_data_get_string(overlay, "url"), id.isEmpty() ? nullptr : id.constData());
	std::string out = url ? url : obs_data_get_string(overlay, "url");
	free(url);
	return out;
}

/* stream keys are secrets: only their start is ever shown */
QString maskKey(const QString &key)
{
	return key.size() <= 6 ? QString("••••••") : key.left(6) + "••••••";
}

/* a host only reachable on its own network: loopback, RFC 1918, link-local, CGNAT, *.local, bare names */
bool isLanHost(const QString &host)
{
	const QString h = host.trimmed().toLower();
	if (h.isEmpty() || h == "localhost" || h.endsWith(".local") || h.endsWith(".lan") || h.endsWith(".home.arpa"))
		return true;
	const QStringList parts = h.split('.');
	if (parts.size() == 4) {
		int o[4];
		for (int i = 0; i < 4; i++) {
			bool ok = false;
			o[i] = parts[i].toInt(&ok);
			if (!ok || o[i] < 0 || o[i] > 255)
				return false; /* a DNS name with four labels */
		}
		if (o[0] == 10 || o[0] == 127)
			return true;
		if (o[0] == 172 && o[1] >= 16 && o[1] <= 31)
			return true;
		if ((o[0] == 192 && o[1] == 168) || (o[0] == 169 && o[1] == 254))
			return true;
		return o[0] == 100 && o[1] >= 64 && o[1] <= 127;
	}
	if (h.contains(':')) {
		/* IPv6; QUrl::host() gives it without brackets */
		const QString v6 = QString(h).remove('[').remove(']');
		return v6 == "::1" || v6.startsWith("fe80:") || v6.startsWith("fc") || v6.startsWith("fd");
	}
	return !h.contains('.'); /* a bare machine name, e.g. "nas" */
}

/* CTD Relay can hand back its LAN address (LAN_HTTP_HOST) even when this OBS reaches it through a public address,
 * and that LAN IP is unreachable from another network. When the ingest host is a LAN address and the Relay address
 * typed in the dock isn't, keep the RTMP port and path but use the Relay's host. Returns the server unchanged
 * otherwise. */
QString reachableIngestServer(const QString &server, const QString &relayAddress)
{
	QUrl ingest(server);
	const QUrl relay = QUrl::fromUserInput(relayAddress.trimmed());
	if (!ingest.isValid() || ingest.host().isEmpty() || !relay.isValid() || relay.host().isEmpty())
		return server;
	if (!isLanHost(ingest.host()) || isLanHost(relay.host()) || ingest.host() == relay.host())
		return server;
	ingest.setHost(relay.host());
	return ingest.toString();
}

/* The vertical canvas built into CTD OBS (ctd-vertical-canvas) lets other plugins set its stream output through these
 * procs. Stock OBS and Aitum's own plugins don't have them. */
bool ctdVerticalCanvasAvailable()
{
	calldata_t cd = {};
	const bool found = proc_handler_call(obs_get_proc_handler(), "ctd_vertical_get_output", &cd);
	calldata_free(&cd);
	return found;
}

/* True while the vertical canvas's stream is running (CTD OBS reports it as "active"; false when not detectable). */
bool ctdVerticalStreaming()
{
	calldata_t cd = {};
	const bool active = proc_handler_call(obs_get_proc_handler(), "ctd_vertical_get_output", &cd) &&
			    calldata_bool(&cd, "active");
	calldata_free(&cd);
	return active;
}

/* Sets the vertical canvas's stream output (custom RTMP) and saves it; no dialog. False with a reason on failure. */
bool setCtdVerticalOutput(const QString &server, const QString &key, QString &error)
{
	calldata_t cd = {};
	calldata_set_string(&cd, "server", server.toUtf8().constData());
	calldata_set_string(&cd, "key", key.toUtf8().constData());
	bool ok = false;
	if (!proc_handler_call(obs_get_proc_handler(), "ctd_vertical_set_output", &cd)) {
		error = uiText("Relay.VerticalNotLoaded");
	} else {
		ok = calldata_bool(&cd, "success");
		error = QString::fromUtf8(calldata_string(&cd, "error"));
	}
	calldata_free(&cd);
	return ok;
}

/* OBS's own Twitch "Dual Format" / Enhanced Broadcasting (multitrack video) sends straight to Twitch from OBS, so a
 * stream to CTD Relay isn't what goes out. */
bool obsMultitrackEnabled()
{
	config_t *profile = obs_frontend_get_profile_config();
	return profile && config_get_bool(profile, "Stream1", "EnableMultitrackVideo");
}

/* Keyframe interval 2 s on the main stream encoder (Relay warns about long keyframes). Only the keyint_sec of the
 * advanced-output encoder is touched; simple output already uses 2 s. Returns true when the setting is now 2 s. */
bool setMainKeyframe2s()
{
	config_t *profile = obs_frontend_get_profile_config();
	const char *mode = profile ? config_get_string(profile, "Output", "Mode") : nullptr;
	if (!mode || std::string(mode) != "Advanced")
		return false;
	char *dir = obs_frontend_get_current_profile_path();
	if (!dir)
		return false;
	const std::string path = std::string(dir) + "/streamEncoder.json";
	bfree(dir);
	obs_data_t *enc = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!enc)
		enc = obs_data_create();
	obs_data_set_int(enc, "keyint_sec", 2);
	const bool ok = obs_data_save_json_safe(enc, path.c_str(), "tmp", "bak");
	obs_data_release(enc);
	return ok;
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
	~MultiStreamDock() override { obs_frontend_remove_event_callback(frontendEvent, this); }

	explicit MultiStreamDock(QWidget *parent) : QWidget(parent)
	{
		ctd::applyTheme(this);
		/* scrolls instead of squashing, so the dock fits (and docks into) any area */
		auto *layout = new QVBoxLayout(ctd::scrollBody(this));
		layout->setSpacing(10);
		layout->addLayout(ctdHeader(this, "relay-64.png", uiText("Relay.Title")));

		/* connection (H18) */
		auto *connection = new QHBoxLayout();
		server_ = new QLineEdit(this);
		server_->setPlaceholderText(MS_SERVER_EXAMPLE);
		connect_ = ctd::button(
			new QPushButton(uiText("Relay.Connect"), this)); /* also Disconnect: never the bright one */
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
		multitrackWarning_ = new QLabel(uiText("Relay.MultitrackWarning"), this);
		multitrackWarning_->setWordWrap(true);
		multitrackWarning_->setStyleSheet("color: #e5a50a; font-weight: bold;");
		multitrackWarning_->hide();
		layout->addWidget(multitrackWarning_);
		approve_ = ctd::button(new QPushButton(uiText("Relay.OpenApproval"), this), ctd::Role::Primary);
		layout->addWidget(approve_);

		/* profile (H19) */
		auto *profileRow = new QFormLayout();
		profile_ = new QComboBox(this);
		/* a long profile name shortens in the box rather than widening the dock */
		profile_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		profile_->setMinimumContentsLength(10);
		profileRow->setRowWrapPolicy(QFormLayout::WrapLongRows);
		profileRow->addRow(uiText("Relay.ProfileRow"), profile_);
		layout->addLayout(profileRow);
		/* API v1.6: CTD Live mobile reaches OBS through the hub, only when the user says so (it shares a password) */
		shareObsws_ = new QCheckBox(this);
		shareObsws_->setToolTip(uiText("Relay.ShareObsws.Tooltip"));
		layout->addLayout(ctd::wrappingCheckBox(shareObsws_, uiText("Relay.ShareObsws"), this));
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

		/* show profiles (T12): hidden on stock OBS */
		showProfiles_ = new ShowProfilesSection(this);
		layout->addWidget(showProfiles_);

		/* destinations and alerts (H20, H23, H25) */
		auto *destinationsBox = new QGroupBox(uiText("Relay.Destinations"), this);
		auto *destinationsLayout = new QVBoxLayout(destinationsBox);
		destinationsLayout->setSpacing(8);
		destinations_ = new QListWidget(destinationsBox);
		destinations_->setMinimumHeight(96);
		wrapLines(destinations_);
		destinationsLayout->addWidget(destinations_, 1);
		alerts_ = new QListWidget(destinationsBox);
		alerts_->setMinimumHeight(48);
		alerts_->setMaximumHeight(110);
		wrapLines(alerts_);
		destinationsLayout->addWidget(alerts_);
		route_ = new QPushButton(uiText("Relay.Route"), destinationsBox);
		ctd::button(route_, ctd::Role::Primary);
		/* room for the primary button's 3 px offset shadow, so it never sits on the list above or the frame below */
		destinationsLayout->addSpacing(4);
		destinationsLayout->addWidget(route_);
		routeReason_ = new QLabel(uiText("Relay.StopStreamingReason"), destinationsBox);
		routeReason_->setWordWrap(true);
		routeReason_->setVisible(false);
		destinationsLayout->addWidget(routeReason_);
		destinationsLayout->addSpacing(3);
		ctd::eyebrowTitle(destinationsBox);
		layout->addWidget(destinationsBox, 1);

		/* overlays (H21, H22) */
		auto *overlaysBox = new QGroupBox(uiText("Relay.Overlays"), this);
		auto *overlaysLayout = new QVBoxLayout(overlaysBox);
		overlaysLayout->setSpacing(8);
		overlays_ = new QListWidget(overlaysBox);
		overlays_->setMinimumHeight(70);
		wrapLines(overlays_);
		overlaysLayout->addWidget(overlays_, 1);
		/* the selected overlay's scenes (meta.scenes): the whole overlay, or one scene as its own source */
		overlayScene_ = new QComboBox(overlaysBox);
		overlayScene_->setToolTip(uiText("Relay.Overlay.SceneTip"));
		overlayScene_->setVisible(false);
		overlaysLayout->addWidget(overlayScene_);
		/* stacked, not side by side, so the dock stays narrow enough for a side dock area */
		auto *overlayButtons = new QVBoxLayout();
		overlayButtons->setSpacing(6);
		addOverlay_ = new QPushButton(uiText("Relay.AddOverlay"), overlaysBox);
		addScenesByName_ = new QPushButton(uiText("Relay.AddScenesByName"), overlaysBox);
		addScenesByName_->setToolTip(uiText("Relay.AddScenesByNameTip"));
		addScenesByName_->setVisible(false);
		switchOverlays_ = new QPushButton(uiText("Relay.SwitchOverlays"), overlaysBox);
		ctd::button(addOverlay_);
		ctd::button(addScenesByName_);
		ctd::button(switchOverlays_);
		overlayButtons->addWidget(addOverlay_);
		overlayButtons->addWidget(addScenesByName_);
		overlayButtons->addWidget(switchOverlays_);
		overlaysLayout->addLayout(overlayButtons);
		template_ = new QLineEdit(overlaysBox);
		template_->setPlaceholderText(uiText("Relay.TemplatePlaceholder"));
		askCanvas_ = new QComboBox(overlaysBox);
		askCanvas_->addItem("landscape");
		askCanvas_->addItem("vertical");
		ask_ = new QPushButton(uiText("Relay.Ask"), overlaysBox);
		overlaysLayout->addWidget(template_);
		auto *askRow = new QHBoxLayout();
		askRow->addWidget(askCanvas_, 1);
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
		auto *chatBox = new QGroupBox(uiText("Relay.Chat"), this);
		auto *chatLayout = new QVBoxLayout(chatBox);
		chatLayout->setSpacing(8);
		chat_ = new QTextBrowser(chatBox);
		chat_->setOpenExternalLinks(true);
		chat_->setMinimumHeight(100);
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
			if (streamingBlocked()) {
				updateRoute();
				QMessageBox::information(this, uiText("Common.DialogTitle"),
							 uiText("Relay.StopStreamingReason"));
				return;
			}
			wantIngest_ = true;
			ms_request_ingest();
			updateRoute();
		});
		QObject::connect(addOverlay_, &QPushButton::clicked, this, [this] {
			obs_data_t *overlay = selectedOverlay();
			if (overlay)
				addOverlay(overlay, overlayScene_->count() > 0 ? overlayScene_->currentData().toString()
									       : QString());
			else
				QMessageBox::information(this, uiText("Common.DialogTitle"),
							 uiText("Relay.PickOverlayFirst"));
		});
		QObject::connect(addScenesByName_, &QPushButton::clicked, this, [this] {
			obs_data_t *overlay = selectedOverlay();
			if (overlay)
				addScenesByName(overlay);
			else
				QMessageBox::information(this, uiText("Common.DialogTitle"),
							 uiText("Relay.PickOverlayFirst"));
		});
		QObject::connect(overlays_, &QListWidget::currentRowChanged, this, [this] { showScenePicker(); });
		QObject::connect(switchOverlays_, &QPushButton::clicked, this, [this] { switchOverlays(); });
		QObject::connect(ask_, &QPushButton::clicked, this, [this] {
			const QString name = template_->text().trimmed();
			if (name.isEmpty()) {
				QMessageBox::information(this, uiText("Common.DialogTitle"),
							 uiText("Relay.TypeTemplate"));
				return;
			}
			wantCreated_ = true;
			ms_request_overlay(name.toUtf8().constData(), askCanvas_->currentText().toUtf8().constData());
		});

		obs_frontend_add_event_callback(frontendEvent, this);
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
			const QString server = server_->text().trimmed();
			if (server.isEmpty()) {
				/* there is no default server: pairing without one would wait forever */
				action_->setText(uiText("Relay.TypeAddressFirst").arg(MS_SERVER_EXAMPLE));
				server_->setFocus();
				return;
			}
			ms_set_server(server.toUtf8().constData());
			ms_start_pairing();
		}
	}

	void shareClicked(bool on)
	{
		if (on) {
			const QString server = server_->text().trimmed();
			QString detail = uiText("Relay.Share.Detail")
						 .arg(server.isEmpty() ? uiText("Relay.Share.ThisServer") : server);
			if (server.startsWith("https://", Qt::CaseInsensitive))
				detail += "\n\n" + uiText("Relay.Share.Https");
			else
				detail += "\n\n" + uiText("Relay.Share.Http");
			if (QMessageBox::question(this, uiText("Common.DialogTitle"),
						  detail + "\n\n" + uiText("Relay.Share.Question")) !=
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
		clock_->setText(uiText("Relay.Clock").arg(run, utc, local));
		clock_->setToolTip(uiText("Relay.Clock.Tooltip").arg(c.fps, 0, 'g', 4).arg(c.delay_ms / 2, 0, 'f', 0));
		clock_->show();
	}

	void refresh()
	{
		multitrackWarning_->setVisible(obsMultitrackEnabled());
		showProfiles_->refresh();
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
			updateRoute();
			applyIngest(s.ingest);
		}
		lastIngestSeq_ = s.ingest_seq;
		ms_snapshot_free(&s);
	}

	static void frontendEvent(enum obs_frontend_event event, void *data)
	{
		if (event != OBS_FRONTEND_EVENT_STREAMING_STARTING && event != OBS_FRONTEND_EVENT_STREAMING_STARTED &&
		    event != OBS_FRONTEND_EVENT_STREAMING_STOPPED)
			return;
		auto *self = static_cast<MultiStreamDock *>(data);
		QMetaObject::invokeMethod(self, [self] { self->updateRoute(); }, Qt::QueuedConnection);
	}

	/* OBS's main stream or the vertical canvas's is running: the button would change a live output */
	static bool streamingBlocked() { return obs_frontend_streaming_active() || ctdVerticalStreaming(); }

	void updateRoute()
	{
		const bool blocked = streamingBlocked();
		route_->setEnabled(connected_ && !wantIngest_ && !blocked);
		route_->setToolTip(blocked ? uiText("Relay.StopStreamingReason") : QString());
		routeReason_->setVisible(blocked);
	}

	void showConnection(const ms_snapshot &s)
	{
		if (!server_->hasFocus())
			server_->setText(QString::fromUtf8(s.server));
		QString text = QString::fromUtf8(s.message);
		if (s.link == MS_CONNECTED) {
			text += " · " + uiText(s.events_live ? "Relay.LiveUpdates" : "Relay.ReconnectingLiveUpdates");
			/* API v1.4: OBS's own events going to the hub */
			if (s.events_state == MS_EVENTS_OK)
				text += " · " + uiText("Relay.Events.Sent").arg(s.events_sent);
			else if (s.events_state == MS_EVENTS_FAILING)
				text += " · " + uiText("Relay.Events.Retrying");
			else if (s.events_state == MS_EVENTS_UNSUPPORTED)
				text += " · " + uiText("Relay.Events.Unsupported");
			else if (s.events_state == MS_EVENTS_NEED_SCOPE)
				text += "\n" + uiText("Relay.Events.NeedScope");
		}
		status_->setText(text);
		approveUrl_ = QString::fromUtf8(s.approve_url);
		approve_->setVisible(s.link == MS_PAIRING && !approveUrl_.isEmpty());
		connect_->setText(uiText(s.link != MS_DISCONNECTED ? "Relay.Disconnect" : "Relay.Connect"));
		const bool connected = s.link == MS_CONNECTED;
		shareObsws_->setChecked(s.share_obs_websocket);
		shareStatus_->setText(QString::fromUtf8(s.obs_websocket_message));
		shareStatus_->setVisible(s.link != MS_DISCONNECTED && *s.obs_websocket_message);
		profile_->setEnabled(connected && !s.profile_locked);
		connected_ = connected;
		updateRoute();
		addOverlay_->setEnabled(connected);
		addScenesByName_->setEnabled(connected);
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
			const QString label =
				uiText("Relay.ProfileItem")
					.arg(str(p, "name"), status.isEmpty() ? uiText("Relay.Offline") : status)
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
		/* only the parts that are there, so an untitled session doesn't start with a stray separator */
		QStringList parts;
		const QString title = str(session, "title").trimmed();
		if (!title.isEmpty())
			parts << QString("<b style='color:#f2fbff'>%1</b>").arg(title.toHtmlEscaped());
		if (state == "live")
			parts << QString("<b style='color:#ff00b8'>%1</b>").arg(uiText("Relay.Session.Live"));
		else if (!state.isEmpty())
			parts << state.toHtmlEscaped();
		if (obs_data_get_bool(session, "testMode"))
			parts << QString("<b style='color:#39e7ff'>%1</b>").arg(uiText("Relay.Session.Test"));
		/* API v1.3: every platform's viewers added up, kept current by stream.status; absent while unknown */
		if (obs_data_has_user_value(session, "viewers"))
			parts << uiText("Relay.Session.Watching")
					 .arg(QString("<b>%1</b>").arg(obs_data_get_int(session, "viewers")));
		obs_data_release(session);
		QString text = parts.join(" · ");

		/* per-feed bitrates, kept current by feed.status */
		obs_data_array_t *feeds = obs_data_get_array(s.status, "feeds");
		for (size_t i = 0; feeds && i < obs_data_array_count(feeds); i++) {
			obs_data_t *f = obs_data_array_item(feeds, i);
			text += QString("%1%2: %3")
					.arg(text.isEmpty() ? QString() : QString("<br>"))
					.arg(str(f, "feed").toHtmlEscaped(),
					     obs_data_get_bool(f, "online")
						     ? uiText("Common.Kbps").arg(obs_data_get_int(f, "bitrateKbps"))
						     : uiText("Relay.Offline"));
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
			line += down   ? uiText("Relay.Destination.Down")
				: live ? uiText("Relay.Destination.Live")
				       : str(d, "phase");
			obs_data_t *stats = obs_data_get_obj(d, "stats");
			if (stats)
				line += " · " + uiText("Relay.Destination.Stats")
							.arg(obs_data_get_int(stats, "bitrateKbps"))
							.arg(obs_data_get_double(stats, "fps"));
			obs_data_release(stats);
			if (obs_data_get_int(d, "viewers") > 0)
				line += " · " + uiText("Relay.Destination.Viewers").arg(obs_data_get_int(d, "viewers"));
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
		/* the list's own signals would rebuild the picker mid-refresh; it is rebuilt once at the end */
		overlays_->blockSignals(true);
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
			const size_t scenes = overlayScenes(o).size();
			if (scenes > 1)
				line += " · " + uiText("Relay.Overlay.ScenesCount").arg(scenes);
			if (!obs_data_get_bool(o, "appOnline"))
				line += " " + uiText("Relay.Overlay.Offline");
			auto *item = new QListWidgetItem(line, overlays_);
			item->setData(Qt::UserRole, str(o, "id"));
			if (str(o, "id") == selected)
				overlays_->setCurrentItem(item);
			overlayData_.emplace_back(o); /* owned until the next refresh */
		}
		overlays_->blockSignals(false);
		showScenePicker();
	}

	/* The picker under the list: "Whole overlay (follows the show)", then each scene of the selected overlay. Hidden
	 * when it has no scenes to choose from. Keeps the scene picked while the list refreshes. */
	void showScenePicker()
	{
		const QString picked = overlayScene_->currentData().toString();
		overlayScene_->blockSignals(true);
		overlayScene_->clear();
		obs_data_t *overlay = selectedOverlay();
		const std::vector<OverlayScene> scenes = overlay ? overlayScenes(overlay) : std::vector<OverlayScene>();
		const bool choice = scenes.size() > 1;
		if (choice) {
			overlayScene_->addItem(uiText("Relay.Overlay.WholeOverlay"), QString());
			for (const OverlayScene &scene : scenes) {
				overlayScene_->addItem(scene.name, scene.id);
				if (scene.id == picked)
					overlayScene_->setCurrentIndex(overlayScene_->count() - 1);
			}
		}
		overlayScene_->blockSignals(false);
		overlayScene_->setVisible(choice);
		addScenesByName_->setVisible(choice);
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
					joined = uiText("Relay.Chat.Joined").arg(str(user, "name"));
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
			QMessageBox::warning(this, uiText("Common.DialogTitle"), uiText("Relay.Ingest.NoAccess"));
			return;
		}
		if (streamingBlocked()) {
			QMessageBox::information(this, uiText("Common.DialogTitle"),
						 uiText("Relay.StopStreamingReason"));
			return;
		}
		const QString offered = str(ingest, "serverUrl");
		const QString server = reachableIngestServer(offered, server_->text());
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
			QMessageBox::warning(this, uiText("Common.DialogTitle"), uiText("Relay.Ingest.NoKey"));
			return;
		}
		/* the vertical feed's server: its full URL without the key (else the main server), with the same
		 * LAN-to-public host swap as the main stream */
		QString verticalServer = server;
		if (!verticalKey.isEmpty() && verticalUrl.endsWith("/" + verticalKey))
			verticalServer =
				reachableIngestServer(verticalUrl.chopped(verticalKey.size() + 1), server_->text());
		if (server != offered && !offered.isEmpty() && verticalUrl.startsWith(offered))
			verticalUrl = server + verticalUrl.mid(offered.size());
		const bool ctdVertical = !verticalKey.isEmpty() && ctdVerticalCanvasAvailable();

		QString question = uiText("Relay.Ingest.Question").arg(server, maskKey(landscapeKey));
		if (server != offered)
			question +=
				"\n" + uiText("Relay.Ingest.LanSwap").arg(QUrl(offered).host(), QUrl(server).host());
		if (ctdVertical)
			question +=
				"\n\n" + uiText("Relay.Ingest.CtdVertical").arg(verticalServer, maskKey(verticalKey));
		else if (!verticalKey.isEmpty())
			question += "\n\n" + uiText("Relay.Ingest.Aitum").arg(verticalServer, maskKey(verticalKey));
		question += "\n\n" + uiText("Relay.Ingest.Keyframe");
		if (!verticalKey.isEmpty())
			question += " " + uiText(ctdVertical ? "Relay.Ingest.KeyframeCtdVertical"
							     : "Relay.Ingest.KeyframeAitum");
		if (obsMultitrackEnabled())
			question += "\n\n" + uiText("Relay.Ingest.Warning").arg(uiText("Relay.MultitrackWarning"));
		if (QMessageBox::question(this, uiText("Common.DialogTitle"), question) != QMessageBox::Yes)
			return;

		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "server", server.toUtf8().constData());
		obs_data_set_string(settings, "key", landscapeKey.toUtf8().constData());
		obs_service_t *service = obs_service_create("rtmp_custom", "default_service", settings, nullptr);
		obs_data_release(settings);
		if (!service) {
			QMessageBox::warning(this, uiText("Common.DialogTitle"), uiText("Relay.Ingest.NoService"));
			return;
		}
		obs_frontend_set_streaming_service(service);
		obs_frontend_save_streaming_service();
		obs_service_release(service);

		QString verticalNote;
		bool verticalSet = false;
		bool clipboard = !ctdVertical && !verticalUrl.isEmpty();
		if (ctdVertical) {
			QString error;
			verticalSet = setCtdVerticalOutput(verticalServer, verticalKey, error);
			if (verticalSet) {
				verticalNote = " " + uiText("Relay.Ingest.VerticalSet");
			} else {
				blog(LOG_WARNING, "[CTD] relay: couldn't set the vertical canvas's stream: %s",
				     error.toUtf8().constData());
				clipboard = !verticalUrl.isEmpty();
				verticalNote =
					" " +
					uiText("Relay.Ingest.VerticalFailed")
						.arg(error, clipboard ? uiText("Relay.Ingest.VerticalFailedClipboard")
								      : QString());
			}
		} else if (clipboard) {
			verticalNote = " " + uiText("Relay.Ingest.VerticalClipboard");
		}
		if (clipboard)
			QApplication::clipboard()->setText(verticalUrl);
		const bool keyframeSet = setMainKeyframe2s();
		blog(LOG_INFO, "[CTD] relay: OBS's main stream now goes to the chosen profile%s",
		     verticalSet ? ", and the vertical canvas's too" : "");
		action_->setText(uiText("Relay.Ingest.Done") + verticalNote +
				 (keyframeSet ? " " + uiText("Relay.Ingest.KeyframeSet") : QString()));
	}

	obs_data_t *selectedOverlay() const
	{
		const int row = overlays_->currentRow();
		return row >= 0 && row < (int)overlayData_.size() ? overlayData_[row].get() : nullptr;
	}

	/* scene: the one of meta.scenes the source shows; empty = the whole overlay, which follows its show */
	static void setOverlaySettings(obs_source_t *source, obs_data_t *overlay, const QString &scene)
	{
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "url", overlaySceneUrl(overlay, scene).c_str());
		obs_data_set_int(settings, "width", obs_data_get_int(overlay, "width"));
		obs_data_set_int(settings, "height", obs_data_get_int(overlay, "height"));
		obs_source_update(source, settings);
		obs_data_release(settings);

		obs_data_t *priv = obs_source_get_private_settings(source);
		obs_data_set_string(priv, kOverlaySlot, overlaySlot(overlay).toUtf8().constData());
		obs_data_set_string(priv, kOverlayCanvas, overlayCanvas(overlay).toUtf8().constData());
		obs_data_set_string(priv, kOverlayName, obs_data_get_string(overlay, "name"));
		obs_data_set_string(priv, kOverlayApp, obs_data_get_string(overlay, "appId"));
		obs_data_set_string(priv, kOverlayScene, scene.toUtf8().constData());
		obs_data_release(priv);
	}

	/* the name of the overlay's scene `id`, or the id itself */
	static QString sceneName(obs_data_t *overlay, const QString &id)
	{
		for (const OverlayScene &scene : overlayScenes(overlay))
			if (scene.id == id)
				return scene.name;
		return id;
	}

	/* A browser source showing the overlay (or one of its scenes), at the overlay's size, top-left in `scene`. Named
	 * after the overlay ("CTD · <overlay> · <scene>" for a scene), with a number added when the name is taken.
	 * Returns the name it got, or an empty string. */
	static std::string addOverlaySource(obs_scene_t *scene, obs_data_t *overlay, const QString &sceneId)
	{
		const std::string overlayName = obs_data_get_string(overlay, "name");
		const std::string fallback = obs_module_text("Relay.Overlay.DefaultName");
		std::string base = overlayName.empty() ? fallback : overlayName;
		if (!sceneId.isEmpty())
			base = uiText("Relay.Overlay.SceneSourceName")
				       .arg(QString::fromStdString(base), sceneName(overlay, sceneId))
				       .toStdString();
		/* a name no other source has */
		std::string name = base;
		for (int n = 2;; n++) {
			obs_source_t *existing = obs_get_source_by_name(name.c_str());
			if (!existing)
				break;
			obs_source_release(existing);
			name = base + " " + std::to_string(n);
		}
		obs_data_t *settings = obs_data_create();
		obs_source_t *source = obs_source_create("browser_source", name.c_str(), settings, nullptr);
		obs_data_release(settings);
		if (!source)
			return std::string();
		setOverlaySettings(source, overlay, sceneId);
		/* top-left at 1:1, so an overlay the canvas's size covers it exactly. Left to OBS, a browser source that
		 * hasn't loaded yet is 0×0 and gets centred, leaving most of it off the canvas. */
		obs_sceneitem_t *item = obs_scene_add(scene, source);
		if (item) {
			const vec2 origin = {0.0f, 0.0f};
			obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
			obs_sceneitem_set_pos(item, &origin);
		}
		obs_source_release(source);
		return name;
	}

	/* H21: a browser source at the overlay's size, in the current scene of the canvas meta.canvas names (the main
	 * canvas for landscape, the vertical canvas for vertical). sceneId: one of its scenes (meta.scenes), pinned with
	 * scene=<id>; empty = the whole overlay, which follows its show. */
	void addOverlay(obs_data_t *overlay, const QString &sceneId = QString())
	{
		const bool vertical = overlayCanvas(overlay) == "vertical";
		obs_source_t *sceneSource = nullptr;
		QString where = uiText("Relay.Overlay.CurrentScene");
		if (vertical) {
			obs_canvas_t *canvas = findVerticalCanvas();
			if (canvas) {
				sceneSource = canvasCurrentScene(canvas);
				where = uiText("Relay.Overlay.CanvasScene")
						.arg(QString::fromUtf8(obs_canvas_get_name(canvas)));
				obs_canvas_release(canvas);
			}
			if (!sceneSource) {
				action_->setText(uiText("Relay.Overlay.NoVerticalCanvas"));
				where = uiText("Relay.Overlay.MainScene");
			}
		}
		if (!sceneSource)
			sceneSource = obs_frontend_get_current_scene();
		obs_scene_t *scene = obs_scene_from_source(sceneSource);
		if (!scene) {
			obs_source_release(sceneSource);
			return;
		}
		const std::string name = addOverlaySource(scene, overlay, sceneId);
		if (!name.empty()) {
			blog(LOG_INFO, "[CTD] relay: added overlay '%s'%s%s to %s", name.c_str(),
			     sceneId.isEmpty() ? "" : ", scene ", sceneId.toUtf8().constData(),
			     where.toUtf8().constData());
			if (!vertical || where != uiText("Relay.Overlay.MainScene"))
				action_->setText(
					uiText("Relay.Overlay.Added").arg(QString::fromStdString(name), where));
		}
		obs_source_release(sceneSource);
	}

	/* Is a source in `scene` already showing this overlay's scene `sceneId` (same slot and canvas, else same name)? */
	static bool sceneHasOverlay(obs_scene_t *scene, obs_data_t *overlay, const QString &sceneId)
	{
		struct Find {
			QString slot, canvas, name, scene;
			bool found = false;
		} find{overlaySlot(overlay), overlayCanvas(overlay), str(overlay, "name"), sceneId};
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				auto *f = static_cast<Find *>(param);
				obs_source_t *source = obs_sceneitem_get_source(item);
				if (strcmp(obs_source_get_unversioned_id(source), "browser_source") != 0)
					return true;
				obs_data_t *priv = obs_source_get_private_settings(source);
				const QString slot = str(priv, kOverlaySlot);
				const bool same = (!slot.isEmpty() && !f->slot.isEmpty()
							   ? slot == f->slot && str(priv, kOverlayCanvas) == f->canvas
							   : str(priv, kOverlayName) == f->name) &&
						  str(priv, kOverlayScene) == f->scene;
				obs_data_release(priv);
				f->found = same;
				return !same;
			},
			&find);
		return find.found;
	}

	/* Each of the overlay's scenes into the OBS scenes of its canvas with the same name ("Starting Soon" for the
	 * scene "Starting Soon" or "starting"; letters and digits only, any case). An OBS scene that already shows that
	 * scene of the overlay is left alone. */
	void addScenesByName(obs_data_t *overlay)
	{
		const std::vector<OverlayScene> scenes = overlayScenes(overlay);
		if (scenes.size() < 2) {
			action_->setText(uiText("Relay.Overlay.NoScenes"));
			return;
		}
		/* the OBS scenes of the overlay's canvas: the main one's, or the vertical canvas's */
		struct Collect {
			std::vector<obs_source_t *> scenes; /* strong references */
		} collect;
		const bool vertical = overlayCanvas(overlay) == "vertical";
		obs_canvas_t *canvas = vertical ? findVerticalCanvas() : nullptr;
		if (vertical && !canvas) {
			action_->setText(uiText("Relay.Overlay.NoVerticalCanvasByName"));
			return;
		}
		if (canvas) {
			obs_canvas_enum_scenes(
				canvas,
				[](void *param, obs_source_t *source) {
					static_cast<Collect *>(param)->scenes.push_back(obs_source_get_ref(source));
					return true;
				},
				&collect);
			obs_canvas_release(canvas);
		} else {
			struct obs_frontend_source_list list = {};
			obs_frontend_get_scenes(&list);
			for (size_t i = 0; i < list.sources.num; i++)
				collect.scenes.push_back(obs_source_get_ref(list.sources.array[i]));
			obs_frontend_source_list_free(&list);
		}

		int added = 0, already = 0;
		QStringList missing;
		for (const OverlayScene &scene : scenes) {
			const QByteArray id = scene.id.toUtf8(), name = scene.name.toUtf8();
			bool matched = false;
			for (obs_source_t *source : collect.scenes) {
				obs_scene_t *obsScene = obs_scene_from_source(source);
				if (!source || !obsScene ||
				    !ctd_overlay_scene_matches(obs_source_get_name(source), id.constData(),
							       name.constData()))
					continue;
				matched = true;
				if (sceneHasOverlay(obsScene, overlay, scene.id)) {
					already++;
					continue;
				}
				const std::string added_name = addOverlaySource(obsScene, overlay, scene.id);
				if (added_name.empty())
					continue;
				added++;
				blog(LOG_INFO, "[CTD] relay: added overlay '%s', scene %s, to the OBS scene '%s'",
				     added_name.c_str(), id.constData(), obs_source_get_name(source));
			}
			if (!matched)
				missing << scene.name;
		}
		for (obs_source_t *source : collect.scenes)
			obs_source_release(source);

		QString text = added ? uiText("Relay.Overlay.ScenesByNameAdded").arg(added)
				     : uiText("Relay.Overlay.ScenesByNameNone");
		if (already)
			text += " " + uiText("Relay.Overlay.ScenesByNameAlready").arg(already);
		if (!missing.isEmpty())
			text += " " + uiText("Relay.Overlay.ScenesByNameMissing").arg(missing.join(", "));
		action_->setText(text);
	}

	/* H21: browser sources showing one of the hub's overlays now show this profile's equivalent (meta.slot +
	 * meta.canvas; by name when the overlay has no slot), on the same scene of it (ctd_overlay_scene). A source
	 * pinned to a scene the equivalent overlay doesn't have is left as it is. Never deletes anything. */
	void switchOverlays()
	{
		struct Match {
			MultiStreamDock *dock;
			int switched = 0;
			int kept = 0;
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
				const QString scene = str(priv, kOverlayScene);
				obs_data_release(priv);
				if (slot.isEmpty() && name.isEmpty())
					return true; /* not one of ours */
				for (const auto &o : m->dock->overlayData_) {
					const QString oSlot = overlaySlot(o.get());
					const bool same = !slot.isEmpty() && !oSlot.isEmpty()
								  ? oSlot == slot && overlayCanvas(o.get()) == canvas
								  : str(o.get(), "name") == name;
					if (!same)
						continue;
					bool hasScene = scene.isEmpty();
					for (const OverlayScene &s : overlayScenes(o.get()))
						hasScene = hasScene || s.id == scene;
					if (hasScene) {
						setOverlaySettings(source, o.get(), scene);
						m->switched++;
					} else {
						m->kept++;
					}
					break;
				}
				return true;
			},
			&match);
		QString text = match.switched ? uiText("Relay.Overlay.Switched").arg(match.switched)
					      : uiText("Relay.Overlay.NoneToSwitch");
		if (match.kept)
			text += " " + uiText("Relay.Overlay.SceneKept").arg(match.kept);
		if (match.kept)
			blog(LOG_INFO,
			     "[CTD] relay: kept %d overlay source(s): their scene isn't in this profile's overlay",
			     match.kept);
		action_->setText(text);
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
	QLabel *multitrackWarning_;
	QPushButton *approve_;
	QCheckBox *shareObsws_;
	QLabel *shareStatus_;
	QComboBox *profile_;
	ShowProfilesSection *showProfiles_;
	QLabel *session_;
	QLabel *clock_;
	QListWidget *destinations_;
	QListWidget *alerts_;
	QPushButton *route_;
	QLabel *routeReason_;
	bool connected_ = false;
	QListWidget *overlays_;
	QComboBox *overlayScene_;
	QPushButton *addOverlay_;
	QPushButton *addScenesByName_;
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
	if (!obs_frontend_add_dock_by_id("ctd-multistream", obs_module_text("Relay.Title"), dock))
		delete dock;
}
