/*
 * CTD Overlay overlays as OBS browser sources, shared by the CTD Relay dock and the CTD Overlays dock: an overlay is
 * one of CTD Relay's overlay resources (GET /profiles/{id}/overlays items: id, name, url, width, height, appId,
 * appName, appOnline, meta { canvas, slot, scenes? }). Adding one puts a browser source at the overlay's size,
 * top-left, in the current scene of its canvas (the vertical canvas for a vertical overlay), marked with private
 * settings so "Switch overlays" and the CTD Overlays dock can find it again.
 *
 * UI thread only (the frontend API).
 */
#pragma once

#include <obs.h>

#include <QString>
#include <QWidget>

#include <string>
#include <vector>

#include "../overlay-address.h"

class QComboBox;
class QLabel;
class QPushButton;
class QTimer;

namespace ctd::overlays {

/* the private settings keys that mark a browser source as one of the hub's overlays (H21) */
constexpr const char *kSlot = "ctd_overlay_slot";
constexpr const char *kCanvas = "ctd_overlay_canvas";
constexpr const char *kName = "ctd_overlay_name";
constexpr const char *kApp = "ctd_overlay_app";
/* the overlay's scene the source is pinned to (meta.scenes id, SKP 04 §4.9); empty or absent = the whole overlay */
constexpr const char *kScene = "ctd_overlay_scene";
/* the overlay resource's id ("<overlay id>:<canvas>[:<profile>]"), so the CTD Overlays dock can tell it apart */
constexpr const char *kResource = "ctd_overlay_resource";

/* landscape or vertical: meta.canvas, else the shape */
QString canvas(obs_data_t *overlay);
/* meta.slot: the overlay's role in OBS, the same for the equivalent overlay of every profile */
QString slot(obs_data_t *overlay);

/* One of the scenes CTD Overlay says a URL may pin (meta.scenes: [{ id, name, core?, url? }]). */
struct Scene {
	QString id;
	QString name;
	std::string url; /* its own browser-source URL when CTD Overlay gives one; else the overlay's with scene=<id> */
};

/* meta.scenes, the ones with an id; empty when the overlay offers no choice (one scene, or an older CTD Overlay) */
std::vector<Scene> scenes(obs_data_t *overlay);

/* the name of the overlay's scene `id`, or the id itself */
QString sceneName(obs_data_t *overlay, const QString &id);

/* the overlay's URL showing only `scene` (the whole overlay when it is empty): the scene's own url from meta.scenes,
 * else the overlay's url with scene=<id> set (SKP 04 §4.9). At the address CTD Overlay gave it (the default one). */
std::string sceneUrl(obs_data_t *overlay, const QString &scene);

/* Which of CTD Overlay's addresses a browser source opens an overlay at (meta.addresses; overlay-address.h). */
enum class Address { Local, Public };

/* the overlay's meta.addresses, checked: an http:// public address is refused, an older CTD Overlay is "legacy" */
ctd_overlay_origins origins(obs_data_t *overlay);

/* Is the overlay known at `address`? (Local: always, for an older CTD Overlay: its one URL.) */
bool hasAddress(obs_data_t *overlay, Address address);

/* sceneUrl at `address`: the same path and query at that address's origin. Empty when the overlay isn't known there.
 * The URL holds the overlay's key: never log it. */
std::string sceneUrl(obs_data_t *overlay, const QString &scene, Address address);

/* The address chosen for overlays of the CTD Relay profile `relayProfile` (empty: no profile yet), remembered in the
 * module's config (overlay-address.json); Local until the operator picks Public. UI thread only. */
Address savedAddress(const QString &relayProfile);
void saveAddress(const QString &relayProfile, Address address);
/* goes up on every saveAddress, so each dock's picker can follow the other's */
uint64_t savedAddressSeq();

/* The vertical canvas (CTD OBS's built-in one, Aitum's, or anything made with OBS's canvas API): the first canvas
 * other than the main one whose picture is taller than wide. A strong reference, or nullptr. */
obs_canvas_t *findVerticalCanvas();

/* The scene showing on a canvas's output: channel 0, through its transition. A strong reference, or nullptr. */
obs_source_t *canvasCurrentScene(obs_canvas_t *canvas);

/* Points a browser source at the overlay (or one of its scenes) at its size and marks it as ours. It is shut down
 * when not visible (obs-browser's "shutdown"), so only overlays that show keep a browser running. At `address`;
 * when the overlay isn't known there, at the address CTD Overlay gave it, and false is returned. */
bool setSourceSettings(obs_source_t *source, obs_data_t *overlay, const QString &scene,
		       Address address = Address::Local);

/* Turns on "shut down when not visible" for every overlay browser source of ours that lacks it (collections made
 * before it was set). UI thread. Returns how many were changed. */
int shutdownHiddenOverlays();

/* A browser source showing the overlay (or one of its scenes), at the overlay's size, top-left in `scene`. Named
 * after the overlay ("<overlay> · <scene>" for a scene), with a number added when the name is taken. Returns the name
 * it got, or an empty string. */
std::string addSource(obs_scene_t *scene, obs_data_t *overlay, const QString &sceneId,
		      Address address = Address::Local);

struct AddResult {
	std::string name; /* the new source's name; empty when nothing was added */
	QString where;    /* "the current scene", "the current scene of <canvas>", "the main canvas's current scene" */
	bool noVertical = false; /* a vertical overlay, but no vertical canvas: it went on the main canvas */
	bool noAddress = false;  /* the overlay isn't known at the address asked for: nothing was added */
};

/* H21: a browser source at the overlay's size, in the current scene of the canvas meta.canvas names (the main canvas
 * for landscape, the vertical canvas for vertical). sceneId: one of its scenes (meta.scenes), pinned with scene=<id>;
 * empty = the whole overlay, which follows its show. Logs what it added. */
AddResult addToCurrentScene(obs_data_t *overlay, const QString &sceneId = QString(), Address address = Address::Local);

/* How many browser sources of the current scene collection show one of `overlays` (GET /profiles/{id}/overlays
 * items) at each address: matched by URL (origin and path, overlay-address.h), whoever added them. */
struct AddressUse {
	int local = 0;
	int pub = 0;
	int unknown = 0; /* an older CTD Overlay's, whose address isn't known */
	bool operator==(const AddressUse &o) const { return local == o.local && pub == o.pub && unknown == o.unknown; }
};

/* "Switch to public / local": the outcome of rewriting existing sources */
struct SwitchResult {
	int changed = 0;
	int already = 0;
	int unavailable = 0; /* their overlay isn't known at that address: left as they were */
};

/*
 * The Address row both docks put by their overlays: Local network or Public (internet), remembered per CTD Relay
 * profile (savedAddress); Public grayed out, with "Set a public address in CTD Overlay", while none of the overlays has
 * one. Under it: which address the collection's CTD overlay sources use, and "Switch to public" / "Switch to local",
 * which rewrites them to the chosen one (only sources whose URL is one of the overlays' at one of their origins; the
 * query, scene= and key= included, is kept).
 *
 * The dock calls setContext() from its refresh. Nothing calls back into the dock: the picker reports in its own
 * label, and its timer and signals are its children, gone with it. UI thread only.
 */
class AddressPicker : public QWidget {
public:
	explicit AddressPicker(QWidget *parent);
	~AddressPicker() override;

	/* the CTD Relay profile and its overlays (GET /profiles/{id}/overlays items; NULL = none). Cheap when nothing
	 * changed. */
	void setContext(const QString &relayProfile, obs_data_array_t *overlays);

	/* the address Add to scene, Copy URL and Open use: the chosen one, Local while Public isn't known */
	Address address() const;
	/* is any overlay known at `a`? */
	bool known(Address a) const;
	/* a new count of the collection's sources (after adding one) */
	void rescan();

	/* the text for an overlay that isn't known at the chosen address */
	static QString unavailableText(Address a);

protected:
	void showEvent(QShowEvent *event) override;

private:
	void showChoice();
	void showUse();
	void switchSources();

	QComboBox *choice_;
	QLabel *hint_;
	QLabel *use_;
	QPushButton *switch_;
	QLabel *result_;
	QTimer *scan_;
	QString profile_;
	obs_data_array_t *overlays_ = nullptr; /* held (addref) */
	uint64_t seq_ = UINT64_MAX;
	bool knownLocal_ = false;
	bool knownPublic_ = false;
	AddressUse useCounts_;
	bool counted_ = false;
};

AddressUse countAddressUse(obs_data_array_t *overlays);
SwitchResult switchAddress(obs_data_array_t *overlays, Address to);

} // namespace ctd::overlays
