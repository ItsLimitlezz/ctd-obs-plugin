/*
 * The plugin's settings files and CTD OBS's one CTD settings folder (integration gap 17). See ctd-settings.c.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* First thing in obs_module_load: picks the settings folder (moving the files there once) and registers the procs */
void ctd_settings_init(void);
void ctd_settings_shutdown(void);

/* The full path of one settings file (bmalloc'd, bfree it); its folder exists */
char *ctd_settings_path(const char *file);

#ifdef __cplusplus
}
#endif
