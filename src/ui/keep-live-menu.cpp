/*
 * Tools ▸ "Let CTD apps keep multiview scenes live": the switch for KeepScenesLive (src/obs/keep-live.h). On by default;
 * saved in OBS's user config ([CTDLive] KeepScenesLive) by keep_live_set_enabled.
 */

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAction>
#include <QString>

#include "../obs/keep-live.h"

extern "C" void ctd_keep_live_menu_register(void)
{
	auto *action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("KeepLive.Menu")));
	if (!action)
		return;
	action->setCheckable(true);
	action->setChecked(keep_live_enabled());
	action->setToolTip(QString::fromUtf8(obs_module_text("KeepLive.Menu.Tooltip")));
	action->setStatusTip(action->toolTip());
	QObject::connect(action, &QAction::toggled, [](bool on) { keep_live_set_enabled(on); });
}
