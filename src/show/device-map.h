/* M4 device mapping in the plugin (design section 5, tickets T11 + T13): the libobs side of devices.h.
 *
 * - On a switch (stage hook, worker thread, before OBS changes): this machine's devices are listed per source type, the
 *   bundle's device slots are scored (devices.h) against them with this machine's remembered map and aliases, and the
 *   chosen devices are written into the staged collection. Missing slots keep their settings.
 * - After the switch is verified (applied hook, UI thread): remembered + auto slots are saved into this machine's
 *   remembered map, and the "Fix mapping" dialog opens when a slot is suggested, missing, or remembered but unplugged.
 * - The dialog (T13, src/ui/device-map-dialog.cpp) reads rows with show_devices_get and saves choices with
 *   show_devices_choose. Changing a device is not a switch: it is allowed while live (design section 5).
 *
 * Everything here is called on OBS's UI thread unless noted. */
#ifndef CTD_SHOW_DEVICE_MAP_H
#define CTD_SHOW_DEVICE_MAP_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* show.c calls these from show_init / show_shutdown. */
void show_devices_init(void);
void show_devices_shutdown(void);

struct show_device_option {
	char *id;   /* the value OBS stores in the source's settings */
	char *name; /* what the dropdown shows */
};

struct show_device_row {
	char *slot;              /* the source's uuid */
	char *source_name;       /* the source's name in OBS now ("" when the source is gone) */
	char *label;             /* the source's name when the profile was saved */
	char *source_type;       /* "dshow_input", "wasapi_input_capture", ... */
	char *device_class;      /* "video", "audio" or "display" */
	char *was;               /* "Cam Link 4K (studio-pc)": the device on the machine that saved the profile */
	char *status;            /* "remembered", "auto", "suggested" or "missing" (devices.h) */
	int score;               /* 0..100 */
	bool remembered_missing; /* this machine had a device remembered for the slot and it isn't plugged in */
	char *current_id;        /* the device the source uses now ("" when none of this machine's devices) */
	char *current_name;      /* its name ("" when none) */
	struct show_device_option *options; /* this machine's devices of the source's type, in OBS's order */
	size_t option_count;
};

struct show_device_rows {
	char id[37];        /* the show profile */
	char *profile_name; /* its name */
	char *machine;      /* this machine's name */
	struct show_device_row *rows;
	size_t count;
};

/* The rows for show profile id (the active one: the sources must be loaded). Lists this machine's devices now, so a
 * camera plugged in since the switch shows up. False when the profile isn't active or has no device slots. */
bool show_devices_get(const char *id, struct show_device_rows *out);
void show_devices_free(struct show_device_rows *rows);

/* The user's choice for one slot: the source gets device_id now (its settings are updated; allowed while live), and
 * it is saved in this machine's remembered map for the profile and in the machine's alias table (so another profile
 * with the same captured device maps to it too). device_id "" forgets the slot (the source keeps what it has).
 * Returns false (message in err) when the source is gone or the map can't be written. */
bool show_devices_choose(const char *id, const char *slot, const char *device_id, const char *device_name, char *err,
			 size_t err_size);

/* The dock registers how to open the dialog; it is called on the UI thread after a verified switch (or pull + switch)
 * when show_devices_needs_fix says so, and by the dock's "Devices…" button. */
typedef void (*show_devices_open_fn)(void *opaque, const char *id);
void show_devices_set_opener(show_devices_open_fn fn, void *opaque);

#ifdef __cplusplus
}
#endif

#endif
