/*
 * Tools ▸ CTD failover (CTD OBS F2): what to do when a feed on program dies. One switch for the whole thing, the
 * fallback scenes in order, and per feed: what counts as dead (no signal, colour bars, a saved picture, frozen) and
 * whether it has a scene of its own to go to. The settings belong to the scene collection; Save writes them into it.
 * The rules themselves are src/obs/failover-core.c.
 */

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../obs/failover.h"
#include "icons.hpp"

extern "C" void ctd_failover_dialog_open(void);

namespace {

/* user-visible text, from data/locale */
QString uiText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

void copyName(char *dst, const QString &text)
{
	snprintf(dst, FO_NAME_MAX, "%s", text.toUtf8().constData());
}

/* every scene of the collection, in OBS's order */
QStringList sceneNames()
{
	QStringList names;
	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		const char *name = obs_source_get_name(scenes.sources.array[i]);
		if (name && *name)
			names << QString::fromUtf8(name);
	}
	obs_frontend_source_list_free(&scenes);
	return names;
}

bool collectFeed(void *param, obs_source_t *source)
{
	auto *names = static_cast<QStringList *>(param);
	const char *id = obs_source_get_unversioned_id(source);
	const char *name = obs_source_get_name(source);
	if (id && strcmp(id, "ctd_live_source") == 0 && name && *name)
		names->append(QString::fromUtf8(name));
	return true;
}

/* every CTD Live Source with video, by name */
QStringList feedNames()
{
	QStringList names;
	obs_enum_sources(collectFeed, &names);
	names.sort(Qt::CaseInsensitive);
	return names;
}

QTableWidgetItem *checkItem(bool on)
{
	auto *item = new QTableWidgetItem();
	item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
	item->setCheckState(on ? Qt::Checked : Qt::Unchecked);
	return item;
}

enum Column { ColFeed, ColWatch, ColSignal, ColBars, ColFrozen, ColPicture, ColTarget, ColCount };

class FailoverDialog : public QDialog {
public:
	explicit FailoverDialog(QWidget *parent) : QDialog(parent), config_(std::make_unique<struct fo_config>())
	{
		setWindowTitle(uiText("Failover.Title"));
		setWindowIcon(ctdIcon("obs-plugin-64.png"));
		ctd::applyTheme(this);
		failover_get_config(config_.get());
		scenes_ = sceneNames();

		auto *outer = new QVBoxLayout(this);
		outer->addLayout(ctdHeader(this, "obs-plugin-64.png", uiText("Failover.Title")));

		enabled_ = new QCheckBox(uiText("Failover.Enabled"), this);
		enabled_->setChecked(config_->enabled);
		outer->addWidget(enabled_);
		auto *intro = new QLabel(uiText("Failover.Intro"), this);
		intro->setWordWrap(true);
		outer->addWidget(intro);

		auto *timing = new QFormLayout();
		delay_ = new QSpinBox(this);
		delay_->setRange(FO_DELAY_MIN_MS / 1000, FO_DELAY_MAX_MS / 1000);
		delay_->setSuffix(uiText("Failover.Seconds"));
		delay_->setValue(config_->delay_ms / 1000);
		timing->addRow(uiText("Failover.Delay"), delay_);
		outer->addLayout(timing);

		/* the fallback list */
		outer->addWidget(ctd::eyebrow(uiText("Failover.Scenes"), this));
		auto *scenesHelp = new QLabel(uiText("Failover.Scenes.Help"), this);
		scenesHelp->setWordWrap(true);
		outer->addWidget(scenesHelp);
		auto *sceneRow = new QHBoxLayout();
		list_ = new QListWidget(this);
		list_->setMinimumHeight(96);
		for (size_t i = 0; i < config_->scene_count; i++)
			list_->addItem(QString::fromUtf8(config_->scenes[i]));
		sceneRow->addWidget(list_, 1);
		auto *sceneButtons = new QVBoxLayout();
		scenePick_ = new QComboBox(this);
		scenePick_->addItems(scenes_);
		scenePick_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		scenePick_->setMinimumContentsLength(14);
		auto *add = new QPushButton(uiText("Failover.Scenes.Add"), this);
		auto *remove = new QPushButton(uiText("Failover.Scenes.Remove"), this);
		auto *up = new QPushButton(uiText("Failover.Scenes.Up"), this);
		auto *down = new QPushButton(uiText("Failover.Scenes.Down"), this);
		for (QPushButton *b : {add, remove, up, down})
			ctd::button(b);
		sceneButtons->addWidget(scenePick_);
		sceneButtons->addWidget(add);
		sceneButtons->addWidget(remove);
		sceneButtons->addWidget(up);
		sceneButtons->addWidget(down);
		sceneButtons->addStretch(1);
		sceneRow->addLayout(sceneButtons);
		outer->addLayout(sceneRow);
		connect(add, &QPushButton::clicked, this, [this] { addScene(); });
		connect(remove, &QPushButton::clicked, this, [this] { delete list_->takeItem(list_->currentRow()); });
		connect(up, &QPushButton::clicked, this, [this] { moveScene(-1); });
		connect(down, &QPushButton::clicked, this, [this] { moveScene(1); });

		/* per feed */
		outer->addWidget(ctd::eyebrow(uiText("Failover.Feeds"), this));
		table_ = new QTableWidget(this);
		table_->setColumnCount(ColCount);
		table_->setHorizontalHeaderLabels({uiText("Failover.Col.Feed"), uiText("Failover.Col.Watch"),
						   uiText("Failover.Col.Signal"), uiText("Failover.Col.Bars"),
						   uiText("Failover.Col.Frozen"), uiText("Failover.Col.Picture"),
						   uiText("Failover.Col.Target")});
		table_->verticalHeader()->setVisible(false);
		table_->setSelectionMode(QAbstractItemView::NoSelection);
		table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
		table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
		table_->horizontalHeader()->setSectionResizeMode(ColFeed, QHeaderView::Stretch);
		table_->setMinimumHeight(140);
		fillFeeds();
		outer->addWidget(table_, 1);
		empty_ = new QLabel(uiText("Failover.Feeds.None"), this);
		empty_->setWordWrap(true);
		empty_->setVisible(rows_.empty());
		outer->addWidget(empty_);

		auto *more = new QFormLayout();
		frozen_ = new QSpinBox(this);
		frozen_->setRange(1, 600);
		frozen_->setSuffix(uiText("Failover.Seconds"));
		frozen_->setValue(config_->frozen_ms / 1000);
		more->addRow(uiText("Failover.FrozenAfter"), frozen_);
		auto *returnRow = new QHBoxLayout();
		return_ = new QCheckBox(uiText("Failover.Return"), this);
		return_->setChecked(config_->return_when_back);
		returnAfter_ = new QSpinBox(this);
		returnAfter_->setRange(0, 600);
		returnAfter_->setSuffix(uiText("Failover.Seconds"));
		returnAfter_->setValue(config_->return_after_ms / 1000);
		returnRow->addWidget(return_);
		returnRow->addWidget(returnAfter_);
		returnRow->addStretch(1);
		more->addRow(returnRow);
		outer->addLayout(more);

		status_ = new QLabel(this);
		status_->setWordWrap(true);
		outer->addWidget(status_);
		showStatus();

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close, this);
		for (QAbstractButton *b : buttons->buttons()) {
			if (auto *push = qobject_cast<QPushButton *>(b))
				ctd::button(push, buttons->standardButton(b) == QDialogButtonBox::Save
							  ? ctd::Role::Primary
							  : ctd::Role::Secondary);
		}
		outer->addWidget(buttons);
		connect(buttons, &QDialogButtonBox::accepted, this, [this] {
			save();
			accept();
		});
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		resize(760, 640);
	}

private:
	struct Row {
		QString source;
		bool hasPicture = false;
		std::array<uint8_t, FO_GRID_BYTES> picture{};
		QPushButton *pictureButton = nullptr;
		QComboBox *target = nullptr;
	};

	void addScene()
	{
		const QString name = scenePick_->currentText();
		if (name.isEmpty() || list_->count() >= FO_MAX_SCENES)
			return;
		for (int i = 0; i < list_->count(); i++) {
			if (list_->item(i)->text() == name)
				return; /* each scene once */
		}
		list_->addItem(name);
		list_->setCurrentRow(list_->count() - 1);
	}

	void moveScene(int by)
	{
		const int row = list_->currentRow();
		const int to = row + by;
		if (row < 0 || to < 0 || to >= list_->count())
			return;
		QListWidgetItem *item = list_->takeItem(row);
		list_->insertItem(to, item);
		list_->setCurrentRow(to);
	}

	void fillFeeds()
	{
		const QStringList feeds = feedNames();
		table_->setRowCount(static_cast<int>(feeds.size()));
		rows_.clear();
		rows_.reserve(static_cast<size_t>(feeds.size()));
		for (int i = 0; i < feeds.size(); i++) {
			const QByteArray feed = feeds[i].toUtf8();
			struct fo_rule rule;
			fo_rule_defaults(&rule, feed.constData());
			/* its own rule, else the defaults; "watch" off only when a rule (or "only listed feeds") says so */
			const bool watched = fo_rule_for(config_.get(), feed.constData(), &rule);
			Row row;
			row.source = feeds[i];
			row.hasPicture = rule.has_reference;
			memcpy(row.picture.data(), rule.reference, FO_GRID_BYTES);

			auto *name = new QTableWidgetItem(feeds[i]);
			name->setFlags(Qt::ItemIsEnabled);
			table_->setItem(i, ColFeed, name);
			table_->setItem(i, ColWatch, checkItem(watched));
			table_->setItem(i, ColSignal, checkItem(rule.on_signal_lost));
			table_->setItem(i, ColBars, checkItem(rule.on_bars));
			table_->setItem(i, ColFrozen, checkItem(rule.on_frozen));

			row.pictureButton = new QPushButton(table_);
			ctd::button(row.pictureButton);
			table_->setCellWidget(i, ColPicture, row.pictureButton);
			connect(row.pictureButton, &QPushButton::clicked, this, [this, i] { togglePicture(i); });

			row.target = new QComboBox(table_);
			row.target->addItem(uiText("Failover.Target.List"), QString());
			for (const QString &scene : scenes_)
				row.target->addItem(scene, scene);
			const QString target = QString::fromUtf8(rule.target);
			int index = row.target->findData(target);
			if (index < 0 && !target.isEmpty()) {
				/* a scene that no longer exists: kept as it is, so nothing is lost by opening this window */
				row.target->addItem(target, target);
				index = row.target->count() - 1;
			}
			row.target->setCurrentIndex(index < 0 ? 0 : index);
			table_->setCellWidget(i, ColTarget, row.target);

			rows_.push_back(row);
			labelPicture(i);
		}
	}

	void labelPicture(int index)
	{
		const Row &row = rows_[static_cast<size_t>(index)];
		row.pictureButton->setText(uiText(row.hasPicture ? "Failover.Picture.Clear" : "Failover.Picture.Save"));
		row.pictureButton->setToolTip(uiText("Failover.Picture.Tip"));
	}

	void togglePicture(int index)
	{
		Row &row = rows_[static_cast<size_t>(index)];
		if (row.hasPicture) {
			row.hasPicture = false;
		} else if (failover_capture_picture(row.source.toUtf8().constData(), row.picture.data())) {
			row.hasPicture = true;
		} else {
			status_->setText(uiText("Failover.Picture.None").arg(row.source));
			return;
		}
		labelPicture(index);
	}

	void showStatus()
	{
		struct failover_status status;
		failover_get_status(&status);
		if (!*status.last) {
			status_->setText(uiText("Failover.Status.None"));
			return;
		}
		const QString at = QDateTime::fromMSecsSinceEpoch(status.last_at_ms).toString("HH:mm:ss");
		status_->setText(uiText("Failover.Status.Last").arg(at, QString::fromUtf8(status.last)));
	}

	bool checked(int row, int column) const
	{
		const QTableWidgetItem *item = table_->item(row, column);
		return item && item->checkState() == Qt::Checked;
	}

	void save()
	{
		struct fo_config *c = config_.get();
		c->enabled = enabled_->isChecked();
		c->delay_ms = delay_->value() * 1000;
		c->frozen_ms = frozen_->value() * 1000;
		c->return_when_back = return_->isChecked();
		c->return_after_ms = returnAfter_->value() * 1000;
		c->scene_count = 0;
		for (int i = 0; i < list_->count() && c->scene_count < FO_MAX_SCENES; i++)
			copyName(c->scenes[c->scene_count++], list_->item(i)->text());

		/* the feeds listed here get the rule shown; rules for feeds that aren't in this collection right now
		 * (a source removed for tonight) are kept as they were */
		std::vector<struct fo_rule> kept;
		for (size_t i = 0; i < c->rule_count; i++) {
			bool listed = false;
			for (const Row &row : rows_)
				listed = listed || row.source == QString::fromUtf8(c->rules[i].source);
			if (!listed)
				kept.push_back(c->rules[i]);
		}
		c->rule_count = 0;
		for (size_t i = 0; i < rows_.size() && c->rule_count < FO_MAX_RULES; i++) {
			const Row &row = rows_[i];
			const int r = static_cast<int>(i);
			struct fo_rule *rule = &c->rules[c->rule_count++];
			fo_rule_defaults(rule, row.source.toUtf8().constData());
			rule->watch = checked(r, ColWatch);
			rule->on_signal_lost = checked(r, ColSignal);
			rule->on_bars = checked(r, ColBars);
			rule->on_frozen = checked(r, ColFrozen);
			rule->has_reference = row.hasPicture;
			memcpy(rule->reference, row.picture.data(), FO_GRID_BYTES);
			copyName(rule->target, row.target->currentData().toString());
		}
		for (size_t i = 0; i < kept.size() && c->rule_count < FO_MAX_RULES; i++)
			c->rules[c->rule_count++] = kept[i];

		failover_set_config(c);
		obs_frontend_save(); /* into the scene collection's file */
	}

	std::unique_ptr<struct fo_config> config_; /* some tens of kilobytes: not on the stack */
	QStringList scenes_;
	std::vector<Row> rows_;
	QCheckBox *enabled_ = nullptr;
	QSpinBox *delay_ = nullptr;
	QListWidget *list_ = nullptr;
	QComboBox *scenePick_ = nullptr;
	QTableWidget *table_ = nullptr;
	QLabel *empty_ = nullptr;
	QSpinBox *frozen_ = nullptr;
	QCheckBox *return_ = nullptr;
	QSpinBox *returnAfter_ = nullptr;
	QLabel *status_ = nullptr;
};

void openFromMenu(void *)
{
	ctd_failover_dialog_open();
}

/* void ctd_failover_dialog_open(): for CTD OBS's CTD menu. UI thread only. */
void procOpen(void *, calldata_t *)
{
	ctd_failover_dialog_open();
}

} // namespace

extern "C" void ctd_failover_dialog_open(void)
{
	FailoverDialog dialog(static_cast<QWidget *>(obs_frontend_get_main_window()));
	dialog.exec();
}

extern "C" void ctd_failover_dialog_register(void)
{
	proc_handler_add(obs_get_proc_handler(), "void ctd_failover_dialog_open()", procOpen, nullptr);
	/* Tools has it until CTD OBS's own CTD menu does (it opens the window through the proc above) */
	obs_frontend_add_tools_menu_item(obs_module_text("Failover.MenuItem"), openFromMenu, nullptr);
}
