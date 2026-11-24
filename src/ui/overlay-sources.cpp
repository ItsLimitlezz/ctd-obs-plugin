/* See overlay-sources.hpp. Moved here from the CTD Relay dock, unchanged in behaviour, so the CTD Overlays dock adds
 * overlays exactly the same way. */
#include "overlay-sources.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QComboBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QLabel>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "../overlay-scenes.h"
#include "brand.hpp"

namespace ctd::overlays {

namespace {

QString uiText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString str(obs_data_t *data, const char *key)
{
	return QString::fromUtf8(data ? obs_data_get_string(data, key) : "");
}

/* a malloc'd C string as a std::string (empty for NULL), freed */
std::string taken(char *s)
{
	std::string out = s ? s : "";
	free(s);
	return out;
}

enum ctd_address cAddress(Address a)
{
	return a == Address::Public ? CTD_ADDRESS_PUBLIC : CTD_ADDRESS_LOCAL;
}

constexpr int kScanMs = 3000;

} // namespace

QString canvas(obs_data_t *overlay)
{
	obs_data_t *meta = obs_data_get_obj(overlay, "meta");
	QString canvas = str(meta, "canvas");
	obs_data_release(meta);
	if (canvas.isEmpty())
		canvas = obs_data_get_int(overlay, "height") > obs_data_get_int(overlay, "width") ? "vertical"
												  : "landscape";
	return canvas;
}

QString slot(obs_data_t *overlay)
{
	obs_data_t *meta = obs_data_get_obj(overlay, "meta");
	const QString slot = str(meta, "slot");
	obs_data_release(meta);
	return slot;
}

std::vector<Scene> scenes(obs_data_t *overlay)
{
	std::vector<Scene> out;
	obs_data_t *meta = obs_data_get_obj(overlay, "meta");
	obs_data_array_t *list = meta ? obs_data_get_array(meta, "scenes") : nullptr;
	const size_t count = list ? obs_data_array_count(list) : 0;
	for (size_t i = 0; i < count; i++) {
		obs_data_t *scene = obs_data_array_item(list, i);
		const QString id = str(scene, "id");
		const QString name = str(scene, "name");
		if (!id.isEmpty())
			out.push_back({id, name.isEmpty() ? id : name, obs_data_get_string(scene, "url")});
		obs_data_release(scene);
	}
	obs_data_array_release(list);
	obs_data_release(meta);
	return out;
}

QString sceneName(obs_data_t *overlay, const QString &id)
{
	for (const Scene &scene : scenes(overlay))
		if (scene.id == id)
			return scene.name;
	return id;
}

std::string sceneUrl(obs_data_t *overlay, const QString &scene)
{
	if (!scene.isEmpty())
		for (const Scene &s : scenes(overlay))
			if (s.id == scene && !s.url.empty())
				return s.url;
	const QByteArray id = scene.toUtf8();
	char *url = ctd_overlay_scene_url(obs_data_get_string(overlay, "url"), id.isEmpty() ? nullptr : id.constData());
	std::string out = url ? url : obs_data_get_string(overlay, "url");
	free(url);
	return out;
}

ctd_overlay_origins origins(obs_data_t *overlay)
{
	ctd_overlay_origins o;
	obs_data_t *meta = overlay ? obs_data_get_obj(overlay, "meta") : nullptr;
	obs_data_t *addresses = meta ? obs_data_get_obj(meta, "addresses") : nullptr;
	ctd_overlay_origins_read(&o, overlay ? obs_data_get_string(overlay, "url") : nullptr, addresses != nullptr,
				 addresses ? obs_data_get_string(addresses, "local") : nullptr,
				 addresses ? obs_data_get_string(addresses, "public") : nullptr,
				 addresses ? obs_data_get_string(addresses, "default") : nullptr);
	obs_data_release(addresses);
	obs_data_release(meta);
	return o;
}

bool hasAddress(obs_data_t *overlay, Address address)
{
	const ctd_overlay_origins o = origins(overlay);
	return ctd_overlay_has_address(&o, cAddress(address));
}

std::string sceneUrl(obs_data_t *overlay, const QString &scene, Address address)
{
	const ctd_overlay_origins o = origins(overlay);
	return taken(ctd_overlay_url_for(&o, sceneUrl(overlay, scene).c_str(), cAddress(address)));
}

/* ---- the address chosen per CTD Relay profile: overlay-address.json in the module's config ---- */

namespace {

struct SavedAddresses {
	bool loaded = false;
	std::map<std::string, Address> byProfile;
	uint64_t seq = 0;
};

SavedAddresses &saved()
{
	static SavedAddresses s;
	return s;
}

std::string addressFile()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *file = obs_module_config_path("overlay-address.json");
	std::string out = file ? file : "";
	bfree(file);
	return out;
}

void loadSaved()
{
	SavedAddresses &s = saved();
	if (s.loaded)
		return;
	s.loaded = true;
	const std::string file = addressFile();
	obs_data_t *data = file.empty() ? nullptr : obs_data_create_from_json_file_safe(file.c_str(), "bak");
	obs_data_t *profiles = data ? obs_data_get_obj(data, "profiles") : nullptr;
	for (obs_data_item_t *item = profiles ? obs_data_first(profiles) : nullptr; item; obs_data_item_next(&item)) {
		const char *value = obs_data_item_get_string(item);
		if (value && strcmp(value, "public") == 0)
			s.byProfile[obs_data_item_get_name(item)] = Address::Public;
	}
	obs_data_release(profiles);
	obs_data_release(data);
}

} // namespace

Address savedAddress(const QString &relayProfile)
{
	loadSaved();
	auto it = saved().byProfile.find(relayProfile.toStdString());
	return it == saved().byProfile.end() ? Address::Local : it->second;
}

void saveAddress(const QString &relayProfile, Address address)
{
	loadSaved();
	SavedAddresses &s = saved();
	const std::string key = relayProfile.toStdString();
	if (savedAddress(relayProfile) == address)
		return;
	if (address == Address::Local)
		s.byProfile.erase(key); /* the default */
	else
		s.byProfile[key] = address;
	s.seq++;
	obs_data_t *data = obs_data_create();
	obs_data_t *profiles = obs_data_create();
	for (const auto &[profile, a] : s.byProfile)
		if (!profile.empty()) /* before a CTD Relay profile is picked: this session only */
			obs_data_set_string(profiles, profile.c_str(), a == Address::Public ? "public" : "local");
	obs_data_set_obj(data, "profiles", profiles);
	const std::string file = addressFile();
	if (file.empty() || !obs_data_save_json_safe(data, file.c_str(), "tmp", "bak"))
		blog(LOG_WARNING, "[CTD] overlays: couldn't save the overlay address choice");
	obs_data_release(profiles);
	obs_data_release(data);
}

uint64_t savedAddressSeq()
{
	loadSaved();
	return saved().seq;
}

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

bool setSourceSettings(obs_source_t *source, obs_data_t *overlay, const QString &scene, Address address)
{
	std::string url = sceneUrl(overlay, scene, address);
	const bool atAddress = !url.empty();
	if (!atAddress)
		url = sceneUrl(overlay, scene);
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "url", url.c_str());
	obs_data_set_int(settings, "width", obs_data_get_int(overlay, "width"));
	obs_data_set_int(settings, "height", obs_data_get_int(overlay, "height"));
	/* obs-browser's "Shut down source when not visible": an overlay's browser (a CEF renderer) runs only while its
	 * scene shows; otherwise every overlay of every scene keeps a browser running and using memory */
	obs_data_set_bool(settings, "shutdown", true);
	obs_source_update(source, settings);
	obs_data_release(settings);

	obs_data_t *priv = obs_source_get_private_settings(source);
	obs_data_set_string(priv, kSlot, slot(overlay).toUtf8().constData());
	obs_data_set_string(priv, kCanvas, canvas(overlay).toUtf8().constData());
	obs_data_set_string(priv, kName, obs_data_get_string(overlay, "name"));
	obs_data_set_string(priv, kApp, obs_data_get_string(overlay, "appId"));
	obs_data_set_string(priv, kScene, scene.toUtf8().constData());
	obs_data_set_string(priv, kResource, obs_data_get_string(overlay, "id"));
	obs_data_release(priv);
	return atAddress;
}

int shutdownHiddenOverlays()
{
	/* refs taken in the enum, updated after it (obs_enum_sources holds libobs's sources lock) */
	std::vector<obs_source_t *> found;
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			const char *id = obs_source_get_unversioned_id(source);
			if (!id || strcmp(id, "browser_source") != 0)
				return true;
			obs_data_t *priv = obs_source_get_private_settings(source);
			const bool ours = *obs_data_get_string(priv, kResource) || *obs_data_get_string(priv, kSlot);
			obs_data_release(priv);
			if (!ours)
				return true;
			obs_data_t *settings = obs_source_get_settings(source);
			const bool off = !obs_data_get_bool(settings, "shutdown");
			obs_data_release(settings);
			if (off) {
				obs_source_t *ref = obs_source_get_ref(source);
				if (ref)
					static_cast<std::vector<obs_source_t *> *>(param)->push_back(ref);
			}
			return true;
		},
		&found);
	for (obs_source_t *source : found) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_bool(settings, "shutdown", true);
		obs_source_update(source, settings);
		obs_data_release(settings);
		obs_source_release(source);
	}
	if (!found.empty())
		blog(LOG_INFO, "[CTD] overlays: %zu overlay browser source(s) now shut down when not visible",
		     found.size());
	return (int)found.size();
}

std::string addSource(obs_scene_t *scene, obs_data_t *overlay, const QString &sceneId, Address address)
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
	setSourceSettings(source, overlay, sceneId, address);
	/* top-left at 1:1, so an overlay the canvas's size covers it exactly. Left to OBS, a browser source that hasn't
	 * loaded yet is 0×0 and gets centred, leaving most of it off the canvas. */
	obs_sceneitem_t *item = obs_scene_add(scene, source);
	if (item) {
		const vec2 origin = {0.0f, 0.0f};
		obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
		obs_sceneitem_set_pos(item, &origin);
	}
	obs_source_release(source);
	return name;
}

AddResult addToCurrentScene(obs_data_t *overlay, const QString &sceneId, Address address)
{
	AddResult result;
	if (!hasAddress(overlay, address)) {
		result.noAddress = true;
		return result;
	}
	const bool vertical = canvas(overlay) == "vertical";
	obs_source_t *sceneSource = nullptr;
	result.where = uiText("Relay.Overlay.CurrentScene");
	if (vertical) {
		obs_canvas_t *found = findVerticalCanvas();
		if (found) {
			sceneSource = canvasCurrentScene(found);
			result.where =
				uiText("Relay.Overlay.CanvasScene").arg(QString::fromUtf8(obs_canvas_get_name(found)));
			obs_canvas_release(found);
		}
		if (!sceneSource) {
			result.noVertical = true;
			result.where = uiText("Relay.Overlay.MainScene");
		}
	}
	if (!sceneSource)
		sceneSource = obs_frontend_get_current_scene();
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene) {
		obs_source_release(sceneSource);
		return result;
	}
	result.name = addSource(scene, overlay, sceneId, address);
	if (!result.name.empty())
		blog(LOG_INFO, "[CTD] relay: added overlay '%s'%s%s to %s, at its %s address", result.name.c_str(),
		     sceneId.isEmpty() ? "" : ", scene ", sceneId.toUtf8().constData(),
		     result.where.toUtf8().constData(), address == Address::Public ? "public" : "local network");
	obs_source_release(sceneSource);
	return result;
}

/* ---- which address the collection's sources use, and the switch ---- */

namespace {

/* one overlay resource, ready for matching */
struct Known {
	obs_data_t *overlay; /* not held: the caller's array holds it */
	ctd_overlay_origins origins;
	std::string url;
};

std::vector<Known> knownOverlays(obs_data_array_t *overlays)
{
	std::vector<Known> out;
	const size_t count = overlays ? obs_data_array_count(overlays) : 0;
	for (size_t i = 0; i < count; i++) {
		obs_data_t *o = obs_data_array_item(overlays, i);
		Known k{o, origins(o), obs_data_get_string(o, "url")};
		obs_data_release(o); /* the array keeps it */
		if (!k.url.empty())
			out.push_back(std::move(k));
	}
	return out;
}

/* every browser source of the collection: a ref and its URL, copied out while obs_enum_sources holds the sources
 * lock (the graphics thread takes it every frame); the matching comes after */
struct Browser {
	obs_source_t *source; /* a strong reference */
	std::string url;
};

std::vector<Browser> browserSources()
{
	std::vector<Browser> out;
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			const char *id = obs_source_get_unversioned_id(source);
			if (!id || strcmp(id, "browser_source") != 0)
				return true;
			obs_source_t *ref = obs_source_get_ref(source);
			if (!ref)
				return true;
			obs_data_t *settings = obs_source_get_settings(source);
			static_cast<std::vector<Browser> *>(param)->push_back(
				{ref, obs_data_get_string(settings, "url")});
			obs_data_release(settings);
			return true;
		},
		&out);
	return out;
}

void release(std::vector<Browser> &browsers)
{
	for (Browser &b : browsers)
		obs_source_release(b.source);
	browsers.clear();
}

} // namespace

AddressUse countAddressUse(obs_data_array_t *overlays)
{
	AddressUse use;
	const std::vector<Known> known = knownOverlays(overlays);
	if (known.empty())
		return use;
	std::vector<Browser> browsers = browserSources();
	for (const Browser &b : browsers) {
		for (const Known &k : known) {
			/* this overlay's URL at one of its origins (an ALREADY or UNAVAILABLE says it is) */
			if (ctd_overlay_switch_url(&k.origins, k.url.c_str(), b.url.c_str(), CTD_ADDRESS_LOCAL,
						   nullptr) == CTD_SWITCH_NOT_OURS)
				continue;
			switch (ctd_overlay_address_of(&k.origins, b.url.c_str())) {
			case CTD_ADDRESS_LOCAL:
				use.local++;
				break;
			case CTD_ADDRESS_PUBLIC:
				use.pub++;
				break;
			default:
				use.unknown++;
				break;
			}
			break;
		}
	}
	release(browsers);
	return use;
}

SwitchResult switchAddress(obs_data_array_t *overlays, Address to)
{
	SwitchResult result;
	const std::vector<Known> known = knownOverlays(overlays);
	std::vector<Browser> browsers = browserSources();
	for (const Browser &b : browsers) {
		for (const Known &k : known) {
			char *url = nullptr;
			const enum ctd_switch r =
				ctd_overlay_switch_url(&k.origins, k.url.c_str(), b.url.c_str(), cAddress(to), &url);
			if (r == CTD_SWITCH_NOT_OURS)
				continue;
			if (r == CTD_SWITCH_CHANGED && url) {
				/* only the URL: its size, scene and marks stay as they are */
				obs_data_t *settings = obs_data_create();
				obs_data_set_string(settings, "url", url);
				obs_source_update(b.source, settings);
				obs_data_release(settings);
				result.changed++;
			} else if (r == CTD_SWITCH_ALREADY) {
				result.already++;
			} else if (r == CTD_SWITCH_UNAVAILABLE) {
				result.unavailable++;
			}
			free(url);
			break;
		}
	}
	release(browsers);
	/* counts only: the URLs hold the overlays' keys */
	blog(LOG_INFO,
	     "[CTD] overlays: switched %d overlay source(s) to the %s address (%d already were, %d not known there)",
	     result.changed, to == Address::Public ? "public" : "local network", result.already, result.unavailable);
	return result;
}

/* ---- the Address row ---- */

AddressPicker::AddressPicker(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(6);
	auto *row = new QHBoxLayout();
	row->setSpacing(6);
	auto *label = new QLabel(uiText("Overlays.Address"), this);
	choice_ = new QComboBox(this);
	choice_->addItem(uiText("Overlays.Address.Local"));
	choice_->addItem(uiText("Overlays.Address.Public"));
	choice_->setToolTip(uiText("Overlays.Address.Tip"));
	choice_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	choice_->setMinimumContentsLength(8);
	label->setBuddy(choice_);
	row->addWidget(label);
	row->addWidget(choice_, 1);
	layout->addLayout(row);
	hint_ = new QLabel(uiText("Overlays.Address.NoPublic"), this);
	hint_->setWordWrap(true);
	hint_->setProperty("ctdRole", "muted");
	hint_->setVisible(false);
	layout->addWidget(hint_);
	use_ = new QLabel(this);
	use_->setWordWrap(true);
	use_->setProperty("ctdRole", "muted");
	use_->setVisible(false);
	layout->addWidget(use_);
	switch_ = ctd::button(new QPushButton(this));
	switch_->setToolTip(uiText("Overlays.Address.Switch.Tip"));
	layout->addWidget(switch_);
	result_ = new QLabel(this);
	result_->setWordWrap(true);
	result_->setProperty("ctdRole", "muted");
	result_->setVisible(false);
	layout->addWidget(result_);

	QObject::connect(choice_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
		const Address a = index == 1 ? Address::Public : Address::Local;
		if (a == Address::Public && !knownPublic_) {
			showChoice(); /* grayed out: not pickable, but keep the box honest */
			return;
		}
		saveAddress(profile_, a);
		seq_ = savedAddressSeq();
		showChoice();
	});
	QObject::connect(switch_, &QPushButton::clicked, this, [this] { switchSources(); });
	/* the collection changes under us (sources added by hand, another collection): counted again now and then, only
	 * while the dock shows */
	scan_ = new QTimer(this);
	QObject::connect(scan_, &QTimer::timeout, this, [this] {
		if (isVisible())
			showUse();
	});
	scan_->start(kScanMs);
	showChoice();
}

AddressPicker::~AddressPicker()
{
	scan_->stop();
	obs_data_array_release(overlays_);
}

void AddressPicker::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	showUse();
}

void AddressPicker::setContext(const QString &relayProfile, obs_data_array_t *overlays)
{
	const uint64_t seq = savedAddressSeq();
	if (relayProfile == profile_ && overlays == overlays_ && seq == seq_)
		return;
	const bool listChanged = overlays != overlays_ || relayProfile != profile_;
	profile_ = relayProfile;
	seq_ = seq;
	if (overlays != overlays_) {
		obs_data_array_addref(overlays);
		obs_data_array_release(overlays_);
		overlays_ = overlays;
	}
	knownLocal_ = knownPublic_ = false;
	const size_t count = overlays_ ? obs_data_array_count(overlays_) : 0;
	for (size_t i = 0; i < count; i++) {
		obs_data_t *o = obs_data_array_item(overlays_, i);
		const ctd_overlay_origins origin = origins(o);
		knownLocal_ = knownLocal_ || ctd_overlay_has_address(&origin, CTD_ADDRESS_LOCAL);
		knownPublic_ = knownPublic_ || ctd_overlay_has_address(&origin, CTD_ADDRESS_PUBLIC);
		obs_data_release(o);
	}
	showChoice();
	if (listChanged) {
		result_->setVisible(false);
		if (isVisible())
			showUse();
	}
}

bool AddressPicker::known(Address a) const
{
	return a == Address::Public ? knownPublic_ : knownLocal_;
}

Address AddressPicker::address() const
{
	const Address chosen = savedAddress(profile_);
	return chosen == Address::Public && !knownPublic_ ? Address::Local : chosen;
}

void AddressPicker::rescan()
{
	showUse();
}

QString AddressPicker::unavailableText(Address a)
{
	return uiText(a == Address::Public ? "Overlays.Address.NotPublic" : "Overlays.Address.NotLocal");
}

void AddressPicker::showChoice()
{
	/* Public is grayed out while no overlay has a public address; the choice itself is kept for when one comes */
	if (auto *model = qobject_cast<QStandardItemModel *>(choice_->model())) {
		if (QStandardItem *pub = model->item(1)) {
			pub->setEnabled(knownPublic_);
			pub->setToolTip(knownPublic_ ? QString() : uiText("Overlays.Address.NoPublic"));
		}
	}
	const Address a = address();
	choice_->blockSignals(true);
	choice_->setCurrentIndex(a == Address::Public ? 1 : 0);
	choice_->blockSignals(false);
	choice_->setEnabled(overlays_ && obs_data_array_count(overlays_) > 0);
	hint_->setVisible(!knownPublic_ && overlays_ && obs_data_array_count(overlays_) > 0);
	switch_->setText(
		uiText(a == Address::Public ? "Overlays.Address.SwitchPublic" : "Overlays.Address.SwitchLocal"));
	switch_->setEnabled(known(a));
}

void AddressPicker::showUse()
{
	const AddressUse use = countAddressUse(overlays_);
	if (counted_ && use == useCounts_)
		return;
	counted_ = true;
	useCounts_ = use;
	QStringList parts;
	if (use.local)
		parts << uiText("Overlays.Address.UseLocal").arg(use.local);
	if (use.pub)
		parts << uiText("Overlays.Address.UsePublic").arg(use.pub);
	if (use.unknown)
		parts << uiText("Overlays.Address.UseUnknown").arg(use.unknown);
	use_->setText(parts.isEmpty() ? QString() : uiText("Overlays.Address.Use").arg(parts.join(", ")));
	use_->setVisible(!parts.isEmpty());
}

void AddressPicker::switchSources()
{
	if (!known(address()))
		return;
	/* the browser sources reload: on air, that shows. The question runs its own event loop: the dock (and this
	 * picker) may be gone when it returns, and the profile or its overlays may have changed. */
	if (obs_frontend_streaming_active() || obs_frontend_recording_active()) {
		QPointer<AddressPicker> self(this);
		const auto answer = QMessageBox::question(this, uiText("Common.DialogTitle"),
							  uiText("Overlays.Address.SwitchLive"));
		if (!self || answer != QMessageBox::Yes)
			return;
	}
	const Address to = address();
	if (!known(to))
		return;
	const SwitchResult r = switchAddress(overlays_, to);
	QStringList parts;
	if (r.changed)
		parts << uiText(to == Address::Public ? "Overlays.Address.SwitchedPublic"
						      : "Overlays.Address.SwitchedLocal")
				 .arg(r.changed);
	else if (!r.already && !r.unavailable)
		parts << uiText("Overlays.Address.NoneToSwitch");
	if (r.already)
		parts << uiText("Overlays.Address.Already").arg(r.already);
	if (r.unavailable)
		parts << uiText(to == Address::Public ? "Overlays.Address.UnavailablePublic"
						      : "Overlays.Address.UnavailableLocal")
				 .arg(r.unavailable);
	result_->setText(parts.join(" "));
	result_->setVisible(true);
	showUse();
}

} // namespace ctd::overlays
