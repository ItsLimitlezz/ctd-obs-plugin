/*
 * The CTD Hub dock (SPEC 5.3, H9 to H12, H14): the streams on a MediaMTX server profile, live, with what each is and
 * how its link is doing, and one click to put them in OBS.
 *
 * It reads the profile's shared poller (src/api/poller.h) once a second and never calls the API itself. Every OBS
 * change (sources, scene items) happens here on the UI thread, where the frontend API wants it.
 */

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QDesktopServices>
#include <QUrl>
#include <QHeaderView>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../api/poller.h"
#include "../obs/issues.h"
#include "../obs/sync-groups.h"
#include "../obs/servers.h"
#include "icons.hpp"

namespace {

constexpr int kHistory = 60; /* bitrate samples kept per stream for its sparkline (2 minutes at 2 s) */

enum Column { ColLive, ColName, ColPublisher, ColTracks, ColBitrate, ColLink, ColIssues, ColInObs, ColCount };

QString qs(const char *value)
{
	return QString::fromUtf8(value ? value : "");
}

/* "srtConn" → "SRT" */
QString protocolName(const QString &type)
{
	if (type.startsWith("srt"))
		return "SRT";
	if (type.startsWith("rtmp"))
		return "RTMP";
	if (type.startsWith("rtsp"))
		return "RTSP";
	if (type.startsWith("webRTC"))
		return "WebRTC";
	if (type.startsWith("hls"))
		return "HLS";
	return type;
}

/* the bitrate history as a small line, the latest value written over it */
class Sparkline : public QWidget {
public:
	Sparkline(const std::deque<double> &values, QWidget *parent) : QWidget(parent), values_(values)
	{
		setMinimumSize(110, 22);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const double top = values_.empty() ? 0.0 : *std::max_element(values_.begin(), values_.end());
		if (values_.size() > 1 && top > 0) {
			QPainterPath line;
			const double step = (double)width() / (double)(kHistory - 1);
			const double x0 = width() - step * (double)(values_.size() - 1);
			for (size_t i = 0; i < values_.size(); i++) {
				const QPointF point(x0 + step * (double)i,
						    height() - 2 - (values_[i] / top) * (height() - 4));
				if (i == 0)
					line.moveTo(point);
				else
					line.lineTo(point);
			}
			p.setPen(QPen(ctd::cyan(), 1.5));
			p.drawPath(line);
		}
		const double now = values_.empty() ? 0.0 : values_.back();
		p.setPen(ctd::inkBody());
		p.drawText(rect().adjusted(2, 0, -2, 0), Qt::AlignRight | Qt::AlignVCenter,
			   now >= 1000 ? QString("%1 Mbps").arg(now / 1000.0, 0, 'f', 1)
				       : QString("%1 kbps").arg((int)std::lround(now)));
	}

private:
	std::deque<double> values_;
};

/* CTD sources in OBS: what they read, and the scenes they are in */
struct InObs {
	std::string source; /* the first source on this path, for Reconnect and Properties */
	std::set<std::string> scenes;
};

class HubDock : public QWidget {
public:
	explicit HubDock(QWidget *parent) : QWidget(parent)
	{
		ctd::applyTheme(this);
		auto *layout = new QVBoxLayout(this);
		layout->setSpacing(8);
		layout->addLayout(ctdHeader(this, "obs-plugin-64.png", "CTD Hub"));

		/* H9: the profile and its status */
		auto *top = new QHBoxLayout();
		profile_ = new QComboBox(this);
		auto *manage = new QPushButton("Manage…", this);
		/* S16: the sync clip, for a source's sync check; opens in the browser, to show on a phone or a screen */
		auto *clip = new QPushButton("Sync clip", this);
		clip->setToolTip(
			"A white flash and a beep once a second. Show it in front of a camera, then run the sync "
			"check in that source's properties.");
		connect(clip, &QPushButton::clicked, this, [] {
			char *file = obs_module_file("sync-clip.html");
			if (file)
				QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromUtf8(file)));
			bfree(file);
		});
		ctd::button(manage);
		ctd::button(clip);
		top->addWidget(profile_, 1);
		top->addWidget(manage);
		top->addWidget(clip);
		layout->addLayout(top);
		status_ = new QLabel(this);
		status_->setWordWrap(true);
		status_->setProperty("ctdRole", "muted");
		layout->addWidget(status_);

		/* H12 */
		auto *addAllRow = new QHBoxLayout();
		addAll_ = new QPushButton("Add all live streams", this);
		grid_ = new QComboBox(this);
		grid_->addItem("as they are", 0);
		grid_->addItem("in a 2×2 grid", 2);
		grid_->addItem("in a 3×3 grid", 3);
		grid_->addItem("in a 4×4 grid", 4);
		ctd::button(addAll_, ctd::Role::Primary);
		addAllRow->addWidget(addAll_);
		addAllRow->addWidget(grid_, 1);
		layout->addLayout(addAllRow);

		/* H13: off unless ticked, per profile */
		auto *autoRow = new QHBoxLayout();
		autoAdd_ = new QCheckBox("Auto-add new live streams to", this);
		autoScene_ = new QComboBox(this);
		autoRow->addWidget(autoAdd_);
		autoRow->addWidget(autoScene_, 1);
		layout->addLayout(autoRow);

		/* H8: only when the profile allows changing the server */
		createPath_ = ctd::button(new QPushButton("Create path…", this), ctd::Role::Hot);
		layout->addWidget(createPath_);
		action_ = new QLabel(this);
		action_->setWordWrap(true);
		layout->addWidget(action_);

		/* H10 */
		table_ = new QTableWidget(0, ColCount, this);
		table_->setHorizontalHeaderLabels(
			{"", "Stream", "Publisher", "Tracks", "Bitrate", "Link", "Issues", "In OBS"});
		table_->verticalHeader()->setVisible(false);
		table_->setSelectionBehavior(QAbstractItemView::SelectRows);
		table_->setSelectionMode(QAbstractItemView::SingleSelection);
		table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
		table_->setContextMenuPolicy(Qt::CustomContextMenu);
		table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
		table_->horizontalHeader()->setSectionResizeMode(ColInObs, QHeaderView::Stretch);
		table_->horizontalHeader()->setFont(ctd::font(ctd::Face::Mono, 10, QFont::Bold, true, 1.0));
		table_->setShowGrid(false);
		layout->addWidget(table_, 1);

		/* D2: the issues panel; hover one for its likely cause */
		layout->addWidget(ctd::eyebrow("Issues", this));
		issues_ = new QListWidget(this);
		issues_->setMaximumHeight(110);
		issues_->setWordWrap(true);
		issues_->setContextMenuPolicy(Qt::CustomContextMenu);
		layout->addWidget(issues_);
		connect(issues_, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
			QListWidgetItem *item = issues_->itemAt(pos);
			if (!item)
				return;
			QMenu menu(this);
			for (const QString &action : item->data(Qt::UserRole + 1).toStringList()) {
				const QString id = action.section('\t', 0, 0), label = action.section('\t', 1);
				const QString issue = item->data(Qt::UserRole).toString();
				menu.addAction(label, [this, issue, id] { applyFix(issue, id); });
			}
			if (!menu.isEmpty())
				menu.exec(issues_->viewport()->mapToGlobal(pos));
		});
		connect(issues_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
			const QStringList actions = item->data(Qt::UserRole + 1).toStringList();
			if (!actions.isEmpty())
				applyFix(item->data(Qt::UserRole).toString(), actions.first().section('\t', 0, 0));
		});

		/* S15: the sync groups, each with its members' lock state, timecode, offset and delay; hidden while none */
		syncLabel_ = ctd::eyebrow("Sync groups", this);
		layout->addWidget(syncLabel_);
		sync_ = new QTreeWidget(this);
		sync_->setColumnCount(5);
		sync_->setHeaderLabels({"Group / source", "State", "Timecode", "Offset", "Delay"});
		sync_->setRootIsDecorated(true);
		sync_->setMaximumHeight(140);
		sync_->header()->setStretchLastSection(false);
		sync_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
		sync_->header()->setFont(ctd::font(ctd::Face::Mono, 10, QFont::Bold, true, 1.0));
		layout->addWidget(sync_);
		syncLabel_->hide();
		sync_->hide();
		auto *syncTimer = new QTimer(this);
		connect(syncTimer, &QTimer::timeout, this, [this] { showSyncGroups(); });
		syncTimer->start(500);

		connect(profile_, QOverload<int>::of(&QComboBox::activated), this, [this](int) { switchProfile(); });
		connect(manage, &QPushButton::clicked, this, [this] {
			ctd_servers_dialog_open();
			loadProfiles();
		});
		connect(addAll_, &QPushButton::clicked, this, [this] { addAllLive(); });
		connect(autoAdd_, &QCheckBox::toggled, this, [this](bool) { saveAutoAdd(); });
		connect(autoScene_, QOverload<int>::of(&QComboBox::activated), this, [this](int) { saveAutoAdd(); });
		connect(createPath_, &QPushButton::clicked, this, [this] { createPath(); });
		connect(table_, &QTableWidget::customContextMenuRequested, this,
			[this](const QPoint &pos) { rowMenu(pos); });
		connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
			const std::string name = rowName(row);
			if (!name.empty())
				addToScene(name, currentScene(), 0, 0, 0);
		});

		auto *timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, [this] { refresh(); });
		timer->start(1000);
		loadProfiles();
		/* a profile added, changed or removed anywhere (Tools ▸ CTD Live servers) shows here at once */
		servers_watch(serversChanged, this);
	}

	~HubDock() override
	{
		servers_unwatch(serversChanged, this);
		mtx_poller_release(poller_);
	}

private:
	/* ------------------------------------------------------- profiles -- */

	static void serversChanged(void *param, const char *)
	{
		QPointer<HubDock> self = static_cast<HubDock *>(param);
		QMetaObject::invokeMethod(
			qApp,
			[self] {
				if (self)
					self->loadProfiles();
			},
			Qt::QueuedConnection);
	}

	void loadProfiles()
	{
		const QString keep = profile_->currentData().toString();
		profile_->clear();
		int select = -1;
		for (size_t i = 0; i < servers_count(); i++) {
			struct ctd_server s;
			if (!servers_get_index(i, &s))
				continue;
			profile_->addItem(QString("%1 (%2)").arg(qs(s.name), qs(s.host)), qs(s.id));
			if (qs(s.id) == keep || (keep.isEmpty() && s.is_default))
				select = profile_->count() - 1;
			ctd_server_free(&s);
		}
		if (select < 0 && profile_->count())
			select = 0;
		profile_->setCurrentIndex(select);
		switchProfile();
	}

	void switchProfile()
	{
		const std::string id = profile_->currentData().toString().toStdString();
		if (id == profileId_)
			return;
		profileId_ = id;
		mtx_poller_release(poller_);
		poller_ = mtx_poller_acquire(id.c_str());
		lastSeq_ = 0;
		autoSeq_ = 0;
		seenLive_.clear();
		seenInit_ = false;
		history_.clear();
		table_->setRowCount(0);
		loadAutoAdd();
		refresh();
	}

	/* ------------------------------------------------------- H13 auto-add -- */

	static char *hubConfigFile()
	{
		char *dir = obs_module_config_path("");
		if (dir) {
			os_mkdirs(dir);
			bfree(dir);
		}
		return obs_module_config_path("hub.json");
	}

	void fillScenes(const QString &keep)
	{
		autoScene_->clear();
		struct obs_frontend_source_list scenes = {};
		obs_frontend_get_scenes(&scenes);
		for (size_t i = 0; i < scenes.sources.num; i++)
			autoScene_->addItem(qs(obs_source_get_name(scenes.sources.array[i])));
		obs_frontend_source_list_free(&scenes);
		const int at = autoScene_->findText(keep);
		if (at >= 0)
			autoScene_->setCurrentIndex(at);
		else if (!keep.isEmpty()) {
			autoScene_->insertItem(0, keep); /* a scene from another collection */
			autoScene_->setCurrentIndex(0);
		}
	}

	void loadAutoAdd()
	{
		char *file = hubConfigFile();
		obs_data_t *root = file ? obs_data_create_from_json_file_safe(file, "bak") : nullptr;
		bfree(file);
		obs_data_t *profiles = root ? obs_data_get_obj(root, "profiles") : nullptr;
		obs_data_t *mine = profiles ? obs_data_get_obj(profiles, profileId_.c_str()) : nullptr;
		autoAdd_->blockSignals(true);
		autoAdd_->setChecked(mine && obs_data_get_bool(mine, "auto_add"));
		autoAdd_->blockSignals(false);
		fillScenes(mine ? qs(obs_data_get_string(mine, "scene")) : QString());
		obs_data_release(mine);
		obs_data_release(profiles);
		obs_data_release(root);

		struct ctd_server s;
		const bool found = servers_get(profileId_.c_str(), &s);
		createPath_->setVisible(found && s.allow_writes);
		if (found)
			ctd_server_free(&s);
	}

	void saveAutoAdd()
	{
		char *file = hubConfigFile();
		if (!file)
			return;
		obs_data_t *root = obs_data_create_from_json_file_safe(file, "bak");
		if (!root)
			root = obs_data_create();
		obs_data_t *profiles = obs_data_get_obj(root, "profiles");
		if (!profiles) {
			profiles = obs_data_create();
			obs_data_set_obj(root, "profiles", profiles);
		}
		obs_data_t *mine = obs_data_create();
		obs_data_set_bool(mine, "auto_add", autoAdd_->isChecked());
		obs_data_set_string(mine, "scene", autoScene_->currentText().toUtf8().constData());
		obs_data_set_obj(profiles, profileId_.c_str(), mine);
		obs_data_save_json_safe(root, file, "tmp", "bak");
		obs_data_release(mine);
		obs_data_release(profiles);
		obs_data_release(root);
		bfree(file);
	}

	/* A path that goes live after the dock has seen the server once gets a source in the chosen scene, unless OBS
	 * already reads it. Never removes anything. */
	void autoAdd(const struct mtx_poll_snapshot &snap)
	{
		if (snap.status != MTX_API_OK)
			return;
		std::set<std::string> live;
		for (size_t i = 0; i < snap.count; i++) {
			if (snap.items[i].live)
				live.insert(snap.items[i].name);
		}
		if (!seenInit_) {
			seenLive_ = live; /* what was live already isn't new */
			seenInit_ = true;
			return;
		}
		std::vector<std::string> fresh;
		for (const std::string &name : live) {
			if (!seenLive_.count(name))
				fresh.push_back(name);
		}
		seenLive_ = live;
		if (fresh.empty() || !autoAdd_->isChecked() || autoScene_->currentText().isEmpty())
			return;
		inObs_ = findInObs();
		for (const std::string &name : fresh) {
			if (inObs_.count(name))
				continue;
			obs_source_t *scene = obs_get_source_by_name(autoScene_->currentText().toUtf8().constData());
			if (!scene)
				return;
			blog(LOG_INFO, "[ctd-live] CTD Hub: '%s' went live; adding it to '%s'", name.c_str(),
			     obs_source_get_name(scene));
			addToScene(name, scene, 0, 0, 0);
			inObs_ = findInObs();
		}
	}

	/* ------------------------------------------------- H8 config writes -- */

	/* the profile's API, for a write; false (and says why) when the profile doesn't allow writes */
	bool writeTarget(std::string &host, int &port, std::string &user, std::string &pass, std::string &token)
	{
		struct ctd_server s;
		if (!servers_get(profileId_.c_str(), &s))
			return false;
		char api_host[256];
		const bool ok = s.allow_writes && ctd_server_api_address(&s, api_host, sizeof(api_host), &port);
		if (ok) {
			host = api_host;
			user = s.api_user;
			pass = s.api_pass;
			token = s.api_token;
		} else {
			action_->setText(s.allow_writes ? "This profile has no API URL."
							: "This profile doesn't allow changing the server (Manage…).");
		}
		ctd_server_free(&s);
		return ok;
	}

	/* runs one write off the UI thread; the outcome lands in the action line and the log */
	template<typename Call> void runWrite(const QString &doing, const QString &done, Call call)
	{
		std::string host, user, pass, token;
		int port = 0;
		if (!writeTarget(host, port, user, pass, token))
			return;
		action_->setText(doing);
		QPointer<QLabel> label = action_;
		QPointer<HubDock> self = this;
		std::thread([=] {
			const struct mtx_api_server api = {host.c_str(), port,    user.c_str(), pass.c_str(),
							   nullptr,      nullptr, token.c_str()};
			const enum mtx_api_status status = call(api);
			const QString text =
				status == MTX_API_OK             ? done
				: status == MTX_API_UNAUTHORIZED ? "The server refused: this API login can't change it."
				: status == MTX_API_NOT_FOUND    ? "The server doesn't have that."
								 : "The server refused or couldn't be reached.";
			blog(status == MTX_API_OK ? LOG_INFO : LOG_WARNING, "[ctd-live] CTD Hub: %s %s",
			     done.toUtf8().constData(), status == MTX_API_OK ? "(done)" : "(failed)");
			QMetaObject::invokeMethod(
				qApp,
				[label, self, text] {
					if (label)
						label->setText(text);
					if (self)
						mtx_poller_refresh(self->poller_);
				},
				Qt::QueuedConnection);
		}).detach();
	}

	void kickPublisher(const std::string &path)
	{
		const auto info = publishers_.find(path);
		if (info == publishers_.end() || info->second.second.empty())
			return;
		if (QMessageBox::question(this, "CTD Hub",
					  QString("Disconnect the publisher of \"%1\"? A camera app normally reconnects "
						  "on its own.")
						  .arg(QString::fromStdString(path))) != QMessageBox::Yes)
			return;
		const std::string type = info->second.first, id = info->second.second;
		runWrite(QString("Kicking the publisher of \"%1\"…").arg(QString::fromStdString(path)),
			 QString("Kicked the publisher of \"%1\".").arg(QString::fromStdString(path)),
			 [type, id](const struct mtx_api_server &api) {
				 return mtx_api_kick(&api, type.c_str(), id.c_str());
			 });
	}

	void removePath(const std::string &path)
	{
		if (QMessageBox::question(
			    this, "CTD Hub",
			    QString("Remove \"%1\" from the server's configuration? (Until MediaMTX restarts, "
				    "for paths in mediamtx.yml.)")
				    .arg(QString::fromStdString(path))) != QMessageBox::Yes)
			return;
		runWrite(QString("Removing \"%1\"…").arg(QString::fromStdString(path)),
			 QString("Removed \"%1\".").arg(QString::fromStdString(path)),
			 [path](const struct mtx_api_server &api) { return mtx_api_delete_path(&api, path.c_str()); });
	}

	void createPath()
	{
		bool ok = false;
		const QString name =
			QInputDialog::getText(this, "CTD Hub",
					      "New path name (letters, digits, _ - . ~ /):", QLineEdit::Normal,
					      QString(), &ok)
				.trimmed();
		if (!ok || name.isEmpty())
			return;
		const std::string path = name.toStdString();
		if (!mtx_path_name_valid(path.c_str())) {
			action_->setText("That isn't a valid path name.");
			return;
		}
		runWrite(QString("Creating \"%1\"…").arg(name), QString("Created \"%1\".").arg(name),
			 [path](const struct mtx_api_server &api) {
				 return mtx_api_add_path(&api, path.c_str(), false);
			 });
	}

	/* ----------------------------------------------------------- view -- */

	std::map<std::string, InObs> findInObs()
	{
		struct Find {
			std::string profile;
			std::string host; /* a source on "Custom" with the profile's address counts too */
			long long port;
			std::map<std::string, InObs> byPath;
			std::map<std::string, std::string> sourceToPath;
		} find{profileId_, "", 0, {}, {}};
		struct ctd_server profile;
		if (servers_get(profileId_.c_str(), &profile)) {
			find.host = profile.host;
			find.port = profile.srt_port;
			ctd_server_free(&profile);
		}

		obs_enum_sources(
			[](void *param, obs_source_t *source) {
				auto *f = static_cast<Find *>(param);
				const char *id = obs_source_get_unversioned_id(source);
				if (strcmp(id, "ctd_live_source") != 0 && strcmp(id, "ctd_live_audio_source") != 0)
					return true;
				obs_data_t *settings = obs_source_get_settings(source);
				const std::string server = obs_data_get_string(settings, "server");
				const std::string path = obs_data_get_string(settings, "path");
				const bool same_address = server.empty() && !f->host.empty() &&
							  f->host == obs_data_get_string(settings, "host") &&
							  f->port == obs_data_get_int(settings, "port");
				obs_data_release(settings);
				if ((server == f->profile || same_address) && !path.empty()) {
					InObs &in = f->byPath[path];
					if (in.source.empty())
						in.source = obs_source_get_name(source);
					f->sourceToPath[obs_source_get_name(source)] = path;
				}
				return true;
			},
			&find);

		auto scan = [](void *param, obs_source_t *scene_source) {
			auto *f = static_cast<Find *>(param);
			obs_scene_t *scene = obs_scene_from_source(scene_source);
			for (const auto &[source, path] : f->sourceToPath) {
				if (obs_scene_find_source_recursive(scene, source.c_str()))
					f->byPath[path].scenes.insert(obs_source_get_name(scene_source));
			}
			return true;
		};
		obs_enum_scenes(scan, &find);
		obs_enum_canvases(
			[](void *param, obs_canvas_t *canvas) {
				obs_canvas_t *main_canvas = obs_get_main_canvas();
				const bool is_main = canvas == main_canvas;
				obs_canvas_release(main_canvas);
				if (!is_main) /* the main canvas's scenes came from obs_enum_scenes */
					obs_canvas_enum_scenes(
						canvas,
						[](void *p, obs_source_t *s) {
							auto *f = static_cast<Find *>(p);
							obs_scene_t *scene = obs_scene_from_source(s);
							for (const auto &[source, path] : f->sourceToPath) {
								if (obs_scene_find_source_recursive(scene,
												    source.c_str()))
									f->byPath[path].scenes.insert(
										obs_source_get_name(s));
							}
							return true;
						},
						param);
				return true;
			},
			&find);
		return find.byPath;
	}

	void refresh()
	{
		if (!poller_)
			return;
		struct mtx_poll_snapshot snap;
		mtx_poller_snapshot(poller_, &snap);
		/* H13 works while the dock is hidden; the table only while it shows */
		if (snap.seq != autoSeq_) {
			autoSeq_ = snap.seq;
			autoAdd(snap);
		}
		/* D2: an issue raised or cleared redraws too */
		const uint64_t issueSeq = issues_seq();
		if (issueSeq != lastIssueSeq_) {
			lastIssueSeq_ = issueSeq;
			lastSeq_ = 0;
		}
		if (!isVisible() || snap.seq == lastSeq_) {
			mtx_poll_snapshot_free(&snap);
			return;
		}
		lastSeq_ = snap.seq;
		showStatus(snap);
		struct issue_list issues;
		issues_get(&issues);
		showIssues(issues);

		const std::string selected = rowName(table_->currentRow());
		const std::map<std::string, InObs> inObs = findInObs();
		inObs_ = inObs;
		live_.clear();
		publishers_.clear();
		std::set<std::string> listed;

		table_->setRowCount(0);
		for (size_t i = 0; i < snap.count; i++) {
			const struct mtx_stream &s = snap.items[i];
			listed.insert(s.name);
			publishers_[s.name] = {s.publisher ? s.publisher : "", s.publisher_id ? s.publisher_id : ""};
			std::deque<double> &h = history_[s.name];
			h.push_back(s.live ? s.bitrate_kbps : 0.0);
			while ((int)h.size() > kHistory)
				h.pop_front();
			if (s.live)
				live_.push_back(s.name);

			const int row = table_->rowCount();
			table_->insertRow(row);
			auto *dot = new QTableWidgetItem(s.live ? "●" : "○");
			dot->setForeground(s.live ? ctd::magenta() : ctd::inkMuted()); /* magenta: on air */
			dot->setToolTip(s.live ? "Live" : "Nothing publishing");
			table_->setItem(row, ColLive, dot);
			table_->setItem(row, ColName, new QTableWidgetItem(qs(s.name)));
			QString publisher = protocolName(qs(s.publisher));
			if (s.has_link && s.remote && *s.remote)
				publisher += " · " + qs(s.remote);
			table_->setItem(row, ColPublisher, new QTableWidgetItem(publisher));
			table_->setItem(row, ColTracks, new QTableWidgetItem(qs(s.tracks)));
			table_->setCellWidget(row, ColBitrate, new Sparkline(h, table_));
			QString link;
			if (s.has_link) {
				link = QString("%1 ms · %2% loss")
					       .arg((int)std::lround(s.rtt_ms))
					       .arg(s.loss_percent, 0, 'f', 1);
				auto *item = new QTableWidgetItem(link);
				item->setToolTip(
					QString("Round trip %1 ms\nLost %2% over the last poll\nRetransmitted %3, "
						"dropped %4\nReceiving %5 Mbps of a %6 Mbps link")
						.arg(s.rtt_ms, 0, 'f', 1)
						.arg(s.loss_percent, 0, 'f', 2)
						.arg(s.retransmits)
						.arg(s.dropped)
						.arg(s.receive_mbps, 0, 'f', 2)
						.arg(s.link_capacity_mbps, 0, 'f', 1));
				if (s.loss_percent >= 2.0)
					item->setForeground(ctd::magenta());
				table_->setItem(row, ColLink, item);
			}
			const auto in = inObs.find(s.name);
			table_->setItem(row, ColInObs,
					new QTableWidgetItem(inObsText(in == inObs.end() ? nullptr : &in->second)));
			issueBadge(row, s.name, issues);
			if (s.name == selected)
				table_->selectRow(row);
		}

		/* H14: paths CTD sources read that the server no longer lists */
		if (snap.status == MTX_API_OK) {
			for (const auto &[path, in] : inObs) {
				if (listed.count(path))
					continue;
				const int row = table_->rowCount();
				table_->insertRow(row);
				auto *dot = new QTableWidgetItem("✕");
				dot->setForeground(ctd::tallyRed());
				dot->setToolTip("Path not on server");
				table_->setItem(row, ColLive, dot);
				table_->setItem(row, ColName, new QTableWidgetItem(QString::fromStdString(path)));
				auto *gone = new QTableWidgetItem("Path not on server");
				gone->setForeground(ctd::tallyRed());
				table_->setItem(row, ColPublisher, gone);
				table_->setItem(row, ColInObs, new QTableWidgetItem(inObsText(&in)));
				issueBadge(row, path.c_str(), issues);
			}
		}
		addAll_->setEnabled(!live_.empty());
		issue_list_free(&issues);
		mtx_poll_snapshot_free(&snap);
	}

	static QColor severityColor(enum issue_severity severity)
	{
		/* critical: tally red (the issue's title says what); warning: magenta; info: cyan */
		return severity == ISSUE_CRITICAL  ? ctd::tallyRed()
		       : severity == ISSUE_WARNING ? ctd::magenta()
						   : ctd::cyan();
	}

	/* the issues about this stream's path (or a source reading it), as "⚠ 2" in the worst one's colour */
	void issueBadge(int row, const char *path, const struct issue_list &issues)
	{
		int count = 0;
		enum issue_severity worst = ISSUE_INFO;
		QStringList titles;
		for (size_t i = 0; i < issues.count; i++) {
			const struct issue &x = issues.items[i];
			if (strcmp(x.path, path) != 0 || (*x.server_id && x.server_id != profileId_))
				continue;
			count++;
			worst = std::max(worst, x.severity);
			titles << qs(x.title);
		}
		if (!count)
			return;
		auto *item = new QTableWidgetItem(QString("⚠ %1").arg(count));
		item->setForeground(severityColor(worst));
		item->setToolTip(titles.join("\n"));
		table_->setItem(row, ColIssues, item);
	}

	/* D4: one fix, off the UI thread (a kick talks to the server); the outcome in the action line */
	void applyFix(const QString &issue, const QString &action)
	{
		action_->setText("Applying…");
		QPointer<QLabel> label = action_;
		const std::string issueId = issue.toStdString(), actionId = action.toStdString();
		std::thread([label, issueId, actionId] {
			char message[256], url[512];
			issues_apply(issueId.c_str(), actionId.c_str(), message, sizeof(message), url, sizeof(url));
			const QString text = QString::fromUtf8(message), link = QString::fromUtf8(url);
			QMetaObject::invokeMethod(
				qApp,
				[label, text, link] {
					if (!link.isEmpty())
						QApplication::clipboard()->setText(link);
					if (label)
						label->setText(link.isEmpty() ? text : text + " (copied)");
				},
				Qt::QueuedConnection);
		}).detach();
	}

	/* S15: rebuilt twice a second; the groups stay expanded */
	void showSyncGroups()
	{
		if (!isVisible())
			return;
		std::vector<struct ctd_sync_group_view> groups(16);
		const size_t count = sync_groups_snapshot(groups.data(), groups.size());
		syncLabel_->setVisible(count > 0);
		sync_->setVisible(count > 0);
		if (!count) {
			sync_->clear();
			return;
		}
		sync_->clear();
		for (size_t g = 0; g < count; g++) {
			const struct ctd_sync_group_view &v = groups[g];
			auto *top = new QTreeWidgetItem(sync_);
			top->setText(0, QString::fromUtf8(v.name));
			bool lost = false, locked = true;
			for (int m = 0; m < v.count; m++) {
				lost |= sync_state_lost(v.members[m].state);
				locked &= v.members[m].state == SYNC_LOCKED;
			}
			top->setText(1, lost ? "sync lost" : locked ? "locked" : "working");
			top->setText(2, v.mode == SYNC_FIXED_LATENCY ? QString("fixed %1 ms").arg(v.fixed_latency_ms)
								     : QString("align to slowest"));
			top->setText(4, QString("max %1 ms").arg(v.max_delay_ms));
			if (lost)
				top->setForeground(1, ctd::magenta());
			for (int m = 0; m < v.count; m++) {
				const struct ctd_sync_member_view &mv = v.members[m];
				auto *item = new QTreeWidgetItem(top);
				item->setText(0, QString::fromUtf8(mv.source) + (mv.reference ? " (reference)" : ""));
				item->setText(1, sync_state_name(mv.state));
				item->setText(2, mv.timecode[0] ? QString::fromUtf8(mv.timecode) : QString("—"));
				item->setText(3, QString("%1 ms").arg(mv.offset_ms, 0, 'f', 0));
				item->setText(4, QString("%1 ms").arg(mv.delay_ms, 0, 'f', 0));
				if (sync_state_lost(mv.state))
					item->setForeground(1, ctd::magenta());
			}
			top->setExpanded(true);
		}
	}

	/* D2: the issues panel, newest first */
	void showIssues(const struct issue_list &issues)
	{
		issues_->clear();
		for (size_t i = 0; i < issues.count; i++) {
			const struct issue &x = issues.items[i];
			const QString since = QDateTime::fromMSecsSinceEpoch(x.first_seen_ms).toString("HH:mm:ss");
			QString label = QString("%1  %2").arg(since, qs(x.title));
			if (x.action_count)
				label += QString("  ▸ %1").arg(qs(x.actions[0].label));
			auto *item = new QListWidgetItem(label, issues_);
			item->setForeground(x.applied_fix ? ctd::inkMuted() : severityColor(x.severity));
			item->setToolTip(
				qs(x.cause) +
				(x.action_count ? "\n\nRight-click for fixes; double-click applies the first." : ""));
			/* D4: what the item can do */
			QStringList actions;
			for (int a = 0; a < x.action_count; a++)
				actions << QString("%1\t%2").arg(qs(x.actions[a].id), qs(x.actions[a].label));
			item->setData(Qt::UserRole, qs(x.id));
			item->setData(Qt::UserRole + 1, actions);
		}
		if (!issues.count)
			new QListWidgetItem("No issues", issues_);
	}

	static QString inObsText(const InObs *in)
	{
		if (!in)
			return "";
		QStringList scenes;
		for (const std::string &scene : in->scenes)
			scenes << QString::fromStdString(scene);
		return scenes.isEmpty() ? "in OBS, in no scene" : scenes.join(", ");
	}

	void showStatus(const struct mtx_poll_snapshot &snap)
	{
		size_t live = 0;
		for (size_t i = 0; i < snap.count; i++)
			live += snap.items[i].live;
		switch (snap.status) {
		case MTX_API_OK:
			status_->setText(
				QString("Connected%1 · %2 ms · %3 streams, %4 live")
					.arg(*snap.version ? QString(" · MediaMTX %1").arg(qs(snap.version)) : "")
					.arg(snap.latency_ms)
					.arg(snap.count)
					.arg(live));
			break;
		case MTX_API_UNKNOWN:
			status_->setText("Looking for the server…");
			break;
		case MTX_API_UNAUTHORIZED:
			status_->setText("API login failed: check the profile's API login (Manage…)");
			break;
		case MTX_API_NOT_FOUND:
			status_->setText("No MediaMTX API: give the profile its API URL (Manage…) to list streams");
			break;
		default:
			status_->setText("API unreachable. The sources keep playing; this list comes back on its own.");
			break;
		}
	}

	std::string rowName(int row) const
	{
		const QTableWidgetItem *item = row >= 0 ? table_->item(row, ColName) : nullptr;
		return item ? item->text().toStdString() : std::string();
	}

	/* --------------------------------------------------------- actions -- */

	static obs_source_t *currentScene() { return obs_frontend_get_current_scene(); /* a reference */ }

	/* H11: a CTD Live Source for the path on this profile, named after it, fitted to the canvas (or to one cell of a
	 * cols × rows grid) */
	void addToScene(const std::string &path, obs_source_t *sceneSource, int cols, int rows, int index)
	{
		obs_scene_t *scene = obs_scene_from_source(sceneSource);
		if (!scene) {
			obs_source_release(sceneSource);
			return;
		}
		/* the source already made for this path, or a new one */
		obs_source_t *source = nullptr;
		const auto in = inObs_.find(path);
		if (in != inObs_.end())
			source = obs_get_source_by_name(in->second.source.c_str());
		if (!source) {
			std::string name = path;
			for (int n = 2;; n++) {
				obs_source_t *existing = obs_get_source_by_name(name.c_str());
				if (!existing)
					break;
				obs_source_release(existing);
				name = path + " " + std::to_string(n);
			}
			obs_data_t *settings = obs_data_create();
			obs_data_set_string(settings, "server", profileId_.c_str());
			obs_data_set_string(settings, "path", path.c_str());
			source = obs_source_create("ctd_live_source", name.c_str(), settings, nullptr);
			obs_data_release(settings);
		}
		if (source) {
			struct obs_video_info ovi = {};
			obs_get_video_info(&ovi);
			obs_sceneitem_t *item = obs_scene_add(scene, source);
			if (item && ovi.base_width && ovi.base_height) {
				const float w = cols ? (float)ovi.base_width / (float)cols : (float)ovi.base_width;
				const float h = rows ? (float)ovi.base_height / (float)rows : (float)ovi.base_height;
				const vec2 pos = {cols ? w * (float)(index % cols) : 0.0f,
						  rows ? h * (float)(index / std::max(cols, 1)) : 0.0f};
				const vec2 bounds = {w, h};
				obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
				obs_sceneitem_set_pos(item, &pos);
				obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
				obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
				obs_sceneitem_set_bounds(item, &bounds);
			}
			blog(LOG_INFO, "[ctd-live] CTD Hub: added '%s' to '%s'", obs_source_get_name(source),
			     obs_source_get_name(sceneSource));
			obs_source_release(source);
		}
		obs_source_release(sceneSource);
		lastSeq_ = 0; /* the "In OBS" column at the next tick */
	}

	/* H12 */
	void addAllLive()
	{
		const int side = grid_->currentData().toInt();
		obs_source_t *scene = currentScene();
		if (!scene)
			return;
		int index = 0;
		for (const std::string &path : live_) {
			if (side && index >= side * side)
				break; /* the grid is full */
			addToScene(path, obs_source_get_ref(scene), side, side, index++);
		}
		obs_source_release(scene);
	}

	void reconnect(const std::string &path)
	{
		const auto in = inObs_.find(path);
		if (in == inObs_.end())
			return;
		obs_source_t *source = obs_get_source_by_name(in->second.source.c_str());
		if (source) {
			calldata_t cd = {};
			proc_handler_call(obs_source_get_proc_handler(source), "reconnect", &cd);
			calldata_free(&cd);
			obs_source_release(source);
		}
	}

	void openProperties(const std::string &path)
	{
		const auto in = inObs_.find(path);
		if (in == inObs_.end())
			return;
		obs_source_t *source = obs_get_source_by_name(in->second.source.c_str());
		if (source) {
			obs_frontend_open_source_properties(source);
			obs_source_release(source);
		}
	}

	/* H11: the publish URLs MediaMTX accepts for this path, on its default ports */
	void copyPublishUrl(const std::string &path, const QString &kind)
	{
		struct ctd_server s;
		if (!servers_get(profileId_.c_str(), &s))
			return;
		const QString host = qs(s.host), name = QString::fromStdString(path);
		QString url;
		if (kind == "srt")
			url = QString("srt://%1:%2?streamid=publish:%3").arg(host).arg(s.srt_port).arg(name);
		else if (kind == "rtmp")
			url = QString("rtmp://%1:1935/%2").arg(host, name);
		else
			url = QString("http://%1:8889/%2/whip").arg(host, name);
		ctd_server_free(&s);
		QApplication::clipboard()->setText(url);
	}

	void rowMenu(const QPoint &pos)
	{
		const int row = table_->rowAt(pos.y());
		const std::string path = rowName(row);
		if (path.empty())
			return;
		const bool inObs = inObs_.count(path) > 0;
		QMenu menu(this);
		menu.addAction("Add to current scene", [this, path] { addToScene(path, currentScene(), 0, 0, 0); });
		QMenu *to = menu.addMenu("Add to scene");
		struct obs_frontend_source_list scenes = {};
		obs_frontend_get_scenes(&scenes);
		for (size_t i = 0; i < scenes.sources.num; i++) {
			obs_source_t *scene = scenes.sources.array[i];
			const QString name = qs(obs_source_get_name(scene));
			to->addAction(name, [this, path, name] {
				obs_source_t *target = obs_get_source_by_name(name.toUtf8().constData());
				if (target)
					addToScene(path, target, 0, 0, 0);
			});
		}
		obs_frontend_source_list_free(&scenes);
		QMenu *copy = menu.addMenu("Copy publish URL");
		copy->addAction("SRT", [this, path] { copyPublishUrl(path, "srt"); });
		copy->addAction("RTMP", [this, path] { copyPublishUrl(path, "rtmp"); });
		copy->addAction("WHIP (WebRTC)", [this, path] { copyPublishUrl(path, "whip"); });
		/* H8: only when the profile allows changing the server */
		struct ctd_server s;
		const bool writes = servers_get(profileId_.c_str(), &s) && s.allow_writes;
		ctd_server_free(&s);
		if (writes) {
			menu.addSeparator();
			const auto pub = publishers_.find(path);
			menu.addAction("Kick publisher…", [this, path] {
				    kickPublisher(path);
			    })->setEnabled(pub != publishers_.end() && !pub->second.second.empty());
			menu.addAction("Remove path from the server…", [this, path] { removePath(path); });
		}
		menu.addSeparator();
		menu.addAction("Reconnect", [this, path] { reconnect(path); })->setEnabled(inObs);
		menu.addAction("Open properties", [this, path] { openProperties(path); })->setEnabled(inObs);
		menu.exec(table_->viewport()->mapToGlobal(pos));
	}

	QComboBox *profile_;
	QLabel *status_;
	QPushButton *addAll_;
	QComboBox *grid_;
	QTableWidget *table_;
	std::string profileId_;
	struct mtx_poller *poller_ = nullptr;
	uint64_t lastSeq_ = 0;
	uint64_t lastIssueSeq_ = 0;
	QListWidget *issues_;
	QLabel *syncLabel_;
	QTreeWidget *sync_;
	std::map<std::string, std::deque<double>> history_;
	std::map<std::string, InObs> inObs_;
	std::vector<std::string> live_;
	std::map<std::string, std::pair<std::string, std::string>> publishers_; /* path → publisher type, id */
	QCheckBox *autoAdd_;
	QComboBox *autoScene_;
	QPushButton *createPath_;
	QLabel *action_;
	uint64_t autoSeq_ = 0;
	std::set<std::string> seenLive_;
	bool seenInit_ = false;
};

} // namespace

extern "C" void ctd_hub_dock_register(void)
{
	auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
	obs_frontend_add_dock_by_id("ctd-hub", "CTD Hub", new HubDock(main));
}
