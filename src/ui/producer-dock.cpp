/*
 * The CTD Producer dock: what a producer watches during a show, in one place. On top, how many people are watching
 * (every platform added up, and one chip per destination with its own count, canvas and state); below, one chat with
 * every platform's messages merged in the order they arrived.
 *
 * It is read-only and talks to nothing itself: everything comes from the CTD Relay client's snapshot (multistream.c),
 * which this widget reads up to ten times a second while it is visible. The logic with no Qt in it (totals, states,
 * the chat ring) is in producer-core.c, tested in test/producer.
 *
 * Drawing is kept cheap: a new message adds one block to the end of a plain-text document (nothing above it is laid
 * out again), the chips repaint only when their own values change, and nothing is drawn more often than the timer
 * ticks. Chat text is never logged.
 */

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QAbstractButton>
#include <QDesktopServices>
#include <QDockWidget>
#include <QEvent>
#include <QFocusEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLayout>
#include <QMainWindow>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QStringList>
#include <QStyle>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#include "../multistream.h"
#include "icons.hpp"
#include "producer-core.h"

namespace {

constexpr const char *kProducerDockId = "ctd_producer";
constexpr const char *kRelayDockId = "ctd-multistream";
constexpr int kRefreshMs = 100; /* at most ten redraws a second, however fast chat moves */

/* user-visible text, from data/locale */
QString uiText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString str(obs_data_t *data, const char *key)
{
	return QString::fromUtf8(data ? obs_data_get_string(data, key) : "");
}

QColor rgb(uint32_t value)
{
	return QColor((value >> 16) & 0xff, (value >> 8) & 0xff, value & 0xff);
}

/* the amber of the Relay dock's warnings: something is being put right, or needs a look */
QColor amber()
{
	return QColor(0xe5, 0xa5, 0x0a);
}

/* text that is there but shouldn't draw the eye (joined lines, a hidden filter) */
QColor inkDim()
{
	return QColor(0x5d, 0x70, 0x7b);
}

QString countText(long long n)
{
	char out[32];
	producer_format_count(n, out, sizeof(out));
	return QString::fromUtf8(out);
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

/* ------------------------------------------------------------- a layout that wraps --- */

/* Lays its items out left to right and wraps to a new row when the width runs out, so chips fit a dock of any
 * width. (Qt has no such layout built in; this is the usual shape of one.) With `grid`, every item gets the same
 * width: as many columns as fit the widest item, stretched to fill the row. */
class FlowLayout : public QLayout {
public:
	explicit FlowLayout(QWidget *parent, int hSpacing, int vSpacing, bool grid = false)
		: QLayout(parent),
		  hSpacing_(hSpacing),
		  vSpacing_(vSpacing),
		  grid_(grid)
	{
		setContentsMargins(0, 0, 0, 0);
	}
	~FlowLayout() override
	{
		while (QLayoutItem *item = takeAt(0))
			delete item;
	}

	void addItem(QLayoutItem *item) override { items_.push_back(item); }
	int count() const override { return static_cast<int>(items_.size()); }
	QLayoutItem *itemAt(int index) const override
	{
		return index >= 0 && index < count() ? items_[static_cast<size_t>(index)] : nullptr;
	}
	QLayoutItem *takeAt(int index) override
	{
		if (index < 0 || index >= count())
			return nullptr;
		QLayoutItem *item = items_[static_cast<size_t>(index)];
		items_.erase(items_.begin() + index);
		return item;
	}
	Qt::Orientations expandingDirections() const override { return {}; }
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), false); }
	void setGeometry(const QRect &rect) override
	{
		QLayout::setGeometry(rect);
		arrange(rect, true);
	}
	QSize sizeHint() const override { return minimumSize(); }
	QSize minimumSize() const override
	{
		/* one item per row is the narrowest it can go */
		QSize size;
		for (const QLayoutItem *item : items_)
			if (!item->isEmpty())
				size = size.expandedTo(item->minimumSize());
		return size;
	}

private:
	int arrange(const QRect &rect, bool apply) const
	{
		/* grid: the column width that fits the widest item, then stretched so the columns fill the row */
		int column = 0;
		if (grid_) {
			for (const QLayoutItem *item : items_)
				if (!item->isEmpty())
					column = qMax(column, item->sizeHint().width());
			const int columns = qMax(1, (rect.width() + hSpacing_) / qMax(1, column + hSpacing_));
			column = (rect.width() - (columns - 1) * hSpacing_) / columns;
		}
		int x = rect.x(), y = rect.y(), rowHeight = 0;
		for (QLayoutItem *item : items_) {
			if (item->isEmpty())
				continue; /* a hidden chip takes no room */
			QSize size = item->sizeHint();
			if (grid_)
				size.setWidth(qMax(1, column));
			if (x > rect.x() && x + size.width() > rect.right() + 1) {
				x = rect.x();
				y += rowHeight + vSpacing_;
				rowHeight = 0;
			}
			if (apply)
				item->setGeometry(QRect(QPoint(x, y), size));
			x += size.width() + hSpacing_;
			rowHeight = qMax(rowHeight, size.height());
		}
		return y + rowHeight - rect.y();
	}

	std::vector<QLayoutItem *> items_;
	int hSpacing_;
	int vSpacing_;
	bool grid_;
};

/* ------------------------------------------------------------- destination chips --- */

struct DestInfo {
	QString id;
	QString platform;
	QString name;
	QString canvas; /* "landscape" or "vertical" */
	QString message;
	enum producer_state state = PRODUCER_OFFLINE;
	long long viewers = -1; /* -1: unknown */

	bool operator==(const DestInfo &o) const
	{
		return id == o.id && platform == o.platform && name == o.name && canvas == o.canvas &&
		       message == o.message && state == o.state && viewers == o.viewers;
	}
};

QString stateText(enum producer_state state)
{
	switch (state) {
	case PRODUCER_LIVE:
		return uiText("Producer.State.Live");
	case PRODUCER_STARTING:
		return uiText("Producer.State.Starting");
	case PRODUCER_RECOVERING:
		return uiText("Producer.State.Recovering");
	case PRODUCER_WAITING:
		return uiText("Producer.State.Waiting");
	case PRODUCER_DOWN:
		return uiText("Producer.State.Down");
	case PRODUCER_OFFLINE:
		break;
	}
	return uiText("Producer.State.Offline");
}

/* live is magenta (on air), as in the Relay dock; red only ever sits beside the word DOWN */
QColor stateColor(enum producer_state state)
{
	switch (state) {
	case PRODUCER_LIVE:
		return ctd::magenta();
	case PRODUCER_STARTING:
		return ctd::cyan();
	case PRODUCER_RECOVERING:
	case PRODUCER_WAITING:
		return amber();
	case PRODUCER_DOWN:
		return ctd::tallyRed();
	case PRODUCER_OFFLINE:
		break;
	}
	return ctd::inkMuted();
}

QString canvasText(const QString &canvas)
{
	if (canvas == "vertical")
		return uiText("Producer.Canvas.Vertical");
	if (canvas == "landscape")
		return uiText("Producer.Canvas.Landscape");
	return canvas;
}

/* One destination: the platform's badge, its name, which canvas feeds it, its state and its viewers. Painted by
 * hand so it looks the same under the plugin's own stylesheet and under CTD OBS's theme, and repaints only itself. */
class DestChip : public QWidget {
public:
	explicit DestChip(QWidget *parent) : QWidget(parent)
	{
		setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
		nameFont_ = ctd::font(ctd::Face::Mono, 12, QFont::Bold);
		smallFont_ = ctd::font(ctd::Face::Mono, 9, QFont::Bold, true, 0.5);
		badgeFont_ = ctd::font(ctd::Face::Mono, 10, QFont::Bold);
		countFont_ = ctd::font(ctd::Face::Heading, 15, QFont::Bold);
	}

	void setInfo(const DestInfo &info)
	{
		if (info == info_ && built_)
			return;
		const QSize before = sizeHint();
		info_ = info;
		built_ = true;
		const struct producer_platform *platform = producer_platform(info.platform.toUtf8().constData());
		badge_ = QString::fromUtf8(platform->badge);
		color_ = rgb(platform->rgb);
		state_ = stateText(info.state);
		canvas_ = canvasText(info.canvas);
		count_ = countText(info.viewers);
		QString tip = info.name;
		if (*platform->name && info.name.compare(QString::fromUtf8(platform->name), Qt::CaseInsensitive) != 0)
			tip += QString(" (%1)").arg(QString::fromUtf8(platform->name));
		tip += " · " + canvas_ + " · " + state_ + " · " +
		       (info.viewers >= 0 ? uiText("Producer.Chip.Viewers").arg(count_)
					  : uiText("Producer.Chip.ViewersUnknown"));
		if (!info.message.isEmpty())
			tip += "\n" + info.message;
		setToolTip(tip);
		setAccessibleName(tip);
		if (sizeHint() != before)
			updateGeometry();
		update();
	}

	/* wide enough for both rows; a very long name is cut with "…" rather than widening every chip */
	QSize sizeHint() const override
	{
		const QFontMetrics name(nameFont_), small(smallFont_), count(countFont_);
		const int top =
			qMin(name.horizontalAdvance(info_.name), kNameMax) + kGap + count.horizontalAdvance(count_);
		const int bottom =
			kCanvasIcon + 5 + small.horizontalAdvance(canvas_) + kGap + small.horizontalAdvance(state_);
		return QSize(kPad + kBadge + 8 + qMax(top, bottom) + kPad, kHeight);
	}
	QSize minimumSizeHint() const override { return sizeHint(); }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		const QColor accent = stateColor(info_.state);
		const bool on = info_.state != PRODUCER_OFFLINE;
		p.fillRect(rect(), ctd::surface());
		p.setPen(on ? accent : ctd::line());
		p.drawRect(rect().adjusted(0, 0, -1, -1));
		if (on) /* a bar down the side in the state's colour, so the state reads at a glance */
			p.fillRect(QRect(0, 0, 3, height()), accent);

		/* the platform's badge */
		const QRect badge(kPad, (height() - kBadge) / 2, kBadge, kBadge);
		p.fillRect(badge, on ? color_ : ctd::lineStrong());
		p.setFont(badgeFont_);
		p.setPen(on ? ctd::onAccent() : ctd::inkMuted());
		p.drawText(badge, Qt::AlignCenter, badge_);

		const QFontMetrics name(nameFont_), small(smallFont_), count(countFont_);
		const int left = badge.right() + 9;
		const int right = width() - kPad;

		/* first row: the name, and its viewers (a dash while the platform doesn't say) */
		const int countWidth = count.horizontalAdvance(count_);
		const QRect top(left, 5, right - left, height() / 2 - 4);
		p.setFont(countFont_);
		p.setPen(info_.viewers >= 0 ? ctd::ink() : inkDim());
		p.drawText(top, Qt::AlignRight | Qt::AlignVCenter, count_);
		p.setFont(nameFont_);
		p.setPen(on ? ctd::ink() : ctd::inkBody());
		p.drawText(top, Qt::AlignLeft | Qt::AlignVCenter,
			   name.elidedText(info_.name, Qt::ElideRight, qMax(0, top.width() - countWidth - kGap)));

		/* second row: the canvas's shape and name, and the state */
		const QRect bottom(left, height() / 2 + 1, right - left, height() / 2 - 6);
		const QRect shape = info_.canvas == "vertical" ? QRect(left + 2, bottom.center().y() - 5, 7, 11)
							       : QRect(left, bottom.center().y() - 3, 11, 7);
		p.setPen(ctd::inkMuted());
		p.drawRect(shape.adjusted(0, 0, -1, -1));
		p.setFont(smallFont_);
		p.drawText(bottom.adjusted(kCanvasIcon + 5, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, canvas_);
		p.setPen(accent);
		p.drawText(bottom, Qt::AlignRight | Qt::AlignVCenter, state_);
	}

private:
	static constexpr int kPad = 9;
	static constexpr int kBadge = 26;
	static constexpr int kHeight = 44;
	static constexpr int kCanvasIcon = 11;
	static constexpr int kGap = 12;      /* between what is on the left of a row and what is on its right */
	static constexpr int kNameMax = 130; /* a name's share of the chip's width */

	DestInfo info_;
	bool built_ = false;
	QString badge_, state_, canvas_, count_;
	QColor color_;
	QFont nameFont_, smallFont_, badgeFont_, countFont_;
};

/* ------------------------------------------------------------- filter chips --- */

/* A small switch above the chat: a platform (or "joined" lines) shown while it is on. Keyboard and screen-reader
 * friendly because it is a real checkable button; painted by hand for the same reason as DestChip. */
class FilterChip : public QAbstractButton {
public:
	FilterChip(const QString &text, const QColor &color, QWidget *parent) : QAbstractButton(parent), color_(color)
	{
		setText(text);
		setCheckable(true);
		setChecked(true);
		setCursor(Qt::PointingHandCursor);
		setFocusPolicy(Qt::TabFocus);
		setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
		setFont(ctd::font(ctd::Face::Mono, 10, QFont::Bold, true, 1.0));
	}

	QSize sizeHint() const override
	{
		return QSize(8 + kDot + 6 + fontMetrics().horizontalAdvance(text()) + 9, 22);
	}
	QSize minimumSizeHint() const override { return sizeHint(); }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		const bool on = isChecked();
		p.fillRect(rect(), on ? ctd::surfaceRaised() : ctd::voidColor());
		p.setPen(keyboardFocus_ || underMouse() ? ctd::cyan() : on ? ctd::lineStrong() : ctd::line());
		p.drawRect(rect().adjusted(0, 0, -1, -1));
		const QRect dot(8, (height() - kDot) / 2, kDot, kDot);
		if (on) {
			p.fillRect(dot, color_);
		} else {
			p.setPen(inkDim());
			p.drawRect(dot.adjusted(0, 0, -1, -1));
		}
		p.setPen(on ? ctd::ink() : inkDim());
		p.drawText(rect().adjusted(8 + kDot + 6, 0, -9, 0), Qt::AlignLeft | Qt::AlignVCenter, text());
	}
	void enterEvent(QEnterEvent *event) override
	{
		update();
		QAbstractButton::enterEvent(event);
	}
	void leaveEvent(QEvent *event) override
	{
		update();
		QAbstractButton::leaveEvent(event);
	}
	/* the focus outline is for the keyboard: a click, or the window coming to the front, doesn't leave one behind */
	void focusInEvent(QFocusEvent *event) override
	{
		keyboardFocus_ = event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason ||
				 event->reason() == Qt::ShortcutFocusReason;
		QAbstractButton::focusInEvent(event);
	}
	void focusOutEvent(QFocusEvent *event) override
	{
		keyboardFocus_ = false;
		QAbstractButton::focusOutEvent(event);
	}

private:
	static constexpr int kDot = 7;
	bool keyboardFocus_ = false;
	QColor color_;
};

/* ------------------------------------------------------------- the dock --- */

class ProducerDock : public QWidget {
public:
	explicit ProducerDock(QWidget *parent) : QWidget(parent), model_(producer_chat_create())
	{
		ctd::applyTheme(this);
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(9, 9, 9, 9);
		layout->setSpacing(8);
		layout->addLayout(ctdHeader(this, "relay-64.png", uiText("Producer.Title")));

		/* viewers: the total, big, and what the session is doing */
		auto *top = new QHBoxLayout();
		top->setSpacing(10);
		auto *totalColumn = new QVBoxLayout();
		totalColumn->setSpacing(0);
		totalColumn->addWidget(ctd::eyebrow(uiText("Producer.Viewers"), this));
		total_ = new QLabel(this);
		total_->setFont(ctd::font(ctd::Face::Display, 38, QFont::Bold));
		total_->setProperty("ctdRole", "title");
		total_->setTextFormat(Qt::PlainText);
		totalColumn->addWidget(total_);
		top->addLayout(totalColumn);
		top->addStretch(1);
		session_ = new QLabel(this);
		session_->setAlignment(Qt::AlignRight | Qt::AlignTop);
		session_->setTextFormat(Qt::RichText);
		session_->setFont(ctd::font(ctd::Face::Mono, 11));
		session_->setMinimumWidth(40);
		top->addWidget(session_, 0, Qt::AlignTop);
		layout->addLayout(top);

		/* one chip per destination */
		chipsHost_ = new QWidget(this);
		chips_ = new FlowLayout(chipsHost_, 6, 6, true);
		layout->addWidget(chipsHost_);
		noDestinations_ = new QLabel(uiText("Producer.NoDestinations"), this);
		noDestinations_->setWordWrap(true);
		noDestinations_->setFont(ctd::font(ctd::Face::Mono, 12));
		noDestinations_->setProperty("ctdRole", "muted");
		layout->addWidget(noDestinations_);

		/* what is in the way of showing things, and the button that fixes it */
		banner_ = new QFrame(this);
		auto *bannerLayout = new QVBoxLayout(banner_);
		bannerLayout->setContentsMargins(0, 0, 0, 0);
		bannerLayout->setSpacing(6);
		bannerText_ = new QLabel(banner_);
		bannerText_->setWordWrap(true);
		bannerText_->setFont(ctd::font(ctd::Face::Mono, 12));
		bannerText_->setTextFormat(Qt::PlainText);
		bannerLayout->addWidget(bannerText_);
		bannerButton_ = ctd::button(new QPushButton(banner_), ctd::Role::Primary);
		bannerLayout->addWidget(bannerButton_, 0, Qt::AlignLeft);
		layout->addWidget(banner_);
		QObject::connect(bannerButton_, &QPushButton::clicked, this, [this] { bannerClicked(); });

		/* chat: which platforms to show, then the merged list */
		auto *chatHead = new QHBoxLayout();
		chatHead->addWidget(ctd::eyebrow(uiText("Producer.Chat"), this));
		chatHead->addStretch(1);
		layout->addLayout(chatHead);
		filtersHost_ = new QWidget(this);
		filters_ = new FlowLayout(filtersHost_, 5, 5);
		joins_ = new FilterChip(uiText("Producer.Filter.Joined"), ctd::inkMuted(), filtersHost_);
		joins_->setToolTip(uiText("Producer.Filter.Joined.Tooltip"));
		filters_->addWidget(joins_);
		QObject::connect(joins_, &QAbstractButton::toggled, this, [this] { filtersChanged(); });
		layout->addWidget(filtersHost_);

		chat_ = new QPlainTextEdit(this);
		chat_->setReadOnly(true);
		chat_->setUndoRedoEnabled(false);
		chat_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
		chat_->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
		chat_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		chat_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
		chat_->setFrameShape(QFrame::NoFrame);
		chat_->setMinimumHeight(70);
		chat_->setFont(ctd::font(ctd::Face::Mono, 12));
		chat_->setPlaceholderText(uiText("Producer.Chat.Empty"));
		/* its own rule, so it looks the same under CTD OBS's theme (which has no rule for this widget) */
		chat_->setStyleSheet("QPlainTextEdit { background: #08111a; color: #b9c6ce; border: 1px solid #1a2127;"
				     " border-radius: 4px; selection-background-color: #39e7ff;"
				     " selection-color: #04070c; }");
		chat_->document()->setDocumentMargin(6);
		layout->addWidget(chat_, 1);

		/* "N new messages": over the bottom of the chat while it is scrolled up */
		jump_ = ctd::button(new QPushButton(chat_), ctd::Role::Primary);
		jump_->setGraphicsEffect(nullptr); /* no offset shadow over the text behind it */
		jump_->hide();
		chat_->installEventFilter(this);
		QObject::connect(jump_, &QPushButton::clicked, this, [this] { follow(); });

		/* Following or reading back is decided once the scroll bar has settled: a resize or an append moves its
		 * range and its value one after the other. While following, a longer list keeps its end in view. */
		scrollCheck_ = new QTimer(this);
		scrollCheck_->setSingleShot(true);
		scrollCheck_->setInterval(0);
		QObject::connect(scrollCheck_, &QTimer::timeout, this, [this] { scrolled(); });
		QScrollBar *bar = chat_->verticalScrollBar();
		QObject::connect(bar, &QScrollBar::valueChanged, this, [this] { scrollCheck_->start(); });
		QObject::connect(bar, &QScrollBar::rangeChanged, this, [this, bar] {
			if (following_)
				bar->setValue(bar->maximum());
		});

		buildFormats();
		loadSettings();

		timer_ = new QTimer(this);
		QObject::connect(timer_, &QTimer::timeout, this, [this] { refresh(); });
		timer_->start(kRefreshMs);
		refresh(true);
	}

	~ProducerDock() override { producer_chat_destroy(model_); }

	/* the dock in view (the CTD Relay dock's button, CTD OBS's ctd_producer_show proc) */
	void showDockWindow() { showDock(kProducerDockId); }

protected:
	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		refresh(true); /* nothing was drawn while it was hidden */
	}

	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (watched == chat_ && event->type() == QEvent::Resize)
			placeJump();
		return QWidget::eventFilter(watched, event);
	}

private:
	/* ------------------------------------------------------------- snapshot --- */

	void refresh(bool force = false)
	{
		if (!isVisible() && !force)
			return;
		struct ms_snapshot s;
		ms_snapshot_get(&s);
		if (s.seq != lastSeq_ || force) {
			lastSeq_ = s.seq;
			showBanner(s);
			if (lastStatus_.changed(s.status) || lastProfiles_.changed(s.profiles) || force ||
			    QString::fromUtf8(s.profile_id) != lastProfileId_) {
				lastProfileId_ = QString::fromUtf8(s.profile_id);
				showStatus(s);
			}
		}
		if (s.chat_seq != lastChatSeq_ || force) {
			lastChatSeq_ = s.chat_seq;
			syncChat(s);
		}
		ms_snapshot_free(&s);
	}

	/* ------------------------------------------------------------- connection and permissions --- */

	enum class Banner { None, Approve, NotConnected, PairAgain };

	void showBanner(const ms_snapshot &s)
	{
		const QString code = QString::fromUtf8(s.pairing_code);
		approveUrl_ = QString::fromUtf8(s.approve_url);
		Banner banner = Banner::None;
		QString text, button;
		if (!code.isEmpty()) {
			/* a pairing is waiting to be approved (asked for here or in the CTD Relay dock) */
			banner = Banner::Approve;
			text = uiText("Producer.Pair.Approve").arg(code);
			button = uiText("Relay.OpenApproval");
		} else if (s.link == MS_PAIRING) {
			banner = Banner::Approve;
			text = QString::fromUtf8(s.message);
		} else if (s.link != MS_CONNECTED) {
			banner = Banner::NotConnected;
			text = uiText("Producer.NotConnected");
			if (s.link == MS_ERROR && *s.message)
				text += "\n" + QString::fromUtf8(s.message);
			button = uiText("Producer.OpenRelay");
		} else if (s.chat_need_scope || s.status_need_scope) {
			/* connected, but whoever approved the pairing left a permission out (or the pairing is older than
			 * it): say which, and offer the pairing again. The connection stays up until the new one is approved. */
			banner = Banner::PairAgain;
			if (s.chat_need_scope && s.status_need_scope) {
				text = uiText("Producer.Scope.Both");
				button = uiText("Producer.Scope.Both.Button");
			} else if (s.chat_need_scope) {
				text = uiText("Producer.Scope.Chat");
				button = uiText("Producer.Scope.Chat.Button");
			} else {
				text = uiText("Producer.Scope.Status");
				button = uiText("Producer.Scope.Status.Button");
			}
		}
		bannerKind_ = banner;
		bannerText_->setText(text);
		bannerButton_->setText(button);
		bannerButton_->setVisible(!button.isEmpty() && (banner != Banner::Approve || !approveUrl_.isEmpty()));
		banner_->setVisible(banner != Banner::None);
		/* the banner above already says why there is no chat */
		const bool blocked = s.link != MS_CONNECTED || s.chat_need_scope;
		chat_->setPlaceholderText(blocked ? QString() : uiText("Producer.Chat.Empty"));
	}

	void bannerClicked()
	{
		switch (bannerKind_) {
		case Banner::Approve:
			if (!approveUrl_.isEmpty())
				QDesktopServices::openUrl(QUrl(approveUrl_));
			break;
		case Banner::NotConnected:
			showDock(kRelayDockId);
			break;
		case Banner::PairAgain:
			/* the same pairing as the CTD Relay dock's Connect: it asks for chat and status among the rest */
			blog(LOG_INFO, "[CTD] producer: pairing again for the missing permission");
			ms_start_pairing();
			break;
		case Banner::None:
			break;
		}
	}

	/* ------------------------------------------------------------- viewers --- */

	void showStatus(const ms_snapshot &s)
	{
		std::vector<DestInfo> dests;
		std::vector<struct producer_dest> counts;
		obs_data_array_t *destinations = s.status ? obs_data_get_array(s.status, "destinations") : nullptr;
		const size_t count = destinations ? obs_data_array_count(destinations) : 0;
		for (size_t i = 0; i < count; i++) {
			obs_data_t *d = obs_data_array_item(destinations, i);
			/* a destination that is switched off in CTD Relay isn't part of this show */
			if (obs_data_get_bool(d, "enabled") || obs_data_get_bool(d, "live")) {
				struct producer_dest c = {};
				c.live = obs_data_get_bool(d, "live");
				/* CTD Relay sends null while a count is unknown; null is no value here, never 0 */
				c.has_viewers = obs_data_has_user_value(d, "viewers");
				c.viewers = obs_data_get_double(d, "viewers");
				DestInfo info;
				info.id = str(d, "id");
				info.platform = str(d, "platform");
				info.name = str(d, "name");
				if (info.name.isEmpty())
					info.name = QString::fromUtf8(
						producer_platform(info.platform.toUtf8().constData())->name);
				info.canvas = str(d, "feed");
				info.message = str(d, "message");
				info.state = producer_dest_state(obs_data_get_string(d, "phase"), c.live,
								 obs_data_get_bool(d, "down"));
				info.viewers = producer_dest_viewers(&c);
				dests.push_back(info);
				counts.push_back(c);
				notePlatform(info.platform);
			}
			obs_data_release(d);
		}
		obs_data_array_release(destinations);

		long long total = 0;
		const bool known = producer_total_viewers(counts.data(), counts.size(), &total);
		total_->setText(countText(known ? total : -1));
		total_->setToolTip(uiText(known ? "Producer.Viewers.Tooltip" : "Producer.Viewers.Unknown"));

		/* the profile's name, and what its session is doing */
		QString profileName;
		const size_t profiles = s.profiles ? obs_data_array_count(s.profiles) : 0;
		for (size_t i = 0; i < profiles && profileName.isEmpty(); i++) {
			obs_data_t *profile = obs_data_array_item(s.profiles, i);
			if (str(profile, "id") == lastProfileId_)
				profileName = str(profile, "name");
			obs_data_release(profile);
		}
		obs_data_t *session = s.status ? obs_data_get_obj(s.status, "session") : nullptr;
		const QString state = str(session, "status");
		QStringList lines;
		if (state == "live")
			lines << QString("<b style='color:#ff00b8'>%1</b>").arg(uiText("Relay.Session.Live"));
		else if (state == "starting")
			lines << QString("<b style='color:#39e7ff'>%1</b>").arg(uiText("Producer.State.Starting"));
		else if (s.status)
			lines << QString("<span style='color:#8ba3af'>%1</span>").arg(uiText("Producer.State.Offline"));
		if (session && obs_data_get_bool(session, "testMode") && !lines.isEmpty())
			lines.last() += QString(" <b style='color:#39e7ff'>%1</b>").arg(uiText("Relay.Session.Test"));
		if (!profileName.isEmpty())
			lines << QString("<span style='color:#8ba3af'>%1</span>").arg(profileName.toHtmlEscaped());
		obs_data_release(session);
		session_->setText(lines.join("<br>"));

		/* chips: made again only when the set of destinations changed, otherwise each updates itself */
		bool same = dests.size() == destIds_.size();
		for (size_t i = 0; same && i < dests.size(); i++)
			same = dests[i].id == destIds_[i];
		if (!same) {
			while (QLayoutItem *item = chips_->takeAt(0)) {
				delete item->widget();
				delete item;
			}
			chipWidgets_.clear();
			destIds_.clear();
			for (const DestInfo &info : dests) {
				auto *chip = new DestChip(chipsHost_);
				chips_->addWidget(chip);
				chipWidgets_.push_back(chip);
				destIds_.push_back(info.id);
			}
		}
		for (size_t i = 0; i < dests.size(); i++)
			chipWidgets_[i]->setInfo(dests[i]);
		chipsHost_->setVisible(!dests.empty());
		noDestinations_->setVisible(dests.empty() && s.link == MS_CONNECTED && s.status &&
					    !s.status_need_scope);
	}

	/* ------------------------------------------------------------- chat: the model --- */

	struct LoadContext {
		const std::vector<obs_data_t *> *envelopes;
		obs_data_t *data = nullptr;
		obs_data_t *user = nullptr;
		void release()
		{
			obs_data_release(user);
			obs_data_release(data);
			data = user = nullptr;
		}
	};

	/* one chat.message envelope (ctd-relay docs/API.md) as a line; the strings live until the next call */
	static bool loadLine(void *opaque, size_t index, struct producer_line_in *out)
	{
		auto *ctx = static_cast<LoadContext *>(opaque);
		ctx->release();
		obs_data_t *envelope = (*ctx->envelopes)[index];
		ctx->data = obs_data_get_obj(envelope, "data");
		if (!ctx->data)
			return false;
		ctx->user = obs_data_get_obj(ctx->data, "user");
		out->platform = obs_data_get_string(ctx->data, "platform");
		out->author = ctx->user ? obs_data_get_string(ctx->user, "name") : "";
		out->text = obs_data_get_string(ctx->data, "text");
		out->color = ctx->user ? obs_data_get_string(ctx->user, "color") : "";
		out->join = obs_data_get_bool(ctx->data, "join");
		out->event = strcmp(obs_data_get_string(ctx->data, "kind"), "event") == 0;
		out->highlighted = obs_data_get_bool(ctx->data, "highlighted");
		out->action = obs_data_get_bool(ctx->data, "action");
		out->ts = obs_data_get_int(envelope, "ts");
		return true;
	}

	void syncChat(const ms_snapshot &s)
	{
		const size_t count = s.chat ? obs_data_array_count(s.chat) : 0;
		std::vector<obs_data_t *> envelopes(count);
		std::vector<const char *> ids(count);
		for (size_t i = 0; i < count; i++) {
			envelopes[i] = obs_data_array_item(s.chat, i);
			ids[i] = obs_data_get_string(envelopes[i], "id");
		}
		LoadContext ctx;
		ctx.envelopes = &envelopes;
		struct producer_sync result = {};
		const uint64_t newestBefore = producer_chat_newest_seq(model_);
		producer_chat_sync(model_, ids.data(), count, loadLine, &ctx, &result);
		ctx.release();
		for (obs_data_t *envelope : envelopes)
			obs_data_release(envelope);

		/* a platform seen for the first time gets its filter chip */
		if (result.appended) {
			for (size_t i = producer_chat_count(model_); i > 0; i--) {
				const struct producer_line *line = producer_chat_at(model_, i - 1);
				if (line->seq <= newestBefore)
					break;
				notePlatform(QString::fromUtf8(line->platform));
			}
		}
		if (result.appended || result.removed_front || result.removed_inner)
			showChat(result.removed_inner > 0);
	}

	/* ------------------------------------------------------------- chat: filters --- */

	void notePlatform(const QString &platform)
	{
		if (platform.isEmpty() || filterChips_.contains(platform))
			return;
		const struct producer_platform *known = producer_platform(platform.toUtf8().constData());
		auto *chip = new FilterChip(*known->name ? QString::fromUtf8(known->name) : platform, rgb(known->rgb),
					    filtersHost_);
		chip->setChecked(!hiddenPlatforms_.contains(platform));
		chip->setToolTip(uiText("Producer.Filter.Platform.Tooltip").arg(chip->text()));
		/* platforms first, the joined switch last */
		filters_->removeWidget(joins_);
		filters_->addWidget(chip);
		filters_->addWidget(joins_);
		filterChips_.insert(platform, chip);
		QObject::connect(chip, &QAbstractButton::toggled, this, [this, platform](bool on) {
			if (on)
				hiddenPlatforms_.removeAll(platform);
			else if (!hiddenPlatforms_.contains(platform))
				hiddenPlatforms_ << platform;
			filtersChanged();
		});
		chip->show();
	}

	void filtersChanged()
	{
		if (loading_)
			return;
		saveSettings();
		showChat(true);
	}

	/* the filter as producer-core wants it; valid until hiddenPlatforms_ changes */
	struct Filter {
		std::vector<QByteArray> names;
		std::vector<const char *> pointers;
		struct producer_filter filter = {};
	};
	void currentFilter(Filter &f) const
	{
		for (const QString &platform : hiddenPlatforms_)
			f.names.push_back(platform.toUtf8());
		for (const QByteArray &name : f.names)
			f.pointers.push_back(name.constData());
		f.filter.hidden_platforms = f.pointers.data();
		f.filter.hidden_count = f.pointers.size();
		f.filter.hide_joins = !joins_->isChecked();
	}

	/* which platforms and whether joined lines are hidden: kept between sessions, in the plugin's own config folder */
	static char *settingsFile()
	{
		char *dir = obs_module_config_path("");
		if (dir) {
			os_mkdirs(dir);
			bfree(dir);
		}
		return obs_module_config_path("producer.json");
	}

	void loadSettings()
	{
		loading_ = true;
		char *file = settingsFile();
		obs_data_t *data = file ? obs_data_create_from_json_file(file) : nullptr;
		bfree(file);
		if (data) {
			joins_->setChecked(!obs_data_get_bool(data, "hide_joins"));
			obs_data_array_t *hidden = obs_data_get_array(data, "hidden_platforms");
			for (size_t i = 0; hidden && i < obs_data_array_count(hidden); i++) {
				obs_data_t *item = obs_data_array_item(hidden, i);
				const QString platform = str(item, "id");
				if (!platform.isEmpty() && !hiddenPlatforms_.contains(platform))
					hiddenPlatforms_ << platform;
				obs_data_release(item);
			}
			obs_data_array_release(hidden);
			obs_data_release(data);
		}
		loading_ = false;
	}

	void saveSettings()
	{
		obs_data_t *data = obs_data_create();
		obs_data_set_bool(data, "hide_joins", !joins_->isChecked());
		obs_data_array_t *hidden = obs_data_array_create();
		for (const QString &platform : hiddenPlatforms_) {
			obs_data_t *item = obs_data_create();
			obs_data_set_string(item, "id", platform.toUtf8().constData());
			obs_data_array_push_back(hidden, item);
			obs_data_release(item);
		}
		obs_data_set_array(data, "hidden_platforms", hidden);
		obs_data_array_release(hidden);
		char *file = settingsFile();
		if (file)
			obs_data_save_json_safe(data, file, "tmp", "bak");
		bfree(file);
		obs_data_release(data);
	}

	/* ------------------------------------------------------------- chat: the view --- */

	void buildFormats()
	{
		text_.setForeground(ctd::inkBody());
		strong_.setForeground(ctd::ink());
		notice_.setForeground(ctd::cyan());
		dim_.setForeground(inkDim());
		dim_.setFontItalic(true);
		marker_.setForeground(ctd::magenta());
		marker_.setFontWeight(QFont::Bold);
	}

	/* One line at the end of the document: a platform badge, the author in their colour, the text. Emotes and emoji
	 * are the text CTD Relay sends for them (v1 loads no images). */
	void appendLine(QTextCursor &cursor, const struct producer_line *line)
	{
		if (!shown_.empty())
			cursor.insertBlock();
		const struct producer_platform *platform = producer_platform(line->platform);
		const QColor platformColor = rgb(platform->rgb);
		const QString author = QString::fromUtf8(line->author);
		QString text = QString::fromUtf8(line->text);

		QTextCharFormat badge;
		badge.setFontWeight(QFont::Bold);
		if (line->join) {
			/* someone joined: there, but out of the way of the comments */
			badge.setForeground(inkDim());
			cursor.insertText(QString(" %1 ").arg(QString::fromUtf8(platform->badge)), badge);
			if (text.isEmpty())
				text = uiText("Relay.Chat.Joined").arg(author);
			cursor.insertText(" " + text, dim_);
			return;
		}
		badge.setBackground(platformColor);
		badge.setForeground(ctd::onAccent());
		cursor.insertText(QString(" %1 ").arg(QString::fromUtf8(platform->badge)), badge);
		cursor.insertText(" ", text_);
		if (line->event) {
			/* a platform notice (a sub, a raid, a song request's answer): its own line says who and what */
			cursor.insertText(text.isEmpty() ? author : text, notice_);
			return;
		}
		if (line->highlighted)
			cursor.insertText(QString::fromUtf8("\xe2\x97\x86 "),
					  marker_); /* ◆ a highlighted or paid message */
		QTextCharFormat name;
		name.setFontWeight(QFont::Bold);
		name.setForeground(line->has_color ? rgb(producer_readable_color(line->color)) : platformColor);
		if (!author.isEmpty()) {
			cursor.insertText(author, name);
			cursor.insertText(line->action ? " " : ": ", text_);
		}
		if (line->action) {
			name.setFontWeight(QFont::Normal);
			name.setFontItalic(true);
			cursor.insertText(text, name);
		} else {
			cursor.insertText(text, line->highlighted ? strong_ : text_);
		}
	}

	/* the lines the ring holds that are newer than what the view has seen, added at the end */
	void appendNew(const struct producer_filter &filter, uint64_t upTo = UINT64_MAX)
	{
		const size_t count = producer_chat_count(model_);
		size_t first = count;
		while (first > 0 && producer_chat_at(model_, first - 1)->seq > seen_)
			first--;
		if (first == count)
			return;
		QTextCursor cursor(chat_->document());
		cursor.movePosition(QTextCursor::End);
		cursor.beginEditBlock();
		for (size_t i = first; i < count; i++) {
			const struct producer_line *line = producer_chat_at(model_, i);
			if (line->seq > upTo)
				break;
			seen_ = line->seq;
			if (!producer_line_visible(line, &filter))
				continue;
			appendLine(cursor, line);
			shown_.push_back(line->seq);
		}
		cursor.endEditBlock();
	}

	/* lines the ring has let go of leave the top of the view too */
	void trimFront()
	{
		const uint64_t oldest = producer_chat_oldest_seq(model_);
		size_t gone = 0;
		while (gone < shown_.size() && (oldest == 0 || shown_[gone] < oldest))
			gone++;
		if (!gone)
			return;
		if (gone == shown_.size()) {
			chat_->clear();
			shown_.clear();
			return;
		}
		QTextCursor cursor(chat_->document());
		cursor.movePosition(QTextCursor::Start);
		cursor.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor, static_cast<int>(gone));
		cursor.removeSelectedText();
		shown_.erase(shown_.begin(), shown_.begin() + static_cast<std::ptrdiff_t>(gone));
	}

	/* the whole view again, from the ring: after a filter change or a moderator's deletion, never per message */
	void rebuild(const struct producer_filter &filter, uint64_t upTo)
	{
		chat_->clear();
		shown_.clear();
		seen_ = 0;
		appendNew(filter, upTo);
		if (seen_ < upTo && upTo != UINT64_MAX)
			seen_ = qMin(upTo, producer_chat_newest_seq(model_));
	}

	/* Brings the view up to date with the ring. redraw: lines inside the list changed (not just its two ends). */
	void showChat(bool redraw)
	{
		Filter f;
		currentFilter(f);
		QScrollBar *bar = chat_->verticalScrollBar();
		/* scrolled up, and everything it was showing has since left the ring: there is nothing to hold on to */
		if (!following_ && producer_chat_oldest_seq(model_) > seen_)
			following_ = true;
		if (!following_ && producer_chat_count(model_) == 0)
			following_ = true;

		chat_->setUpdatesEnabled(false);
		if (following_) {
			if (redraw) {
				rebuild(f.filter, UINT64_MAX);
			} else {
				trimFront();
				appendNew(f.filter);
			}
			bar->setValue(bar->maximum());
		} else if (redraw) {
			/* stay where the reader is: the same lines, minus what was removed */
			const int value = bar->value();
			rebuild(f.filter, seen_);
			bar->setValue(qMin(value, bar->maximum()));
		}
		chat_->setUpdatesEnabled(true);
		showJump(f.filter);
	}

	/* the scroll bar moved (the user, a resize, or new lines): at the bottom means following */
	void scrolled()
	{
		QScrollBar *bar = chat_->verticalScrollBar();
		const bool atBottom = bar->value() >= bar->maximum();
		if (atBottom == following_)
			return;
		if (atBottom) {
			follow();
		} else {
			following_ = false; /* reading back: new messages wait, counted on the button */
			Filter f;
			currentFilter(f);
			showJump(f.filter);
		}
	}

	/* back to the newest message, with everything that arrived meanwhile */
	void follow()
	{
		following_ = true;
		showChat(false);
	}

	void showJump(const struct producer_filter &filter)
	{
		const size_t waiting = following_ ? 0 : producer_chat_visible_after(model_, seen_, &filter);
		if (following_) {
			jump_->hide();
			return;
		}
		jump_->setText(waiting == 0   ? uiText("Producer.Jump.Latest")
			       : waiting == 1 ? uiText("Producer.Jump.One")
					      : uiText("Producer.Jump.Many").arg(waiting));
		jump_->adjustSize();
		placeJump();
		jump_->show();
		jump_->raise();
	}

	void placeJump()
	{
		const QWidget *area = chat_->viewport();
		jump_->move(qMax(0, area->x() + (area->width() - jump_->width()) / 2),
			    qMax(0, area->y() + area->height() - jump_->height() - 8));
	}

	/* what a list was last built from, held so its address can't be reused by a newer one */
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

	QLabel *total_;
	QLabel *session_;
	QWidget *chipsHost_;
	FlowLayout *chips_;
	QLabel *noDestinations_;
	std::vector<DestChip *> chipWidgets_;
	std::vector<QString> destIds_;

	QFrame *banner_;
	QLabel *bannerText_;
	QPushButton *bannerButton_;
	Banner bannerKind_ = Banner::None;
	QString approveUrl_;

	QWidget *filtersHost_;
	FlowLayout *filters_;
	FilterChip *joins_;
	QHash<QString, FilterChip *> filterChips_;
	QStringList hiddenPlatforms_;
	bool loading_ = false;

	QPlainTextEdit *chat_;
	QPushButton *jump_;
	QTimer *scrollCheck_;
	QTimer *timer_;
	struct producer_chat *model_;
	std::deque<uint64_t> shown_; /* the seq of each line in the view, top to bottom: one text block each */
	uint64_t seen_ = 0;          /* the newest seq the view has been through (shown or filtered out) */
	bool following_ = true;      /* at the bottom: new messages are shown as they come */
	QTextCharFormat text_, strong_, notice_, dim_, marker_;

	Held<obs_data_t, obs_data_addref, obs_data_release> lastStatus_;
	Held<obs_data_array_t, obs_data_array_addref, obs_data_array_release> lastProfiles_;
	QString lastProfileId_;
	uint64_t lastSeq_ = UINT64_MAX;
	uint64_t lastChatSeq_ = UINT64_MAX;
};

QPointer<ProducerDock> producerDock;

/* void ctd_producer_show(): UI thread only. Shows the CTD Producer dock (the CTD Relay dock's button; CTD OBS). */
void procShow(void *, calldata_t *)
{
	if (producerDock)
		producerDock->showDockWindow();
}

} // namespace

extern "C" void ctd_producer_dock_register(void)
{
	auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *dock = new ProducerDock(main);
	if (!obs_frontend_add_dock_by_id(kProducerDockId, obs_module_text("Producer.Title"), dock)) {
		delete dock;
		return;
	}
	producerDock = dock;
	proc_handler_add(obs_get_proc_handler(), "void ctd_producer_show()", procShow, nullptr);
	blog(LOG_INFO, "[CTD] producer: dock ready");
}
