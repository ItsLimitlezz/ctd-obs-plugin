/*
 * The CTD Overlays dock: everything about CTD Overlay from inside OBS, through CTD Relay (the hub; this dock never
 * talks to CTD Overlay itself). Three pages:
 *
 * - Overlays: every overlay the connected CTD Overlay server registered for this OBS's CTD Relay profile, with its
 *   template and version, its scenes, whether the server is online and whether this scene collection already shows it.
 *   Search, grouped by show. For the whole overlay or one of its scenes: Add to scene (the same browser source the CTD
 *   Relay dock adds: overlay-sources.cpp), Copy URL, Open in browser, and the overlay's page in CTD Overlay.
 * - Updates: CTD Overlay's version, each template's installed version against the newest release of its followed
 *   GitHub source, which updates go by themselves (patch, minor) and which wait (major), the overlays waiting, with a
 *   button to their upgrade screen in CTD Overlay, and the first lines of each release's notes. OBS never upgrades.
 * - Make a template: the template kit in plain words, with the AI prompt, the guide and the starter.
 *
 * Data: the CTD Relay client's snapshot (multistream.c): GET /profiles/{id}/overlays and /overlay-summary
 * (CTD Relay docs/API.md §9.5). The pure logic (versions, links, URL matching) is in overlays-core.c, tested in
 * test/overlays-dock.
 */

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QAbstractItemView>
#include <QApplication>
#include <QBoxLayout>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSet>
#include <QStackedWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "../http-client.h" /* a C header without its own extern "C" */
}
#include "../multistream.h"
#include "../overlay-scenes.h"
#include "../overlays-core.h"
#include "brand.hpp"
#include "overlay-sources.hpp"

namespace {

namespace ov = ctd::overlays;

constexpr const char *kOverlaysDockId = "ctd-overlays";
constexpr const char *kRelayDockId = "ctd-multistream";
constexpr int kRefreshMs = 500;
constexpr int kCollectionScanMs = 3000;

/* The template kit, read from CTD Overlay's public repository when asked for (always the current version) */
constexpr const char *kKitRaw = "https://raw.githubusercontent.com/ItsLimitlezz/ctd-overlay/main/docs/template-kit/";
constexpr const char *kKitBrowse = "https://github.com/ItsLimitlezz/ctd-overlay/tree/main/docs/template-kit";
constexpr const char *kGuideUrl =
	"https://github.com/ItsLimitlezz/ctd-overlay/blob/main/docs/template-kit/TEMPLATE-GUIDE.md";
constexpr const char *kReleasesDocUrl =
	"https://github.com/ItsLimitlezz/ctd-overlay/blob/main/docs/template-releases.md";
/* what "Download starter" saves: the starter template and the documents to give an AI chat with it */
const char *const kKitFiles[] = {
	"README.md",
	"NEW-OVERLAY.md",
	"PROMPT.md",
	"TEMPLATE-GUIDE.md",
	"starter/template.json",
	"starter/index.html",
	"starter/template.css",
	"starter/template.js",
	"starter/previews/preview.svg",
};

QString uiText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString str(obs_data_t *data, const char *key)
{
	return QString::fromUtf8(data ? obs_data_get_string(data, key) : "");
}

/* a C string the caller frees (free()) as a QString */
QString taken(char *s)
{
	QString out = s ? QString::fromUtf8(s) : QString();
	free(s);
	return out;
}

/* Shows one of OBS's docks by its id and brings it to the front (OBS names a dock's QDockWidget after its id). */
void showDock(const char *id)
{
	auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *dock = main ? main->findChild<QDockWidget *>(QString::fromLatin1(id)) : nullptr;
	if (!dock)
		return;
	dock->setVisible(true);
	dock->raise();
	dock->activateWindow();
}

struct DataRef {
	obs_data_t *data = nullptr;
	DataRef() = default;
	explicit DataRef(obs_data_t *d) : data(d) {}
	DataRef(const DataRef &other) : data(other.data) { obs_data_addref(data); }
	DataRef &operator=(const DataRef &other)
	{
		if (this != &other) {
			obs_data_addref(other.data);
			obs_data_release(data);
			data = other.data;
		}
		return *this;
	}
	~DataRef() { obs_data_release(data); }
	obs_data_t *get() const { return data; }
};

/* One overlay resource (one canvas of one overlay) with what the summary says about it */
struct Row {
	DataRef resource; /* GET /profiles/{id}/overlays item: id, name, url, width, height, appId, meta */
	QString key;      /* the resource id */
	QString name;     /* "lower — landscape" */
	QString canvas;   /* landscape | vertical */
	QString appName;
	bool online = false;
	QString slug; /* for its page in CTD Overlay */
	QString ui;   /* CTD Overlay's admin UI address */
	QString show; /* the show it belongs to, or empty */
	QString templateName;
	QString templateVersion;
	QString waitingTo; /* a newer version waits for the operator */
	bool waitingMajor = false;
	std::vector<ov::Scene> scenes;
};

/* What the scene collection shows of one overlay resource */
struct InCollection {
	int whole = 0;        /* sources showing the whole overlay */
	QSet<QString> scenes; /* scenes some source pins */
	bool any() const { return whole > 0 || !scenes.isEmpty(); }
};

enum ItemKind { KindGroup = 1, KindOverlay = 2, KindScene = 3 };
constexpr int kRowRole = Qt::UserRole;       /* index into rows_ */
constexpr int kSceneRole = Qt::UserRole + 1; /* scene id, empty for the whole overlay */
constexpr int kKindRole = Qt::UserRole + 2;

/* -------------------------------------------------------------------- the page switcher --- */

/* Three page buttons in a row, or one under the other when the dock is too narrow for the row. */
class PageBar : public QWidget {
public:
	explicit PageBar(QWidget *parent) : QWidget(parent)
	{
		layout_ = new QBoxLayout(QBoxLayout::LeftToRight, this);
		layout_->setContentsMargins(0, 0, 0, 0);
		layout_->setSpacing(6);
		group_ = new QButtonGroup(this);
		group_->setExclusive(true);
	}

	QPushButton *add(const QString &text)
	{
		auto *b = ctd::button(new QPushButton(text, this));
		b->setCheckable(true);
		b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		group_->addButton(b, static_cast<int>(group_->buttons().size()));
		layout_->addWidget(b, 1);
		QObject::connect(b, &QPushButton::toggled, this, [this] { restyle(); });
		return b;
	}

	QButtonGroup *group() const { return group_; }

	void select(int id)
	{
		if (QAbstractButton *b = group_->button(id))
			b->setChecked(true);
		restyle();
	}

protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QWidget::resizeEvent(event);
		int row = 0;
		for (QAbstractButton *b : group_->buttons())
			row += b->sizeHint().width() + layout_->spacing();
		const auto direction = row > event->size().width() ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
		if (layout_->direction() != direction)
			layout_->setDirection(direction);
	}

private:
	/* the page shown is a primary (cyan) button, the others secondary */
	void restyle()
	{
		for (QAbstractButton *b : group_->buttons()) {
			const char *role = b->isChecked() ? "primary" : "secondary";
			if (b->property("ctdRole").toString() != QLatin1String(role)) {
				b->setProperty("ctdRole", role);
				ctd::repolish(b);
			}
		}
	}

	QBoxLayout *layout_;
	QButtonGroup *group_;
};

/* A card: CTD Night's surface with a hairline border (the theme's #ctdDeviceCard rule) */
QFrame *card(QWidget *parent)
{
	auto *frame = new QFrame(parent);
	frame->setObjectName("ctdDeviceCard");
	frame->setAttribute(Qt::WA_StyledBackground, true);
	return frame;
}

QLabel *wrapped(const QString &text, QWidget *parent, const char *role = nullptr)
{
	auto *label = new QLabel(text, parent);
	label->setWordWrap(true);
	label->setTextInteractionFlags(Qt::TextSelectableByMouse);
	if (role)
		label->setProperty("ctdRole", role);
	return label;
}

void clearLayout(QLayout *layout)
{
	while (QLayoutItem *item = layout->takeAt(0)) {
		if (QWidget *w = item->widget())
			w->deleteLater();
		if (QLayout *child = item->layout()) {
			clearLayout(child);
			child->deleteLater();
		}
		delete item;
	}
}

/* ------------------------------------------------------------------------------ the dock --- */

class OverlaysDock : public QWidget {
public:
	explicit OverlaysDock(QWidget *parent) : QWidget(parent)
	{
		ctd::applyTheme(this);
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(10, 10, 10, 10);
		layout->setSpacing(8);

		auto *head = new QHBoxLayout();
		head->setSpacing(8);
		head->addWidget(new ctd::Stripe(this), 0, Qt::AlignVCenter);
		head->addWidget(ctd::eyebrow(uiText("Overlays.Title"), this), 1, Qt::AlignVCenter);
		layout->addLayout(head);
		status_ = wrapped(QString(), this, "muted");
		layout->addWidget(status_);

		pages_ = new PageBar(this);
		pages_->add(uiText("Overlays.Page.Overlays"));
		updatesButton_ = pages_->add(uiText("Overlays.Page.Updates"));
		pages_->add(uiText("Overlays.Page.Template"));
		layout->addWidget(pages_);

		stack_ = new QStackedWidget(this);
		stack_->addWidget(buildOverlaysPage());
		stack_->addWidget(buildUpdatesPage());
		stack_->addWidget(buildTemplatePage());
		layout->addWidget(stack_, 1);
		QObject::connect(pages_->group(), &QButtonGroup::idClicked, stack_, &QStackedWidget::setCurrentIndex);
		pages_->select(0);

		timer_ = new QTimer(this);
		QObject::connect(timer_, &QTimer::timeout, this, [this] { refresh(false); });
		timer_->start(kRefreshMs);
		scan_ = new QTimer(this);
		QObject::connect(scan_, &QTimer::timeout, this, [this] {
			if (isVisible() && scanCollection())
				rebuildTree();
		});
		scan_->start(kCollectionScanMs);
		obs_frontend_add_event_callback(frontendEvent, this);
	}

	~OverlaysDock() override
	{
		obs_frontend_remove_event_callback(frontendEvent, this);
		*alive_ = false; /* downloads still running report to nobody */
	}

	void showDockWindow() { showDock(kOverlaysDockId); }

protected:
	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		refresh(true);
	}

private:
	/* ----------------------------------------------------------------------- pages --- */

	QWidget *buildOverlaysPage()
	{
		auto *page = new QWidget(this);
		auto *layout = new QVBoxLayout(page);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(8);

		/* no CTD Overlay through CTD Relay: how to get there */
		empty_ = card(page);
		auto *emptyLayout = new QVBoxLayout(empty_);
		emptyLayout->setContentsMargins(12, 12, 12, 12);
		emptyLayout->setSpacing(8);
		emptyTitle_ = wrapped(QString(), empty_, "title");
		emptyTitle_->setFont(ctd::font(ctd::Face::Heading, 15, QFont::Bold));
		emptyText_ = wrapped(QString(), empty_);
		emptyText_->setTextFormat(Qt::RichText);
		emptyText_->setTextInteractionFlags(Qt::TextBrowserInteraction);
		emptyText_->setOpenExternalLinks(true);
		emptyButton_ =
			ctd::button(new QPushButton(uiText("Overlays.Empty.OpenRelay"), empty_), ctd::Role::Primary);
		QObject::connect(emptyButton_, &QPushButton::clicked, this, [] { showDock(kRelayDockId); });
		emptyLayout->addWidget(emptyTitle_);
		emptyLayout->addWidget(emptyText_);
		emptyLayout->addWidget(emptyButton_, 0, Qt::AlignLeft);
		empty_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
		layout->addWidget(empty_, 0, Qt::AlignTop);

		list_ = new QWidget(page);
		auto *listLayout = new QVBoxLayout(list_);
		listLayout->setContentsMargins(0, 0, 0, 0);
		listLayout->setSpacing(8);
		search_ = new QLineEdit(list_);
		search_->setPlaceholderText(uiText("Overlays.Search"));
		search_->setClearButtonEnabled(true);
		listLayout->addWidget(search_);
		groupByShow_ = new QCheckBox(list_);
		groupByShow_->setChecked(true);
		listLayout->addLayout(ctd::wrappingCheckBox(groupByShow_, uiText("Overlays.GroupByShow"), list_));

		tree_ = new QTreeWidget(list_);
		tree_->setHeaderHidden(true);
		tree_->setColumnCount(1);
		tree_->setWordWrap(true);
		tree_->setUniformRowHeights(false);
		tree_->setRootIsDecorated(true);
		tree_->setIndentation(14);
		tree_->setTextElideMode(Qt::ElideNone);
		tree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		tree_->header()->setStretchLastSection(true);
		tree_->setContextMenuPolicy(Qt::CustomContextMenu);
		tree_->setMinimumHeight(140);
		tree_->setSelectionMode(QAbstractItemView::SingleSelection);
		listLayout->addWidget(tree_, 1);

		/* the actions on the selected overlay or scene: two by two, so they fit a narrow dock */
		auto *actions = new QGridLayout();
		actions->setSpacing(6);
		add_ = ctd::button(new QPushButton(uiText("Overlays.Add"), list_), ctd::Role::Primary);
		copy_ = ctd::button(new QPushButton(uiText("Overlays.CopyUrl"), list_));
		open_ = ctd::button(new QPushButton(uiText("Overlays.OpenBrowser"), list_));
		panel_ = ctd::button(new QPushButton(uiText("Overlays.ControlPanel"), list_));
		add_->setToolTip(uiText("Overlays.Add.Tip"));
		copy_->setToolTip(uiText("Overlays.CopyUrl.Tip"));
		panel_->setToolTip(uiText("Overlays.ControlPanel.Tip"));
		actions->addWidget(add_, 0, 0);
		actions->addWidget(copy_, 0, 1);
		actions->addWidget(open_, 1, 0);
		actions->addWidget(panel_, 1, 1);
		listLayout->addLayout(actions);
		action_ = wrapped(QString(), list_, "muted");
		listLayout->addWidget(action_);
		layout->addWidget(list_, 1);

		QObject::connect(search_, &QLineEdit::textChanged, this, [this] { rebuildTree(); });
		QObject::connect(groupByShow_, &QCheckBox::toggled, this, [this] { rebuildTree(); });
		QObject::connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] { updateActions(); });
		QObject::connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &at) {
			QTreeWidgetItem *item = tree_->itemAt(at);
			if (!item || item->data(0, kKindRole).toInt() == KindGroup)
				return;
			tree_->setCurrentItem(item);
			QMenu menu(this);
			for (QPushButton *b : {add_, copy_, open_, panel_}) {
				QAction *a = menu.addAction(b->text().left(1) + b->text().mid(1).toLower());
				a->setEnabled(b->isEnabled());
				QObject::connect(a, &QAction::triggered, b, &QPushButton::click);
			}
			menu.exec(tree_->viewport()->mapToGlobal(at));
		});
		QObject::connect(add_, &QPushButton::clicked, this, [this] { addSelected(); });
		QObject::connect(copy_, &QPushButton::clicked, this, [this] { copySelected(); });
		QObject::connect(open_, &QPushButton::clicked, this, [this] { openSelected(); });
		QObject::connect(panel_, &QPushButton::clicked, this, [this] { openControlPanel(); });
		updateActions();
		return page;
	}

	QScrollArea *scrollPage(QWidget **body, QVBoxLayout **layout)
	{
		auto *scroll = new QScrollArea(this);
		scroll->setObjectName("ctdScroll");
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		scroll->viewport()->setObjectName("ctdScrollViewport");
		*body = new QWidget(scroll);
		(*body)->setObjectName("ctdBody");
		*layout = new QVBoxLayout(*body);
		(*layout)->setContentsMargins(0, 0, 6, 0);
		(*layout)->setSpacing(10);
		scroll->setWidget(*body);
		return scroll;
	}

	QWidget *buildUpdatesPage()
	{
		QWidget *body;
		QVBoxLayout *layout;
		QScrollArea *scroll = scrollPage(&body, &layout);
		updates_ = new QVBoxLayout();
		updates_->setSpacing(10);
		layout->addLayout(updates_);
		layout->addStretch(1);
		return scroll;
	}

	/* "Make a template": the kit in plain words (docs/template-kit in CTD Overlay) */
	QWidget *buildTemplatePage()
	{
		QWidget *body;
		QVBoxLayout *layout;
		QScrollArea *scroll = scrollPage(&body, &layout);

		layout->addWidget(wrapped(uiText("Overlays.Kit.Intro"), body));

		auto *buttons = new QGridLayout();
		buttons->setSpacing(6);
		auto *prompt =
			ctd::button(new QPushButton(uiText("Overlays.Kit.CopyPrompt"), body), ctd::Role::Primary);
		auto *guide = ctd::button(new QPushButton(uiText("Overlays.Kit.OpenGuide"), body));
		auto *starter = ctd::button(new QPushButton(uiText("Overlays.Kit.DownloadStarter"), body));
		prompt->setToolTip(uiText("Overlays.Kit.CopyPrompt.Tip"));
		starter->setToolTip(uiText("Overlays.Kit.DownloadStarter.Tip"));
		/* one under the other: side by side they don't fit a narrow dock */
		buttons->addWidget(prompt, 0, 0);
		buttons->addWidget(guide, 1, 0);
		buttons->addWidget(starter, 2, 0);
		layout->addLayout(buttons);
		kitStatus_ = wrapped(QString(), body, "muted");
		kitStatus_->setVisible(false); /* shown with the first message */
		layout->addWidget(kitStatus_);

		struct Step {
			const char *title;
			const char *text;
		};
		const Step steps[] = {
			{"Overlays.Kit.Step1.Title", "Overlays.Kit.Step1"},
			{"Overlays.Kit.Step2.Title", "Overlays.Kit.Step2"},
			{"Overlays.Kit.Step3.Title", "Overlays.Kit.Step3"},
			{"Overlays.Kit.Step4.Title", "Overlays.Kit.Step4"},
			{"Overlays.Kit.Step5.Title", "Overlays.Kit.Step5"},
			{"Overlays.Kit.Step6.Title", "Overlays.Kit.Step6"},
		};
		int n = 1;
		for (const Step &step : steps) {
			QFrame *c = card(body);
			auto *cl = new QVBoxLayout(c);
			cl->setContentsMargins(12, 10, 12, 10);
			cl->setSpacing(4);
			auto *title =
				wrapped(QString("%1 // %2").arg(n++, 2, 10, QLatin1Char('0')).arg(uiText(step.title)),
					c, "eyebrow");
			title->setFont(ctd::font(ctd::Face::Mono, 10, QFont::Bold, true, 1.5));
			auto *text = wrapped(uiText(step.text), c);
			text->setTextFormat(Qt::RichText);
			text->setTextInteractionFlags(Qt::TextBrowserInteraction);
			text->setOpenExternalLinks(true);
			cl->addWidget(title);
			cl->addWidget(text);
			layout->addWidget(c);
		}
		auto *more =
			wrapped(uiText("Overlays.Kit.More")
					.arg(QString::fromLatin1(kKitBrowse), QString::fromLatin1(kReleasesDocUrl)),
				body, "muted");
		more->setTextFormat(Qt::RichText);
		more->setTextInteractionFlags(Qt::TextBrowserInteraction);
		more->setOpenExternalLinks(true);
		layout->addWidget(more);
		layout->addStretch(1);

		QObject::connect(prompt, &QPushButton::clicked, this, [this] { copyPrompt(); });
		QObject::connect(guide, &QPushButton::clicked, this,
				 [] { QDesktopServices::openUrl(QUrl(QString::fromLatin1(kGuideUrl))); });
		QObject::connect(starter, &QPushButton::clicked, this, [this] { downloadStarter(); });
		return scroll;
	}

	/* ------------------------------------------------------------------- refresh --- */

	static void frontendEvent(enum obs_frontend_event event, void *data)
	{
		auto *dock = static_cast<OverlaysDock *>(data);
		switch (event) {
		case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		case OBS_FRONTEND_EVENT_SCENE_CHANGED:
			if (dock->isVisible() && dock->scanCollection())
				dock->rebuildTree();
			break;
		default:
			break;
		}
	}

	void refresh(bool force)
	{
		if (!isVisible() && !force)
			return;
		ms_snapshot s = {};
		ms_snapshot_get(&s);
		if (force || s.seq != lastSeq_) {
			lastSeq_ = s.seq;
			const bool connected = s.link == MS_CONNECTED;
			const bool listChanged = lastOverlays_.changed(s.overlays) |
						 lastSummaries_.changed(s.overlay_summaries);
			if (force || listChanged || connected != connected_ ||
			    s.overlay_summary_state != summaryState_ || QString::fromUtf8(s.profile_id) != profileId_) {
				connected_ = connected;
				summaryState_ = s.overlay_summary_state;
				profileId_ = QString::fromUtf8(s.profile_id);
				buildRows(s);
				scanCollection();
				showState(s);
				rebuildTree();
				rebuildUpdates(s);
			}
		}
		ms_snapshot_free(&s);
	}

	/* the summary overlay entry for (app, overlay id), and each app's summary */
	void buildRows(const ms_snapshot &s)
	{
		rows_.clear();
		std::map<std::string, DataRef> summaryByApp;
		std::map<std::string, DataRef> entryByOverlay; /* "<appId>\n<overlay id>" */
		const size_t apps = s.overlay_summaries ? obs_data_array_count(s.overlay_summaries) : 0;
		for (size_t i = 0; i < apps; i++) {
			DataRef app(obs_data_array_item(s.overlay_summaries, i));
			const std::string appId = obs_data_get_string(app.get(), "appId");
			obs_data_array_t *overlays = obs_data_get_array(app.get(), "overlays");
			for (size_t j = 0; overlays && j < obs_data_array_count(overlays); j++) {
				DataRef o(obs_data_array_item(overlays, j));
				entryByOverlay[appId + "\n" + obs_data_get_string(o.get(), "id")] = o;
			}
			obs_data_array_release(overlays);
			summaryByApp[appId] = app;
		}

		const size_t count = s.overlays ? obs_data_array_count(s.overlays) : 0;
		for (size_t i = 0; i < count; i++) {
			Row row;
			row.resource = DataRef(obs_data_array_item(s.overlays, i));
			obs_data_t *r = row.resource.get();
			row.key = str(r, "id");
			row.name = str(r, "name");
			row.canvas = ov::canvas(r);
			row.appName = str(r, "appName");
			row.online = obs_data_get_bool(r, "appOnline");
			row.scenes = ov::scenes(r);
			const std::string appId = obs_data_get_string(r, "appId");
			const std::string overlayId = row.key.section(':', 0, 0).toStdString();
			auto app = summaryByApp.find(appId);
			if (app != summaryByApp.end())
				row.ui = str(app->second.get(), "ui");
			auto entry = entryByOverlay.find(appId + "\n" + overlayId);
			if (entry != entryByOverlay.end()) {
				obs_data_t *e = entry->second.get();
				row.slug = str(e, "slug");
				row.show = str(e, "show");
				obs_data_t *t = obs_data_get_obj(e, "template");
				row.templateName = str(t, "name");
				if (row.templateName.isEmpty())
					row.templateName = str(t, "id");
				row.templateVersion = str(t, "version");
				obs_data_release(t);
				obs_data_t *w = obs_data_get_obj(e, "waiting");
				if (w) {
					row.waitingTo = str(w, "to");
					row.waitingMajor = obs_data_get_bool(w, "major");
				}
				obs_data_release(w);
			}
			/* an older CTD Overlay or CTD Relay: the overlay's own URL says where CTD Overlay is, and which */
			const QByteArray url = str(r, "url").toUtf8();
			if (row.ui.isEmpty())
				row.ui = taken(ctd_url_origin(url.constData()));
			if (row.slug.isEmpty())
				row.slug = taken(ctd_overlay_slug_of_url(url.constData()));
			rows_.push_back(std::move(row));
		}
	}

	/* What this scene collection already shows: every browser source that is one of ours (marked by the CTD Relay
	 * dock or this one) or shows one of the overlays' URLs. True when it changed. */
	bool scanCollection()
	{
		struct Scan {
			const std::vector<Row> *rows;
			std::map<QString, InCollection> found;
		} scan{&rows_, {}};
		obs_enum_sources(
			[](void *param, obs_source_t *source) {
				auto *sc = static_cast<Scan *>(param);
				if (strcmp(obs_source_get_unversioned_id(source), "browser_source") != 0)
					return true;
				obs_data_t *settings = obs_source_get_settings(source);
				const std::string url = obs_data_get_string(settings, "url");
				obs_data_release(settings);
				obs_data_t *priv = obs_source_get_private_settings(source);
				const QString resource = str(priv, ov::kResource);
				QString scene = str(priv, ov::kScene);
				obs_data_release(priv);
				if (scene.isEmpty())
					scene = taken(ctd_overlay_scene_of_url(url.c_str()));
				for (const Row &row : *sc->rows) {
					const bool same =
						(!resource.isEmpty() && resource == row.key) ||
						ctd_overlay_url_same(url.c_str(),
								     obs_data_get_string(row.resource.get(), "url"));
					if (!same)
						continue;
					InCollection &in = sc->found[row.key];
					if (scene.isEmpty())
						in.whole++;
					else
						in.scenes.insert(scene);
					break;
				}
				return true;
			},
			&scan);
		bool changed = scan.found.size() != collection_.size();
		for (auto it = scan.found.begin(); !changed && it != scan.found.end(); ++it) {
			auto was = collection_.find(it->first);
			changed = was == collection_.end() || was->second.whole != it->second.whole ||
				  was->second.scenes != it->second.scenes;
		}
		collection_ = std::move(scan.found);
		return changed;
	}

	void showState(const ms_snapshot &s)
	{
		const bool any = !rows_.empty();
		empty_->setVisible(!any);
		list_->setVisible(any);
		if (!connected_) {
			status_->setText(uiText("Overlays.Status.NotConnected"));
			emptyTitle_->setText(uiText("Overlays.Empty.NoRelay.Title"));
			emptyText_->setText(uiText("Overlays.Empty.NoRelay"));
			emptyButton_->setVisible(true);
			return;
		}
		if (!any) {
			status_->setText(uiText("Overlays.Status.NoOverlays"));
			emptyTitle_->setText(uiText("Overlays.Empty.NoServer.Title"));
			emptyText_->setText(uiText("Overlays.Empty.NoServer"));
			emptyButton_->setVisible(true);
			return;
		}
		QSet<QString> servers;
		int online = 0;
		for (const Row &row : rows_) {
			if (!servers.contains(row.appName) && row.online)
				online++;
			servers.insert(row.appName);
		}
		QString text = uiText("Overlays.Status.Count").arg(rows_.size()).arg(servers.size());
		if (online < servers.size())
			text += " " + uiText("Overlays.Status.Offline");
		if (s.overlay_summary_state == MS_SUMMARY_UNSUPPORTED)
			text += " " + uiText("Overlays.Status.OldRelay");
		status_->setText(text);
	}

	/* --------------------------------------------------------------- the list --- */

	QString overlayLine(const Row &row, const InCollection *in) const
	{
		QString line = row.name;
		QStringList details;
		if (!row.templateName.isEmpty())
			details << QString("%1 %2").arg(row.templateName, row.templateVersion).trimmed();
		details << uiText(row.canvas == "vertical" ? "Overlays.Canvas.Vertical" : "Overlays.Canvas.Landscape");
		if (!row.scenes.empty())
			details << uiText("Relay.Overlay.ScenesCount").arg(row.scenes.size());
		QStringList flags;
		flags << uiText(row.online ? "Overlays.Online" : "Overlays.Offline");
		if (in && in->any())
			flags << uiText("Overlays.InCollection");
		if (!row.waitingTo.isEmpty())
			flags << uiText(row.waitingMajor ? "Overlays.UpdateWaitsMajor" : "Overlays.UpdateWaits")
					 .arg(row.waitingTo);
		return line + "\n" + details.join(" · ") + "\n" + flags.join(" · ");
	}

	bool matches(const Row &row, const QString &needle) const
	{
		if (needle.isEmpty())
			return true;
		const QByteArray n = needle.toUtf8();
		for (const QString &field : {row.name, row.show, row.templateName, row.appName, row.slug, row.canvas})
			if (ctd_text_contains(field.toUtf8().constData(), n.constData()))
				return true;
		for (const ov::Scene &scene : row.scenes)
			if (ctd_text_contains(scene.name.toUtf8().constData(), n.constData()))
				return true;
		return false;
	}

	void rebuildTree()
	{
		/* keep the selection across a rebuild */
		QString selectedKey, selectedScene;
		int selectedKind = 0;
		if (QTreeWidgetItem *cur = tree_->currentItem()) {
			selectedKind = cur->data(0, kKindRole).toInt();
			const int idx = cur->data(0, kRowRole).toInt();
			if (selectedKind != KindGroup && idx >= 0 && idx < (int)shownKeys_.size())
				selectedKey = shownKeys_[(size_t)idx];
			selectedScene = cur->data(0, kSceneRole).toString();
		}
		shownKeys_.clear();
		for (const Row &row : rows_)
			shownKeys_.push_back(row.key);

		tree_->setUpdatesEnabled(false);
		tree_->clear();
		const QString needle = search_->text().trimmed();
		const bool grouped = groupByShow_->isChecked();
		QHash<QString, QTreeWidgetItem *> groups;
		QTreeWidgetItem *restore = nullptr;
		int shown = 0;
		for (size_t i = 0; i < rows_.size(); i++) {
			const Row &row = rows_[i];
			if (!matches(row, needle))
				continue;
			shown++;
			auto in = collection_.find(row.key);
			const InCollection *inc = in == collection_.end() ? nullptr : &in->second;
			QTreeWidgetItem *parent = nullptr;
			if (grouped) {
				const QString show = row.show.isEmpty() ? uiText("Overlays.NoShow") : row.show;
				parent = groups.value(show);
				if (!parent) {
					parent = new QTreeWidgetItem(tree_, QStringList(show.toUpper()));
					parent->setData(0, kKindRole, KindGroup);
					parent->setFlags(Qt::ItemIsEnabled);
					parent->setForeground(0, ctd::cyan());
					parent->setFont(0, ctd::font(ctd::Face::Mono, 10, QFont::Bold, true, 1.5));
					groups.insert(show, parent);
				}
			}
			auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
			item->setText(0, overlayLine(row, inc));
			item->setData(0, kRowRole, (int)i);
			item->setData(0, kKindRole, KindOverlay);
			item->setToolTip(0, uiText("Overlays.Item.Tip"));
			if (!row.online)
				item->setForeground(0, ctd::inkMuted());
			if (selectedKind == KindOverlay && row.key == selectedKey)
				restore = item;
			for (const ov::Scene &scene : row.scenes) {
				auto *child = new QTreeWidgetItem(item);
				const bool added = inc && inc->scenes.contains(scene.id);
				child->setText(0, scene.name + (added ? "  ·  " + uiText("Overlays.InCollection")
								      : QString()));
				child->setData(0, kRowRole, (int)i);
				child->setData(0, kSceneRole, scene.id);
				child->setData(0, kKindRole, KindScene);
				child->setToolTip(0, uiText("Overlays.Scene.Tip"));
				if (selectedKind == KindScene && row.key == selectedKey && scene.id == selectedScene)
					restore = child;
			}
		}
		tree_->expandAll();
		/* a scene list is folded unless the search found it by a scene's name */
		if (needle.isEmpty())
			for (QTreeWidgetItemIterator it(tree_); *it; ++it)
				if ((*it)->data(0, kKindRole).toInt() == KindOverlay)
					(*it)->setExpanded(false);
		if (restore) {
			tree_->setCurrentItem(restore);
			if (restore->parent() && restore->data(0, kKindRole).toInt() == KindScene)
				restore->parent()->setExpanded(true);
		}
		if (!shown && !rows_.empty()) {
			auto *none = new QTreeWidgetItem(tree_, QStringList(uiText("Overlays.NoMatch")));
			none->setFlags(Qt::ItemIsEnabled);
			none->setForeground(0, ctd::inkMuted());
			none->setData(0, kKindRole, KindGroup);
		}
		tree_->setUpdatesEnabled(true);
		updateActions();
	}

	/* the selected row and scene (empty: the whole overlay); nullptr when nothing usable is selected */
	const Row *selected(QString *scene) const
	{
		QTreeWidgetItem *item = tree_->currentItem();
		if (!item || !item->isSelected())
			return nullptr;
		const int kind = item->data(0, kKindRole).toInt();
		const int idx = item->data(0, kRowRole).toInt();
		if ((kind != KindOverlay && kind != KindScene) || idx < 0 || idx >= (int)rows_.size())
			return nullptr;
		if (scene)
			*scene = kind == KindScene ? item->data(0, kSceneRole).toString() : QString();
		return &rows_[(size_t)idx];
	}

	void updateActions()
	{
		QString scene;
		const Row *row = selected(&scene);
		const bool hasUrl = row && !str(row->resource.get(), "url").isEmpty();
		add_->setEnabled(hasUrl);
		copy_->setEnabled(hasUrl);
		open_->setEnabled(hasUrl);
		panel_->setEnabled(row && !adminUrl(*row, false).isEmpty());
		const QString what = !row              ? QString()
				     : scene.isEmpty() ? uiText("Overlays.WholeOverlay")
						       : ov::sceneName(row->resource.get(), scene);
		add_->setText(row && !scene.isEmpty() ? uiText("Overlays.AddScene") : uiText("Overlays.Add"));
		if (!row)
			add_->setToolTip(uiText("Overlays.PickFirst"));
		else
			add_->setToolTip(uiText("Overlays.Add.Tip") + "\n" + what);
	}

	QString adminUrl(const Row &row, bool upgrade) const
	{
		const QByteArray ui = row.ui.toUtf8(), slug = row.slug.toUtf8();
		return taken(ctd_overlay_admin_url(ui.constData(), slug.constData(), upgrade));
	}

	void addSelected()
	{
		QString scene;
		const Row *row = selected(&scene);
		if (!row)
			return;
		const ov::AddResult added = ov::addToCurrentScene(row->resource.get(), scene);
		if (added.noVertical)
			action_->setText(uiText("Relay.Overlay.NoVerticalCanvas"));
		else if (!added.name.empty())
			action_->setText(
				uiText("Relay.Overlay.Added").arg(QString::fromStdString(added.name), added.where));
		else
			action_->setText(uiText("Overlays.AddFailed"));
		if (scanCollection())
			rebuildTree();
	}

	void copySelected()
	{
		QString scene;
		const Row *row = selected(&scene);
		if (!row)
			return;
		/* the URL holds the overlay's key: it goes to the clipboard only, never to the log */
		QApplication::clipboard()->setText(QString::fromStdString(ov::sceneUrl(row->resource.get(), scene)));
		action_->setText(uiText("Overlays.Copied")
					 .arg(obs_data_get_int(row->resource.get(), "width"))
					 .arg(obs_data_get_int(row->resource.get(), "height")));
	}

	void openSelected()
	{
		QString scene;
		const Row *row = selected(&scene);
		if (!row)
			return;
		QDesktopServices::openUrl(QUrl(QString::fromStdString(ov::sceneUrl(row->resource.get(), scene))));
	}

	void openControlPanel()
	{
		const Row *row = selected(nullptr);
		if (!row)
			return;
		const QString url = adminUrl(*row, false);
		if (!url.isEmpty())
			QDesktopServices::openUrl(QUrl(url));
	}

	/* ------------------------------------------------------------- updates page --- */

	QLabel *badge(const QString &text, const QColor &color, QWidget *parent)
	{
		auto *label = new QLabel(text, parent);
		label->setFont(ctd::font(ctd::Face::Mono, 9, QFont::Bold, true, 1.0));
		label->setStyleSheet(QString("color: %1; border: 1px solid %1; padding: 1px 6px;").arg(color.name()));
		label->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
		return label;
	}

	void message(const QString &text, const char *role = "muted")
	{
		QFrame *c = card(this);
		auto *l = new QVBoxLayout(c);
		l->setContentsMargins(12, 10, 12, 10);
		auto *label = wrapped(text, c, role);
		label->setTextFormat(Qt::RichText);
		label->setTextInteractionFlags(Qt::TextBrowserInteraction);
		label->setOpenExternalLinks(true);
		l->addWidget(label);
		updates_->addWidget(c);
	}

	void rebuildUpdates(const ms_snapshot &s)
	{
		clearLayout(updates_);
		int waitingCount = 0;
		if (!connected_) {
			message(uiText("Overlays.Updates.NotConnected"));
		} else if (s.overlay_summary_state == MS_SUMMARY_UNSUPPORTED) {
			message(uiText("Overlays.Updates.OldRelay"), "warning");
		} else if (s.overlay_summary_state == MS_SUMMARY_NEED_SCOPE) {
			message(uiText("Overlays.Updates.NeedScope"), "warning");
		} else {
			const size_t apps = s.overlay_summaries ? obs_data_array_count(s.overlay_summaries) : 0;
			if (!apps)
				message(rows_.empty() ? uiText("Overlays.Updates.NoServer")
						      : uiText("Overlays.Updates.NoSummary"));
			for (size_t i = 0; i < apps; i++) {
				obs_data_t *app = obs_data_array_item(s.overlay_summaries, i);
				waitingCount += addServer(app);
				obs_data_release(app);
			}
		}
		updatesButton_->setText(waitingCount ? uiText("Overlays.Page.UpdatesCount").arg(waitingCount)
						     : uiText("Overlays.Page.Updates"));
	}

	/* One CTD Overlay server: its version, its templates, the overlays that wait. Returns how many wait. */
	int addServer(obs_data_t *app)
	{
		const QString ui = str(app, "ui");
		obs_data_t *server = obs_data_get_obj(app, "server");
		const QString version = str(server, "version");
		obs_data_release(server);

		QFrame *head = card(this);
		auto *hl = new QVBoxLayout(head);
		hl->setContentsMargins(12, 10, 12, 10);
		hl->setSpacing(4);
		auto *title = wrapped(str(app, "appName"), head, "title");
		title->setFont(ctd::font(ctd::Face::Heading, 15, QFont::Bold));
		hl->addWidget(title);
		QString line = uiText("Overlays.Updates.ServerVersion").arg(version.isEmpty() ? "?" : version);
		line += " · " + uiText(obs_data_get_bool(app, "appOnline") ? "Overlays.Online" : "Overlays.Offline");
		const QDateTime at = QDateTime::fromString(str(app, "receivedAt"), Qt::ISODateWithMs);
		if (at.isValid())
			line += " · " + uiText("Overlays.Updates.Received")
						.arg(QLocale().toString(at.toLocalTime(), QLocale::ShortFormat));
		hl->addWidget(wrapped(line, head, "muted"));
		if (!ui.isEmpty()) {
			auto *open = ctd::button(new QPushButton(uiText("Overlays.Updates.OpenOverlay"), head));
			QObject::connect(open, &QPushButton::clicked, this,
					 [ui] { QDesktopServices::openUrl(QUrl(ui)); });
			hl->addWidget(open, 0, Qt::AlignLeft);
		}
		updates_->addWidget(head);

		/* overlays waiting for the operator */
		obs_data_array_t *overlays = obs_data_get_array(app, "overlays");
		int waiting = 0;
		QFrame *waitCard = nullptr;
		QVBoxLayout *wl = nullptr;
		for (size_t i = 0; overlays && i < obs_data_array_count(overlays); i++) {
			obs_data_t *o = obs_data_array_item(overlays, i);
			obs_data_t *w = obs_data_get_obj(o, "waiting");
			if (w) {
				if (!waitCard) {
					waitCard = card(this);
					wl = new QVBoxLayout(waitCard);
					wl->setContentsMargins(12, 10, 12, 10);
					wl->setSpacing(6);
					wl->addWidget(ctd::eyebrow(uiText("Overlays.Updates.Waiting"), waitCard));
					wl->addWidget(
						wrapped(uiText("Overlays.Updates.Waiting.Note"), waitCard, "muted"));
				}
				waiting++;
				obs_data_t *t = obs_data_get_obj(o, "template");
				QString templ = str(t, "name");
				if (templ.isEmpty())
					templ = str(t, "id");
				const QString from = str(t, "version");
				obs_data_release(t);
				const bool major = obs_data_get_bool(w, "major");
				auto *row = new QHBoxLayout();
				row->setSpacing(8);
				auto *text =
					wrapped(QString("<b>%1</b> · %2 %3 → %4<br><span style='color:%5'>%6</span>")
							.arg(str(o, "slug").toHtmlEscaped(), templ.toHtmlEscaped(),
							     from.toHtmlEscaped(), str(w, "to").toHtmlEscaped(),
							     ctd::inkMuted().name(), str(w, "why").toHtmlEscaped()),
						waitCard);
				text->setTextFormat(Qt::RichText);
				row->addWidget(text, 1);
				row->addWidget(badge(uiText(major ? "Overlays.Badge.Major" : "Overlays.Badge.Waits"),
						     major ? ctd::magenta() : QColor(0xe5, 0xa5, 0x0a), waitCard),
					       0, Qt::AlignTop);
				wl->addLayout(row);
				const QByteArray u = ui.toUtf8(), slug = str(o, "slug").toUtf8();
				const QString upgrade =
					taken(ctd_overlay_admin_url(u.constData(), slug.constData(), true));
				auto *open =
					ctd::button(new QPushButton(uiText("Overlays.Updates.OpenUpgrade"), waitCard),
						    major ? ctd::Role::Hot : ctd::Role::Secondary);
				open->setEnabled(!upgrade.isEmpty());
				open->setToolTip(uiText("Overlays.Updates.OpenUpgrade.Tip"));
				QObject::connect(open, &QPushButton::clicked, this,
						 [upgrade] { QDesktopServices::openUrl(QUrl(upgrade)); });
				wl->addWidget(open, 0, Qt::AlignLeft);
			}
			obs_data_release(w);
			obs_data_release(o);
		}
		obs_data_array_release(overlays);
		if (waitCard)
			updates_->addWidget(waitCard);

		/* templates */
		obs_data_array_t *templates = obs_data_get_array(app, "templates");
		const size_t count = templates ? obs_data_array_count(templates) : 0;
		if (count)
			updates_->addWidget(ctd::eyebrow(uiText("Overlays.Updates.Templates"), this));
		for (size_t i = 0; i < count; i++) {
			obs_data_t *t = obs_data_array_item(templates, i);
			addTemplate(t, ui);
			obs_data_release(t);
		}
		obs_data_array_release(templates);
		return waiting;
	}

	void addTemplate(obs_data_t *t, const QString &ui)
	{
		QFrame *c = card(this);
		auto *l = new QVBoxLayout(c);
		l->setContentsMargins(12, 10, 12, 10);
		l->setSpacing(4);

		const QString installed = str(t, "installed");
		const QString latest = str(t, "latest");
		obs_data_t *source = obs_data_get_obj(t, "source");
		const QString kind = str(source, "kind");
		const QString repo = str(source, "repo");
		const bool pinned = obs_data_get_bool(source, "pinned");
		obs_data_release(source);
		const bool followed = !kind.isEmpty() && kind != "none";
		const bool autoUpdates = obs_data_get_bool(t, "auto");
		const QString problem = str(t, "problem");

		/* the update kind: CTD Overlay's, else worked out here from the oldest version in use */
		enum ctd_update_kind update = ctd_update_kind_parse(str(t, "update").toUtf8().constData());
		const QString inUse = str(t, "oldestInUse");
		const bool used = !inUse.isEmpty();
		const QString oldest = used ? inUse : installed;
		if (!obs_data_has_user_value(t, "update")) {
			const QString target =
				ctd_semver_compare(latest.toUtf8().constData(), installed.toUtf8().constData()) > 0
					? latest
					: installed;
			update = ctd_update_kind_of(oldest.toUtf8().constData(), target.toUtf8().constData());
		}

		auto *top = new QHBoxLayout();
		top->setSpacing(8);
		auto *name = wrapped(str(t, "name"), c, "title");
		name->setFont(ctd::font(ctd::Face::Mono, 12, QFont::Bold));
		top->addWidget(name, 1);
		QString badgeText;
		QColor badgeColor = ctd::inkMuted();
		if (!problem.isEmpty()) {
			badgeText = uiText("Overlays.Badge.Problem");
			badgeColor = QColor(0xe5, 0xa5, 0x0a);
		} else if (update == CTD_UPDATE_MAJOR) {
			badgeText = uiText("Overlays.Badge.Major");
			badgeColor = ctd::magenta();
		} else if (update != CTD_UPDATE_NONE) {
			badgeText = uiText(autoUpdates && followed ? "Overlays.Badge.Auto" : "Overlays.Badge.Waits")
					    .arg(uiText(update == CTD_UPDATE_MINOR ? "Overlays.Kind.Minor"
										   : "Overlays.Kind.Patch"));
			badgeColor = autoUpdates && followed ? ctd::cyan() : QColor(0xe5, 0xa5, 0x0a);
		} else if (!followed) {
			badgeText = uiText("Overlays.Badge.NotFollowed");
		} else {
			badgeText = uiText("Overlays.Badge.UpToDate");
		}
		top->addWidget(badge(badgeText, badgeColor, c), 0, Qt::AlignTop);
		l->addLayout(top);

		QString versions = uiText("Overlays.Updates.Installed").arg(installed);
		if (followed)
			versions += " · " + uiText("Overlays.Updates.Latest").arg(latest.isEmpty() ? "?" : latest);
		if (used && oldest != installed)
			versions += " · " + uiText("Overlays.Updates.InUse").arg(oldest);
		l->addWidget(wrapped(versions, c));

		QString how;
		if (!problem.isEmpty())
			how = uiText("Overlays.Updates.Problem").arg(problem);
		else if (!followed)
			how = uiText("Overlays.Updates.NotFollowed");
		else if (pinned)
			how = uiText("Overlays.Updates.Pinned");
		else if (update == CTD_UPDATE_MAJOR)
			how = uiText("Overlays.Updates.MajorWaits");
		else if (update != CTD_UPDATE_NONE && autoUpdates)
			how = uiText("Overlays.Updates.Moving");
		else if (!autoUpdates)
			how = uiText("Overlays.Updates.AutoOff");
		if (!how.isEmpty())
			l->addWidget(wrapped(how, c, "muted"));

		/* what changed: the newest release's notes, a few lines */
		obs_data_array_t *notes = obs_data_get_array(t, "notes");
		for (size_t i = 0; notes && i < obs_data_array_count(notes); i++) {
			obs_data_t *n = obs_data_array_item(notes, i);
			QString html = QString("<span style='color:%1'>%2</span>")
					       .arg(ctd::cyan().name(), uiText("Overlays.Updates.WhatChanged")
										.arg(str(n, "version"))
										.toHtmlEscaped());
			/* the lines come as one string, joined with "\n" (obs_data drops arrays of strings) */
			const QStringList lines = str(n, "text").split('\n', Qt::SkipEmptyParts);
			for (const QString &line : lines.mid(0, 8))
				html += "<br>• " + line.toHtmlEscaped();
			auto *label = wrapped(html, c);
			label->setTextFormat(Qt::RichText);
			l->addWidget(label);
			obs_data_release(n);
		}
		obs_data_array_release(notes);

		auto *links = new QHBoxLayout();
		links->setSpacing(6);
		const QByteArray r = repo.toUtf8(), v = latest.toUtf8();
		const QString release = taken(ctd_github_releases_url(r.constData(), v.constData()));
		if (!release.isEmpty()) {
			auto *b = ctd::button(new QPushButton(uiText("Overlays.Updates.ReleaseNotes"), c));
			QObject::connect(b, &QPushButton::clicked, this,
					 [release] { QDesktopServices::openUrl(QUrl(release)); });
			links->addWidget(b);
		}
		const QByteArray u = ui.toUtf8(), id = str(t, "id").toUtf8();
		const QString page = taken(ctd_template_admin_url(u.constData(), id.constData()));
		if (!page.isEmpty()) {
			auto *b = ctd::button(new QPushButton(uiText("Overlays.Updates.OpenTemplate"), c));
			QObject::connect(b, &QPushButton::clicked, this,
					 [page] { QDesktopServices::openUrl(QUrl(page)); });
			links->addWidget(b);
		}
		links->addStretch(1);
		l->addLayout(links);
		updates_->addWidget(c);
	}

	/* ------------------------------------------------------------ template kit --- */

	/* Downloads files of the template kit on a worker thread, then calls done(failed names) on the UI thread. */
	void fetchKit(const std::vector<std::string> &names, const QString &dir,
		      std::function<void(const QStringList &failed)> done)
	{
		auto alive = alive_;
		QPointer<OverlaysDock> self(this);
		std::thread([names, dir, done, alive, self] {
			QStringList failed;
			for (const std::string &name : names) {
				const std::string url = std::string(kKitRaw) + name;
				const QString path = QDir(dir).filePath(QString::fromStdString(name));
				QDir().mkpath(QFileInfo(path).absolutePath());
				const QByteArray p = QDir::toNativeSeparators(path).toUtf8();
				const enum http_status status = http_get_file(url.c_str(), nullptr, p.constData(),
									      nullptr, nullptr, nullptr, nullptr);
				if (status != HTTP_OK) {
					blog(LOG_WARNING,
					     "[CTD] overlays: couldn't download %s from the template kit (%s)",
					     name.c_str(), http_status_text(status));
					failed << QString::fromStdString(name);
				}
			}
			QMetaObject::invokeMethod(
				qApp,
				[failed, done, alive, self] {
					if (*alive && self)
						done(failed);
				},
				Qt::QueuedConnection);
		}).detach();
	}

	void kitSay(const QString &text)
	{
		kitStatus_->setText(text);
		kitStatus_->setVisible(!text.isEmpty());
	}

	void copyPrompt()
	{
		kitSay(uiText("Overlays.Kit.Fetching"));
		const QString dir =
			QDir::temp().filePath(QString("ctd-overlay-kit-%1").arg(QCoreApplication::applicationPid()));
		QDir().mkpath(dir);
		fetchKit({"PROMPT.md"}, dir, [this, dir](const QStringList &failed) {
			QFile file(QDir(dir).filePath("PROMPT.md"));
			if (!failed.isEmpty() || !file.open(QIODevice::ReadOnly)) {
				kitSay(uiText("Overlays.Kit.NoGithub"));
				return;
			}
			QApplication::clipboard()->setText(QString::fromUtf8(file.readAll()));
			file.close();
			file.remove();
			kitSay(uiText("Overlays.Kit.PromptCopied"));
		});
	}

	void downloadStarter()
	{
		const QString parent =
			QFileDialog::getExistingDirectory(this, uiText("Overlays.Kit.PickFolder"), QDir::homePath());
		if (parent.isEmpty())
			return;
		QString dir = QDir(parent).filePath(uiText("Overlays.Kit.FolderName"));
		for (int n = 2; QFileInfo::exists(dir); n++)
			dir = QDir(parent).filePath(uiText("Overlays.Kit.FolderName") + QString(" %1").arg(n));
		if (!QDir().mkpath(dir)) {
			kitSay(uiText("Overlays.Kit.CantWrite"));
			return;
		}
		kitSay(uiText("Overlays.Kit.Fetching"));
		std::vector<std::string> names(std::begin(kKitFiles), std::end(kKitFiles));
		fetchKit(names, dir, [this, dir](const QStringList &failed) {
			if (failed.size() == (int)std::size(kKitFiles)) {
				QDir(dir).removeRecursively();
				kitSay(uiText("Overlays.Kit.NoGithub"));
				return;
			}
			kitSay(failed.isEmpty() ? uiText("Overlays.Kit.Saved").arg(QDir::toNativeSeparators(dir))
						: uiText("Overlays.Kit.SavedSome")
							  .arg(QDir::toNativeSeparators(dir), failed.join(", ")));
			QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
		});
	}

	/* ------------------------------------------------------------------ state --- */

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

	QLabel *status_;
	PageBar *pages_;
	QPushButton *updatesButton_;
	QStackedWidget *stack_;

	QFrame *empty_;
	QLabel *emptyTitle_;
	QLabel *emptyText_;
	QPushButton *emptyButton_;
	QWidget *list_;
	QLineEdit *search_;
	QCheckBox *groupByShow_;
	QTreeWidget *tree_;
	QPushButton *add_;
	QPushButton *copy_;
	QPushButton *open_;
	QPushButton *panel_;
	QLabel *action_;

	QVBoxLayout *updates_;
	QLabel *kitStatus_;

	QTimer *timer_;
	QTimer *scan_;
	std::vector<Row> rows_;
	std::vector<QString> shownKeys_; /* rows_' keys when the tree was last built, for keeping the selection */
	std::map<QString, InCollection> collection_;
	Held<obs_data_array_t, obs_data_array_addref, obs_data_array_release> lastOverlays_, lastSummaries_;
	bool connected_ = false;
	enum ms_summary summaryState_ = MS_SUMMARY_UNKNOWN;
	QString profileId_;
	uint64_t lastSeq_ = UINT64_MAX;
	std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

QPointer<OverlaysDock> overlaysDock;

/* void ctd_overlays_show(): UI thread only. Shows the CTD Overlays dock. */
void procShow(void *, calldata_t *)
{
	if (overlaysDock)
		overlaysDock->showDockWindow();
}

} // namespace

extern "C" void ctd_overlays_dock_register(void)
{
	auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *dock = new OverlaysDock(main);
	if (!obs_frontend_add_dock_by_id(kOverlaysDockId, obs_module_text("Overlays.Title"), dock)) {
		delete dock;
		return;
	}
	overlaysDock = dock;
	proc_handler_add(obs_get_proc_handler(), "void ctd_overlays_show()", procShow, nullptr);
	blog(LOG_INFO, "[CTD] overlays: dock ready");
}
