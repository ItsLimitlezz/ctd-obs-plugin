/*
 * Tools ▸ "Let CTD apps find this OBS": the switch for network discovery (src/obs/discovery.h). On by default; saved in
 * OBS's user config ([CTDLive] Discoverable).
 */

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>

#include <QAction>
#include <QString>

#include "../obs/discovery.h"

namespace {

const char *const SECTION = "CTDLive";
const char *const KEY = "Discoverable";

} // namespace

extern "C" void ctd_discovery_menu_register(void)
{
	config_t *config = obs_frontend_get_user_config();
	if (config)
		config_set_default_bool(config, SECTION, KEY, true);
	discovery_set_enabled(config ? config_get_bool(config, SECTION, KEY) : true);

	auto *action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("Discovery.Menu")));
	if (!action)
		return;
	action->setCheckable(true);
	action->setChecked(discovery_enabled());
	action->setToolTip(QString::fromUtf8(obs_module_text("Discovery.Menu.Tooltip")));
	QObject::connect(action, &QAction::toggled, [](bool on) {
		discovery_set_enabled(on);
		if (config_t *config = obs_frontend_get_user_config()) {
			config_set_bool(config, SECTION, KEY, on);
			config_save_safe(config, "tmp", nullptr);
		}
	});
}
