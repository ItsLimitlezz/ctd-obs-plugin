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

#include <string>
#include <vector>

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
 * else the overlay's url with scene=<id> set (SKP 04 §4.9) */
std::string sceneUrl(obs_data_t *overlay, const QString &scene);

/* The vertical canvas (CTD OBS's built-in one, Aitum's, or anything made with OBS's canvas API): the first canvas
 * other than the main one whose picture is taller than wide. A strong reference, or nullptr. */
obs_canvas_t *findVerticalCanvas();

/* The scene showing on a canvas's output: channel 0, through its transition. A strong reference, or nullptr. */
obs_source_t *canvasCurrentScene(obs_canvas_t *canvas);

/* Points a browser source at the overlay (or one of its scenes) at its size and marks it as ours. */
void setSourceSettings(obs_source_t *source, obs_data_t *overlay, const QString &scene);

/* A browser source showing the overlay (or one of its scenes), at the overlay's size, top-left in `scene`. Named
 * after the overlay ("<overlay> · <scene>" for a scene), with a number added when the name is taken. Returns the name
 * it got, or an empty string. */
std::string addSource(obs_scene_t *scene, obs_data_t *overlay, const QString &sceneId);

struct AddResult {
	std::string name; /* the new source's name; empty when nothing was added */
	QString where;    /* "the current scene", "the current scene of <canvas>", "the main canvas's current scene" */
	bool noVertical = false; /* a vertical overlay, but no vertical canvas: it went on the main canvas */
};

/* H21: a browser source at the overlay's size, in the current scene of the canvas meta.canvas names (the main canvas
 * for landscape, the vertical canvas for vertical). sceneId: one of its scenes (meta.scenes), pinned with scene=<id>;
 * empty = the whole overlay, which follows its show. Logs what it added. */
AddResult addToCurrentScene(obs_data_t *overlay, const QString &sceneId = QString());

} // namespace ctd::overlays
