/*
 * The CTD brand in the plugin's Qt UI (the CineTech Digital design system): the Night theme's colours, the three
 * families (Space Mono for everything you read or act on, Space Grotesk for titles, Chakra Petch for display), square
 * cyan primary buttons with a hard magenta offset shadow, and the cyan / ink / magenta stripe motif.
 *
 * ctd::applyTheme() styles one widget and everything inside it (a dock, the servers window); nothing else in OBS is
 * touched. The fonts ship in data/fonts (SIL Open Font License) and load once; without them the system's mono and sans
 * stand in.
 */

#pragma once

#include <obs-module.h>

#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QGraphicsDropShadowEffect>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QString>
#include <QWidget>

#include <cmath>

namespace ctd {

/* Night theme tokens */
inline QColor voidColor()
{
	return QColor(0x04, 0x07, 0x0c);
}
inline QColor surface()
{
	return QColor(0x08, 0x11, 0x1a);
}
inline QColor surfaceRaised()
{
	return QColor(0x0b, 0x18, 0x21);
}
inline QColor line()
{
	return QColor(0x1a, 0x21, 0x27);
}
inline QColor lineStrong()
{
	return QColor(0x22, 0x2b, 0x34);
}
inline QColor ink()
{
	return QColor(0xf2, 0xfb, 0xff);
}
inline QColor inkBody()
{
	return QColor(0xb9, 0xc6, 0xce);
}
inline QColor inkMuted()
{
	return QColor(0x8b, 0xa3, 0xaf);
}
inline QColor cyan()
{
	return QColor(0x39, 0xe7, 0xff);
}
inline QColor cyanDeep()
{
	return QColor(0x0d, 0x2b, 0x33);
}
inline QColor magenta()
{
	return QColor(0xff, 0x00, 0xb8);
}
inline QColor onAccent()
{
	return QColor(0x04, 0x07, 0x0c);
}
/* tally red: only ever beside a word that says what it means (DOWN, PGM, NOT ON SERVER) */
inline QColor tallyRed()
{
	return QColor(0xff, 0x00, 0x00);
}

enum class Face { Mono, Heading, Display };

inline QString family(Face face)
{
	static bool loaded = false;
	static QString mono = "Consolas", heading = "Segoe UI", display = "Segoe UI";
	if (!loaded) {
		loaded = true;
		const char *files[] = {"fonts/SpaceMono-Regular.ttf",    "fonts/SpaceMono-Bold.ttf",
				       "fonts/SpaceGrotesk-Medium.ttf",  "fonts/SpaceGrotesk-Bold.ttf",
				       "fonts/ChakraPetch-SemiBold.ttf", "fonts/ChakraPetch-Bold.ttf"};
		for (const char *file : files) {
			char *path = obs_module_file(file);
			if (!path)
				continue;
			const int id = QFontDatabase::addApplicationFont(QString::fromUtf8(path));
			bfree(path);
			const QStringList families = id >= 0 ? QFontDatabase::applicationFontFamilies(id)
							     : QStringList();
			if (families.isEmpty())
				continue;
			if (families.first().contains("Mono"))
				mono = families.first();
			else if (families.first().contains("Grotesk"))
				heading = families.first();
			else if (families.first().contains("Chakra"))
				display = families.first();
		}
	}
	return face == Face::Mono ? mono : face == Face::Heading ? heading : display;
}

/* A brand font at a pixel size. upper: set in capitals; spacing: extra pixels between letters. */
inline QFont font(Face face, int px, int weight = QFont::Normal, bool upper = false, qreal spacing = 0)
{
	QFont f(family(face));
	f.setPixelSize(px);
	f.setWeight(static_cast<QFont::Weight>(weight));
	if (upper)
		f.setCapitalization(QFont::AllUppercase);
	if (spacing > 0)
		f.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
	f.setStyleHint(face == Face::Mono ? QFont::Monospace : QFont::SansSerif);
	return f;
}

/* The stylesheet: colours, borders and shapes only (fonts are set in code, so capitals and letter spacing hold) */
inline QString styleSheet()
{
	QString chevron;
	if (char *path = obs_module_file("icons/chevron-down.svg")) {
		chevron = QString::fromUtf8(path).replace('\\', '/');
		bfree(path);
	}
	return QString(R"(
#ctdRoot { background: #04070c; }
#ctdRoot QLabel { color: #b9c6ce; background: transparent; }
#ctdRoot QLabel[ctdRole="title"] { color: #f2fbff; }
#ctdRoot QLabel[ctdRole="eyebrow"] { color: #39e7ff; }
#ctdRoot QLabel[ctdRole="muted"] { color: #8ba3af; }

#ctdRoot QPushButton { background: transparent; color: #f2fbff; border: 1px solid #222b34; border-radius: 0px;
	padding: 6px 12px; }
#ctdRoot QPushButton:hover { border-color: #39e7ff; color: #39e7ff; }
#ctdRoot QPushButton:focus { border: 1px solid #39e7ff; }
#ctdRoot QPushButton:pressed { background: #0d2b33; }
#ctdRoot QPushButton:disabled { color: #4a5a63; border-color: #1a2127; }
#ctdRoot QPushButton[ctdRole="primary"] { background: #39e7ff; color: #04070c; border: none; }
#ctdRoot QPushButton[ctdRole="primary"]:hover { background: #7ef0ff; color: #04070c; }
#ctdRoot QPushButton[ctdRole="primary"]:pressed { background: #2bc4da; }
#ctdRoot QPushButton[ctdRole="primary"]:disabled { background: #1a2127; color: #4a5a63; }
#ctdRoot QPushButton[ctdRole="hot"] { background: transparent; color: #ff00b8; border: 1px solid #ff00b8; }
#ctdRoot QPushButton[ctdRole="hot"]:hover { background: #ff00b8; color: #04070c; }

#ctdRoot QLineEdit, #ctdRoot QComboBox, #ctdRoot QSpinBox { background: #0b1821; color: #f2fbff;
	border: 1px solid #222b34; border-radius: 4px; padding: 4px 8px; selection-background-color: #39e7ff;
	selection-color: #04070c; }
#ctdRoot QLineEdit:focus, #ctdRoot QComboBox:focus, #ctdRoot QSpinBox:focus { border: 1px solid #39e7ff; }
#ctdRoot QLineEdit:disabled, #ctdRoot QComboBox:disabled { color: #4a5a63; }
#ctdRoot QComboBox::drop-down { border: none; width: 20px; }
#ctdRoot QComboBox::down-arrow { image: url(%1); width: 10px; height: 6px; }
#ctdRoot QComboBox QAbstractItemView { background: #08111a; color: #f2fbff; border: 1px solid #222b34;
	selection-background-color: #0d2b33; selection-color: #39e7ff; outline: 0; }

#ctdRoot QCheckBox { color: #b9c6ce; spacing: 8px; }
#ctdRoot QCheckBox::indicator { width: 12px; height: 12px; border: 1px solid #222b34; background: #0b1821; }
#ctdRoot QCheckBox::indicator:hover { border-color: #39e7ff; }
#ctdRoot QCheckBox::indicator:checked { background: #39e7ff; border-color: #39e7ff; }

#ctdRoot QTableWidget, #ctdRoot QListWidget, #ctdRoot QTreeWidget, #ctdRoot QTextBrowser {
	background: #08111a; color: #b9c6ce; border: 1px solid #1a2127; border-radius: 4px; gridline-color: #1a2127;
	outline: 0; }
#ctdRoot QTableWidget::item, #ctdRoot QListWidget::item, #ctdRoot QTreeWidget::item { padding: 3px 4px; }
#ctdRoot QTableWidget::item:selected, #ctdRoot QListWidget::item:selected, #ctdRoot QTreeWidget::item:selected {
	background: #0d2b33; color: #39e7ff; }
#ctdRoot QHeaderView::section { background: #04070c; color: #8ba3af; border: none;
	border-bottom: 1px solid #1a2127; padding: 4px 6px; }
#ctdRoot QTableCornerButton::section { background: #04070c; border: none; }

#ctdRoot QGroupBox { border: 1px solid #1a2127; border-radius: 4px; margin-top: 14px; padding: 10px 8px 8px 8px; }
#ctdRoot QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: #39e7ff; }

#ctdRoot QScrollBar:vertical { background: #04070c; width: 8px; margin: 0; }
#ctdRoot QScrollBar:horizontal { background: #04070c; height: 8px; margin: 0; }
#ctdRoot QScrollBar::handle { background: #222b34; min-height: 24px; min-width: 24px; }
#ctdRoot QScrollBar::handle:hover { background: #39e7ff; }
#ctdRoot QScrollBar::add-line, #ctdRoot QScrollBar::sub-line { width: 0; height: 0; }
#ctdRoot QScrollBar::add-page, #ctdRoot QScrollBar::sub-page { background: none; }
)")
		.arg(chevron);
}

/* Styles a dock or window and everything in it */
inline void applyTheme(QWidget *root)
{
	root->setObjectName("ctdRoot");
	root->setAttribute(Qt::WA_StyledBackground, true);
	root->setStyleSheet(styleSheet());
	root->setFont(font(Face::Mono, 12));
}

/* Restyles a style-dependent widget after its ctdRole property changed */
inline void repolish(QWidget *widget)
{
	widget->style()->unpolish(widget);
	widget->style()->polish(widget);
}

enum class Role { Secondary, Primary, Hot };

/* A button in the brand: uppercase mono label. Primary: cyan fill with a hard magenta offset shadow (the brand's
 * depth, never blur). Hot: the magenta outline, for actions that change the server. */
inline QPushButton *button(QPushButton *b, Role role = Role::Secondary)
{
	b->setFont(font(Face::Mono, 11, QFont::Bold, true, 1.2));
	b->setCursor(Qt::PointingHandCursor);
	b->setProperty("ctdRole", role == Role::Primary ? "primary" : role == Role::Hot ? "hot" : "secondary");
	if (role == Role::Primary) {
		auto *shadow = new QGraphicsDropShadowEffect(b);
		shadow->setBlurRadius(0);
		shadow->setOffset(3, 3);
		shadow->setColor(magenta());
		b->setGraphicsEffect(shadow);
	}
	repolish(b);
	return b;
}

/* An eyebrow: the small cyan section label above a list ("ISSUES", "01 // STREAMS") */
inline QLabel *eyebrow(const QString &text, QWidget *parent)
{
	auto *label = new QLabel(text, parent);
	label->setFont(font(Face::Mono, 10, QFont::Normal, true, 2.0));
	label->setProperty("ctdRole", "eyebrow");
	return label;
}

/* A group box titled like an eyebrow */
inline void eyebrowTitle(QWidget *box)
{
	box->setFont(font(Face::Mono, 10, QFont::Normal, true, 2.0));
	for (QWidget *child : box->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
		child->setFont(font(Face::Mono, 12));
}

/* The stripe motif: a wide cyan bar, a thin ink bar and a wide magenta bar, skewed 20 degrees. Once per view. */
class Stripe : public QWidget {
public:
	explicit Stripe(QWidget *parent = nullptr) : QWidget(parent) { setFixedSize(46, 22); }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const qreal h = height(), slant = h * std::tan(20.0 * 3.14159265358979 / 180.0);
		const qreal wide = 12, thin = 3, gap = 2;
		qreal x = 0;
		const struct {
			qreal w;
			QColor c;
		} bars[] = {{wide, cyan()}, {thin, ink()}, {wide, magenta()}};
		for (const auto &bar : bars) {
			QPainterPath path;
			path.moveTo(x, h);
			path.lineTo(x + slant, 0);
			path.lineTo(x + slant + bar.w, 0);
			path.lineTo(x + bar.w, h);
			path.closeSubpath();
			p.fillPath(path, bar.c);
			x += bar.w + gap;
		}
	}
};

} // namespace ctd
