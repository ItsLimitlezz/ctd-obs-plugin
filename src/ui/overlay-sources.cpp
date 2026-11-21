/* See overlay-sources.hpp. Moved here from the CTD Relay dock, unchanged in behaviour, so the CTD Overlays dock adds
 * overlays exactly the same way. */
#include "overlay-sources.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <cstdlib>

#include "../overlay-scenes.h"

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

void setSourceSettings(obs_source_t *source, obs_data_t *overlay, const QString &scene)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "url", sceneUrl(overlay, scene).c_str());
	obs_data_set_int(settings, "width", obs_data_get_int(overlay, "width"));
	obs_data_set_int(settings, "height", obs_data_get_int(overlay, "height"));
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
}

std::string addSource(obs_scene_t *scene, obs_data_t *overlay, const QString &sceneId)
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
	setSourceSettings(source, overlay, sceneId);
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

AddResult addToCurrentScene(obs_data_t *overlay, const QString &sceneId)
{
	AddResult result;
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
	result.name = addSource(scene, overlay, sceneId);
	if (!result.name.empty())
		blog(LOG_INFO, "[CTD] relay: added overlay '%s'%s%s to %s", result.name.c_str(),
		     sceneId.isEmpty() ? "" : ", scene ", sceneId.toUtf8().constData(),
		     result.where.toUtf8().constData());
	obs_source_release(sceneSource);
	return result;
}

} // namespace ctd::overlays
