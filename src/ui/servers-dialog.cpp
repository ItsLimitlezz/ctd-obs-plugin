/*
 * Feed servers (CTD Relay dock ▸ Feed servers…, Tools ▸ CTD Relay feed servers…; SPEC 5.1, H1 to H4): add, edit, test and remove the MediaMTX server profiles the sources
 * read from. Saving a profile reconnects every source that uses it. The secrets are typed here and stored in the
 * plugin's own config; they are never shown again in clear or logged.
 */

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "../api/mediamtx-api.h"
#include "../obs/servers.h"
#include "icons.hpp"

namespace {

/* user-visible text, from data/locale */
QString uiText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString qs(const char *value)
{
	return QString::fromUtf8(value ? value : "");
}

std::string utf8(const QLineEdit *edit)
{
	return edit->text().trimmed().toStdString();
}

class ServersDialog : public QDialog {
public:
	explicit ServersDialog(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle(uiText("Servers.Title"));
		setWindowIcon(ctdIcon("obs-plugin-64.png"));
		ctd::applyTheme(this);
		auto *outer = new QVBoxLayout(this);
		outer->addLayout(ctdHeader(this, "obs-plugin-64.png", uiText("Servers.Title")));
		auto *layout = new QHBoxLayout();
		outer->addLayout(layout, 1);

		auto *left = new QVBoxLayout();
		left->addWidget(ctd::eyebrow(uiText("Servers.Profiles"), this));
		list_ = new QListWidget(this);
		list_->setMinimumWidth(220);
		left->addWidget(list_, 1);
		auto *add = new QPushButton(uiText("Servers.Add"), this);
		auto *remove = new QPushButton(uiText("Servers.Remove"), this);
		ctd::button(add);
		ctd::button(remove, ctd::Role::Hot);
		auto *listButtons = new QHBoxLayout();
		listButtons->addWidget(add);
		listButtons->addWidget(remove);
		left->addLayout(listButtons);
		layout->addLayout(left);

		auto *right = new QVBoxLayout();
		right->addWidget(ctd::eyebrow(uiText("Servers.Server"), this));
		auto *form = new QFormLayout();
		name_ = new QLineEdit(this);
		host_ = new QLineEdit(this);
		host_->setPlaceholderText("nas.local");
		srtPort_ = new QSpinBox(this);
		srtPort_->setRange(1, 65535);
		apiUrl_ = new QLineEdit(this);
		apiUrl_->setPlaceholderText(uiText("Servers.ApiUrl.Placeholder"));
		apiUser_ = new QLineEdit(this);
		apiPass_ = secret();
		apiToken_ = secret();
		readUser_ = new QLineEdit(this);
		readPass_ = secret();
		passphrase_ = secret();
		webhook_ = secret(); /* it may carry a token */
		webhook_->setPlaceholderText(uiText("Servers.Webhook.Placeholder"));
		default_ = new QCheckBox(uiText("Servers.Default"), this);
		writes_ = new QCheckBox(uiText("Servers.AllowWrites"), this);
		form->addRow(uiText("Servers.Name"), name_);
		form->addRow(uiText("Servers.Address"), host_);
		form->addRow(uiText("Servers.SrtPort"), srtPort_);
		form->addRow(uiText("Servers.ApiUrl"), apiUrl_);
		form->addRow(uiText("Servers.ApiUser"), apiUser_);
		form->addRow(uiText("Servers.ApiPass"), apiPass_);
		form->addRow(uiText("Servers.ApiToken"), apiToken_);
		form->addRow(uiText("Servers.ReadUser"), readUser_);
		form->addRow(uiText("Servers.ReadPass"), readPass_);
		form->addRow(uiText("Servers.Passphrase"), passphrase_);
		form->addRow(uiText("Servers.Webhook"), webhook_);
		form->addRow("", default_);
		form->addRow("", writes_);
		right->addLayout(form);
		testResult_ = new QLabel(this);
		testResult_->setWordWrap(true);
		right->addWidget(testResult_);
		auto *actions = new QHBoxLayout();
		auto *test = new QPushButton(uiText("Servers.Test"), this);
		auto *save = new QPushButton(uiText("Servers.Save"), this);
		save->setDefault(true);
		ctd::button(test);
		ctd::button(save, ctd::Role::Primary);
		actions->addWidget(test);
		actions->addStretch(1);
		actions->addWidget(save);
		right->addLayout(actions);
		auto *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
		for (QAbstractButton *b : close->buttons())
			if (auto *push = qobject_cast<QPushButton *>(b))
				ctd::button(push);
		right->addWidget(close);
		layout->addLayout(right, 1);

		connect(list_, &QListWidget::currentRowChanged, this, [this](int row) { showProfile(row); });
		connect(add, &QPushButton::clicked, this, [this] { addProfile(); });
		connect(remove, &QPushButton::clicked, this, [this] { removeProfile(); });
		connect(save, &QPushButton::clicked, this, [this] { saveProfile(); });
		connect(test, &QPushButton::clicked, this, [this] { testProfile(); });
		connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);

		reload("");
	}

private:
	QLineEdit *secret()
	{
		auto *edit = new QLineEdit(this);
		edit->setEchoMode(QLineEdit::Password);
		return edit;
	}

	/* the list from the store, selecting id (or the first) */
	void reload(const QString &id)
	{
		list_->blockSignals(true);
		list_->clear();
		ids_.clear();
		int select = 0;
		for (size_t i = 0; i < servers_count(); i++) {
			struct ctd_server s;
			if (!servers_get_index(i, &s))
				continue;
			list_->addItem(QString("%1 (%2)%3")
					       .arg(qs(s.name), qs(s.host),
						    s.is_default ? " · " + uiText("Servers.DefaultTag") : QString()));
			ids_.push_back(qs(s.id));
			if (qs(s.id) == id)
				select = (int)ids_.size() - 1;
			ctd_server_free(&s);
		}
		list_->blockSignals(false);
		if (!ids_.empty()) {
			list_->setCurrentRow(select);
			showProfile(select);
		} else {
			showProfile(-1);
		}
	}

	void showProfile(int row)
	{
		testResult_->clear();
		editingId_.clear();
		struct ctd_server s = {};
		const bool found = row >= 0 && row < (int)ids_.size() &&
				   servers_get(ids_[(size_t)row].toUtf8().constData(), &s);
		if (found)
			editingId_ = qs(s.id);
		name_->setText(found ? qs(s.name) : uiText("Servers.NewServer"));
		host_->setText(found ? qs(s.host) : "");
		srtPort_->setValue(found ? s.srt_port : 8890);
		apiUrl_->setText(found ? qs(s.api_url) : "");
		apiUser_->setText(found ? qs(s.api_user) : "");
		apiPass_->setText(found ? qs(s.api_pass) : "");
		apiToken_->setText(found ? qs(s.api_token) : "");
		readUser_->setText(found ? qs(s.read_user) : "");
		readPass_->setText(found ? qs(s.read_pass) : "");
		passphrase_->setText(found ? qs(s.passphrase) : "");
		webhook_->setText(found ? qs(s.webhook_url) : "");
		default_->setChecked(found && s.is_default);
		writes_->setChecked(found && s.allow_writes);
		ctd_server_free(&s);
	}

	void addProfile()
	{
		list_->clearSelection();
		showProfile(-1);
		host_->setFocus();
	}

	void removeProfile()
	{
		if (editingId_.isEmpty())
			return;
		if (QMessageBox::question(this, uiText("Common.DialogTitle"),
					  uiText("Servers.Remove.Question").arg(name_->text())) != QMessageBox::Yes)
			return;
		servers_remove(editingId_.toUtf8().constData());
		reload("");
	}

	void saveProfile()
	{
		const std::string host = utf8(host_);
		if (host.empty()) {
			QMessageBox::information(this, uiText("Common.DialogTitle"), uiText("Servers.NeedHost"));
			return;
		}
		const std::string passphrase = utf8(passphrase_);
		if (!passphrase.empty() && (passphrase.size() < 10 || passphrase.size() > 79)) {
			QMessageBox::information(this, uiText("Common.DialogTitle"),
						 uiText("Servers.PassphraseLength"));
			return;
		}
		const std::string name = utf8(name_), apiUrl = utf8(apiUrl_), apiUser = utf8(apiUser_);
		const std::string apiPass = apiPass_->text().toStdString(), apiToken = utf8(apiToken_);
		const std::string readUser = utf8(readUser_), readPass = readPass_->text().toStdString();
		const std::string webhook = utf8(webhook_);
		if (!webhook.empty() && webhook.rfind("http://", 0) != 0 && webhook.rfind("https://", 0) != 0) {
			QMessageBox::information(this, uiText("Common.DialogTitle"), uiText("Servers.WebhookScheme"));
			return;
		}

		struct ctd_server s = {};
		snprintf(s.id, sizeof(s.id), "%s", editingId_.toUtf8().constData());
		s.name = const_cast<char *>(name.empty() ? host.c_str() : name.c_str());
		s.host = const_cast<char *>(host.c_str());
		s.srt_port = srtPort_->value();
		s.api_url = const_cast<char *>(apiUrl.c_str());
		s.api_user = const_cast<char *>(apiUser.c_str());
		s.api_pass = const_cast<char *>(apiPass.c_str());
		s.api_token = const_cast<char *>(apiToken.c_str());
		s.read_user = const_cast<char *>(readUser.c_str());
		s.read_pass = const_cast<char *>(readPass.c_str());
		s.passphrase = const_cast<char *>(passphrase.c_str());
		s.webhook_url = const_cast<char *>(webhook.c_str());
		s.is_default = default_->isChecked() || servers_count() == 0;
		s.allow_writes = writes_->isChecked();
		char id[48] = "";
		servers_put(&s, id, sizeof(id)); /* copies; sources on it reconnect */
		reload(QString::fromUtf8(id));
		testResult_->setText(uiText("Servers.Saved"));
	}

	/* H3: asks the profile's API for its streams, off the UI thread */
	void testProfile()
	{
		struct ctd_server s = {};
		const std::string apiUrl = utf8(apiUrl_);
		s.api_url = const_cast<char *>(apiUrl.c_str());
		char host[256];
		int port = 0;
		if (!ctd_server_api_address(&s, host, sizeof(host), &port)) {
			testResult_->setText(uiText("Servers.Test.NoApi"));
			return;
		}
		testResult_->setText(uiText("Servers.Testing"));
		QPointer<QLabel> result = testResult_;
		std::thread([result, apiHost = std::string(host), port, user = utf8(apiUser_),
			     pass = apiPass_->text().toStdString(), token = utf8(apiToken_)] {
			const struct mtx_api_server server = {apiHost.c_str(), port,    user.c_str(), pass.c_str(),
							      nullptr,         nullptr, token.c_str()};
			struct mtx_path_list list = {};
			const enum mtx_api_status status = mtx_api_list_paths(&server, &list);
			size_t live = 0;
			for (size_t i = 0; i < list.count; i++)
				live += list.items[i].live;
			QString text;
			switch (status) {
			case MTX_API_OK:
				text = uiText("Servers.Test.Ok").arg(list.count).arg(live);
				break;
			case MTX_API_UNAUTHORIZED:
				text = uiText("Servers.Test.Unauthorized");
				break;
			case MTX_API_NOT_FOUND:
				text = uiText("Servers.Test.NotFound");
				break;
			default:
				text = uiText("Servers.Test.Unreachable");
				break;
			}
			mtx_path_list_free(&list);
			QMetaObject::invokeMethod(
				qApp,
				[result, text] {
					if (result)
						result->setText(text);
				},
				Qt::QueuedConnection);
		}).detach();
	}

	QListWidget *list_;
	std::vector<QString> ids_;
	QString editingId_;
	QLineEdit *name_, *host_, *apiUrl_, *apiUser_, *apiPass_, *apiToken_, *readUser_, *readPass_, *passphrase_,
		*webhook_;
	QSpinBox *srtPort_;
	QCheckBox *default_;
	QCheckBox *writes_;
	QLabel *testResult_;
};

void openFromMenu(void *)
{
	ctd_servers_dialog_open();
}

} // namespace

extern "C" void ctd_servers_dialog_open(void)
{
	ServersDialog dialog(static_cast<QWidget *>(obs_frontend_get_main_window()));
	dialog.exec();
}

/* void ctd_servers_dialog_open(): CTD OBS's CTD menu opens the window through this. UI thread only. */
static void procOpen(void *, calldata_t *)
{
	ctd_servers_dialog_open();
}

extern "C" void ctd_servers_dialog_register(void)
{
	proc_handler_t *ph = obs_get_proc_handler();
	proc_handler_add(ph, "void ctd_servers_dialog_open()", procOpen, nullptr);

	/* CTD OBS registers ctd_menu_available before modules load: its CTD menu has this window, so Tools doesn't */
	calldata_t cd = {0};
	const bool ctdMenu = proc_handler_call(ph, "ctd_menu_available", &cd) && calldata_bool(&cd, "available");
	calldata_free(&cd);
	if (!ctdMenu)
		obs_frontend_add_tools_menu_item(obs_module_text("Servers.MenuItem"), openFromMenu, nullptr);
}
