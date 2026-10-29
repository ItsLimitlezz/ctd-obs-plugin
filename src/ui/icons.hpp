/*
 * The CTD icons (data/icons, Gradient Family): a small header for the plugin's docks, and window icons.
 */

#pragma once

#include "brand.hpp"

#include <obs-module.h>

#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QString>
#include <QWidget>

/* "obs-plugin-64.png" from the plugin's data folder; a null pixmap if it is missing */
inline QPixmap ctdPixmap(const char *file)
{
	QString path;
	char *found = obs_module_file(QString("icons/%1").arg(file).toUtf8().constData());
	if (found) {
		path = QString::fromUtf8(found);
		bfree(found);
	}
	return path.isEmpty() ? QPixmap() : QPixmap(path);
}

inline QIcon ctdIcon(const char *file)
{
	const QPixmap pixmap = ctdPixmap(file);
	return pixmap.isNull() ? QIcon() : QIcon(pixmap);
}

/* the icon, the product's name in the heading face (uppercase) and the stripe motif, as the first row of a dock */
inline QHBoxLayout *ctdHeader(QWidget *parent, const char *icon_file, const QString &title)
{
	auto *row = new QHBoxLayout();
	row->setSpacing(8);
	const QPixmap pixmap = ctdPixmap(icon_file);
	if (!pixmap.isNull()) {
		auto *icon = new QLabel(parent);
		const int size = qRound(22 * parent->devicePixelRatio());
		QPixmap scaled = pixmap.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
		scaled.setDevicePixelRatio(parent->devicePixelRatio());
		icon->setPixmap(scaled);
		row->addWidget(icon);
	}
	auto *name = new QLabel(title, parent);
	name->setFont(ctd::font(ctd::Face::Heading, 16, QFont::Bold, true, 0.6));
	name->setProperty("ctdRole", "title");
	row->addWidget(name, 1);
	row->addWidget(new ctd::Stripe(parent), 0, Qt::AlignVCenter);
	return row;
}
