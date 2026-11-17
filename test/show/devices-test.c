/* Device mapping (design section 5, T11 scoring + M4 remembered map): the score table, greedy assignment, shared
 * devices, aliases, applying a mapping to a collection, and the remembered map surviving A -> B -> A exactly. */
#include "unit.h"
#include "devices.h"
#include "show-fs.h"
#include "store.h"

#include <stdlib.h>

#define PROFILE_A "0f8fad5b-d9cb-469f-a165-70867728950e"
#define PROFILE_B "7c9e6679-7425-40de-944b-e07fc1f90ae7"
#define MACHINE "studio-pc"

/* dshow ids as OBS stores them: "<name>:<device path>" */
#define CAMLINK_1 "Cam Link 4K:\\\\?\\usb#vid_0fd9&pid_0066&mi_00#6&1a2b3c4d&0&0000#{65e8773d-8f56-11d0-a3b9-00a0c9223196}\\global"
#define CAMLINK_2 "Cam Link 4K:\\\\?\\usb#vid_0fd9&pid_0066&mi_00#7&99aa00bb&0&0000#{65e8773d-8f56-11d0-a3b9-00a0c9223196}\\global"
#define CAMLINK_OTHER_PC "Cam Link 4K:\\\\?\\usb#vid_0fd9&pid_0066&mi_00#5&deadbeef&0&0000#{65e8773d-8f56-11d0-a3b9-00a0c9223196}\\global"
#define MAGEWELL "USB Capture HDMI:\\\\?\\usb#vid_2935&pid_0001&mi_00#8&11112222&0&0000#{65e8773d-8f56-11d0-a3b9-00a0c9223196}\\global"
#define VIRTUALCAM "OBS Virtual Camera:{A3FCE0F5-3493-419F-958A-ABA1250EC20B}"
#define TESTPATTERN "Test Pattern:{11111111-2222-3333-4444-555555555555}"

static cj *identity(const char *name, const char *id, const char *vid_pid, const char *serial)
{
	cj *i = cj_new_object();
	if (name)
		cj_set_str(i, "name", name);
	if (id)
		cj_set_str(i, "id", id);
	if (vid_pid)
		cj_set_str(i, "vidPid", vid_pid);
	if (serial)
		cj_set_str(i, "serial", serial);
	return i;
}

static void add_device(cj *devices, const char *slot, const char *type, cj *ident, const char *label)
{
	cj *d = cj_append(devices, cj_new_object());
	cj_set_str(d, "slot", slot);
	cj_set_str(d, "sourceType", type);
	cj_set(d, "identity", ident);
	cj_set_str(d, "label", label);
}

static void add_candidate(cj *candidates, const char *type, const char *id, const char *name)
{
	cj *list = cj_get_or_add_array(candidates, type);
	cj *c = cj_append(list, cj_new_object());
	cj_set_str(c, "id", id);
	cj_set_str(c, "name", name);
}

static const cj *slot_entry(const cj *mapping, const char *slot)
{
	for (size_t i = 0; i < cj_size(mapping); i++)
		if (strcmp(cj_get_str(cj_at(mapping, i), "slot"), slot) == 0)
			return cj_at(mapping, i);
	return NULL;
}

static void test_names(void)
{
	char out[256];
	show_device_normalize_name("Cam Link 4K (2)", out, sizeof(out));
	CHECK_STR(out, "cam link 4k");
	show_device_normalize_name("Cam Link 4K #2", out, sizeof(out));
	CHECK_STR(out, "cam link 4k");
	show_device_normalize_name("Elgato  Cam-Link 4K Video", out, sizeof(out));
	CHECK_STR(out, "elgato cam link 4k");
	show_device_normalize_name("Microphone (Yeti Stereo)", out, sizeof(out));
	CHECK_STR(out, "microphone yeti stereo");
	show_device_normalize_name("", out, sizeof(out));
	CHECK_STR(out, "");
	CHECK_STR(show_device_brand("Cam Link 4K"), "elgato");
	CHECK_STR(show_device_brand("Elgato HD60 S+"), "elgato");
	CHECK_STR(show_device_brand("USB Capture HDMI 4K+"), "magewell");
	CHECK_STR(show_device_brand("DeckLink Mini Recorder"), "blackmagic");
	CHECK(show_device_brand("Integrated Webcam") == NULL);
}

static void test_scores(void)
{
	/* 100: same device id */
	cj *same = identity("Cam Link 4K", CAMLINK_1, "0fd9:0066", "6&1a2b3c4d&0&0000");
	CHECK(show_device_score("dshow_input", same, CAMLINK_1, "Cam Link 4K", 2) == 100);
	/* 95: same VID:PID + serial (the id differs, e.g. another interface path) */
	cj *serial = identity("Cam Link 4K", "Cam Link 4K:somewhere-else", "0fd9:0066", "6&1a2b3c4d&0&0000");
	CHECK(show_device_score("dshow_input", serial, CAMLINK_1, "Cam Link 4K", 2) == 95);
	/* 80: same VID:PID, the only one here; 70 when there are two (name still matches) */
	cj *other_pc = identity("Cam Link 4K", CAMLINK_OTHER_PC, "0fd9:0066", "5&deadbeef&0&0000");
	CHECK(show_device_score("dshow_input", other_pc, CAMLINK_1, "Cam Link 4K", 1) == 80);
	CHECK(show_device_score("dshow_input", other_pc, CAMLINK_1, "Cam Link 4K", 2) == 70);
	/* 70: same display name (audio: no USB ids) */
	cj *mic = identity("Microphone (Yeti Stereo)", "{0.0.1.00000000}.{aaaa}", NULL, NULL);
	CHECK(show_device_score("wasapi_input_capture", mic, "{0.0.1.00000000}.{bbbb}", "Microphone (Yeti Stereo)",
				0) == 70);
	/* 60: normalized name */
	CHECK(show_device_score("wasapi_input_capture", mic, "{x}", "Microphone (2- Yeti Stereo)", 0) < 70);
	cj *cam2 = identity("Cam Link 4K (2)", "x", NULL, NULL);
	CHECK(show_device_score("v4l2_input", cam2, "/dev/video4", "Cam Link 4K", 0) == 60);
	/* 30: same brand family */
	cj *hd60 = identity("Elgato HD60 S+", "y", NULL, NULL);
	CHECK(show_device_score("v4l2_input", hd60, "/dev/video2", "Cam Link 4K", 0) == 30);
	/* 0: nothing in common */
	CHECK(show_device_score("dshow_input", same, MAGEWELL, "USB Capture HDMI", 1) == 0);
	cj_free(same);
	cj_free(serial);
	cj_free(other_pc);
	cj_free(mic);
	cj_free(cam2);
	cj_free(hd60);
}

/* "Different machine" fixtures (design section 9, M4): the bundle's devices against a PC with a Cam Link (another
 * unit), the OBS Virtual Camera and a test-pattern device */
static void test_mapping(void)
{
	cj *devices = cj_new_array();
	add_device(devices, "slot-cam", "dshow_input",
		   identity("Cam Link 4K", CAMLINK_OTHER_PC, "0fd9:0066", "5&deadbeef&0&0000"), "Club cam");
	add_device(devices, "slot-wide", "dshow_input", identity("Elgato Facecam", "Elgato Facecam:xyz", NULL, NULL),
		   "Wide");
	add_device(devices, "slot-gone", "dshow_input", identity("Brio 4K Stream", "Brio:abc", "046d:086b", "1"),
		   "Desk cam");
	add_device(devices, "slot-window", "window_capture",
		   identity(NULL, "Chat:Chrome_WidgetWin_1:chrome.exe", NULL, NULL), "Chat");
	cj *candidates = cj_new_object();
	add_candidate(candidates, "dshow_input", CAMLINK_1, "Cam Link 4K");
	add_candidate(candidates, "dshow_input", VIRTUALCAM, "OBS Virtual Camera");
	add_candidate(candidates, "dshow_input", TESTPATTERN, "Test Pattern");

	cj *mapping = show_devices_map(devices, candidates, NULL, NULL);
	CHECK(cj_size(mapping) == 3); /* window capture is never mapped */
	const cj *cam = slot_entry(mapping, "slot-cam");
	CHECK_STR(cj_get_str(cam, "status"), SHOW_DEVICE_AUTO); /* 80: the only Cam Link here */
	CHECK(cj_get_int(cam, "score", 0) >= 70);
	CHECK_STR(cj_get_str(cam, "id"), CAMLINK_1);
	CHECK_STR(cj_get_str(cam, "was"), "Cam Link 4K");
	const cj *wide = slot_entry(mapping, "slot-wide");
	/* only a brand match (30) and that device went to a better slot: never taken from it */
	CHECK_STR(cj_get_str(wide, "status"), SHOW_DEVICE_MISSING);
	const cj *gone = slot_entry(mapping, "slot-gone");
	CHECK_STR(cj_get_str(gone, "status"), SHOW_DEVICE_MISSING);
	CHECK_STR(cj_get_str(gone, "id"), "");
	CHECK(show_devices_needs_fix(mapping));
	cj_free(mapping);

	/* a brand match alone, with the device free: suggested (needs a confirm) */
	cj_free(devices);
	devices = cj_new_array();
	add_device(devices, "slot-wide", "dshow_input", identity("Elgato Facecam", "Elgato Facecam:xyz", NULL, NULL),
		   "Wide");
	mapping = show_devices_map(devices, candidates, NULL, NULL);
	CHECK_STR(cj_get_str(slot_entry(mapping, "slot-wide"), "status"), SHOW_DEVICE_SUGGESTED);
	CHECK_STR(cj_get_str(slot_entry(mapping, "slot-wide"), "id"), CAMLINK_1);
	CHECK(cj_get_int(slot_entry(mapping, "slot-wide"), "score", 0) == 30);
	cj_free(mapping);
	cj_free(devices);
	devices = cj_new_array();
	add_device(devices, "slot-cam", "dshow_input",
		   identity("Cam Link 4K", CAMLINK_OTHER_PC, "0fd9:0066", "5&deadbeef&0&0000"), "Club cam");
	add_device(devices, "slot-cam-2", "dshow_input",
		   identity("Cam Link 4K", CAMLINK_OTHER_PC, "0fd9:0066", "5&deadbeef&0&0000"), "Club cam (again)");
	mapping = show_devices_map(devices, candidates, NULL, NULL);
	/* the bundle shared one device between two sources: both get it */
	CHECK_STR(cj_get_str(slot_entry(mapping, "slot-cam"), "id"), CAMLINK_1);
	CHECK_STR(cj_get_str(slot_entry(mapping, "slot-cam-2"), "id"), CAMLINK_1);
	CHECK(!show_devices_needs_fix(mapping));
	cj_free(mapping);

	/* two of the same model here: the serial decides (95), the other unit is not a guess */
	add_candidate(candidates, "dshow_input", CAMLINK_2, "Cam Link 4K");
	cj_free(devices);
	devices = cj_new_array();
	add_device(devices, "slot-a", "dshow_input", identity("Cam Link 4K", "x", "0fd9:0066", "7&99aa00bb&0&0000"),
		   "A");
	add_device(devices, "slot-b", "dshow_input", identity("Cam Link 4K", "y", "0fd9:0066", "6&1a2b3c4d&0&0000"),
		   "B");
	mapping = show_devices_map(devices, candidates, NULL, NULL);
	CHECK_STR(cj_get_str(slot_entry(mapping, "slot-a"), "id"), CAMLINK_2);
	CHECK_STR(cj_get_str(slot_entry(mapping, "slot-b"), "id"), CAMLINK_1);
	CHECK(cj_get_int(slot_entry(mapping, "slot-a"), "score", 0) == 95);
	cj_free(mapping);

	/* an alias (fixed once in another profile on this machine) maps the same identity everywhere */
	cj *aliases = cj_new_object();
	cj *gone_ident = identity("Brio 4K Stream", "Brio:abc", "046d:086b", "1");
	CHECK(show_devices_alias(aliases, "dshow_input", gone_ident, TESTPATTERN, "Test Pattern"));
	cj_free(devices);
	devices = cj_new_array();
	add_device(devices, "slot-gone", "dshow_input", gone_ident, "Desk cam");
	mapping = show_devices_map(devices, candidates, NULL, aliases);
	CHECK_STR(cj_get_str(slot_entry(mapping, "slot-gone"), "status"), SHOW_DEVICE_AUTO);
	CHECK_STR(cj_get_str(slot_entry(mapping, "slot-gone"), "id"), TESTPATTERN);
	cj_free(mapping);
	cj_free(aliases);
	cj_free(devices);
	cj_free(candidates);
}

static cj *collection_with(const char *uuid, const char *type, const char *device_id)
{
	cj *c = cj_new_object();
	cj_set_str(c, "name", "Show");
	cj *src = cj_append(cj_set(c, "sources", cj_new_array()), cj_new_object());
	cj_set_str(src, "id", type);
	cj_set_str(src, "uuid", uuid);
	cj_set_str(src, "name", "Club cam");
	cj_set_str(cj_set(src, "settings", cj_new_object()), "video_device_id", device_id);
	cj_set_str(cj_get(src, "settings"), "resolution", "1920x1080");
	return c;
}

static void test_apply(void)
{
	const char *uuid = "a1b2c3d4-0000-4000-8000-000000000001";
	cj *collection = collection_with(uuid, "dshow_input", CAMLINK_OTHER_PC);
	cj *mapping = cj_new_array();
	cj *e = cj_append(mapping, cj_new_object());
	cj_set_str(e, "slot", uuid);
	cj_set_str(e, "sourceType", "dshow_input");
	cj_set_str(e, "status", SHOW_DEVICE_AUTO);
	cj_set_str(e, "id", CAMLINK_1);
	cj_set_str(e, "name", "Cam Link 4K");
	cj *missing = cj_append(mapping, cj_new_object());
	cj_set_str(missing, "slot", "a1b2c3d4-0000-4000-8000-000000000002");
	cj_set_str(missing, "sourceType", "dshow_input");
	cj_set_str(missing, "status", SHOW_DEVICE_MISSING);
	cj_set_str(missing, "id", "");
	CHECK(show_devices_apply(collection, mapping) == 1);
	const cj *settings = cj_get(cj_at(cj_get(collection, "sources"), 0), "settings");
	CHECK_STR(cj_get_str(settings, "video_device_id"), CAMLINK_1);
	CHECK_STR(cj_get_str(settings, "resolution"), "1920x1080"); /* everything else stays */
	CHECK(show_devices_apply(collection, mapping) == 0);        /* already so */
	cj_free(mapping);
	cj_free(collection);
}

/* The remembered map (decision 3): on the same machine, switching A -> B -> A gives A exactly the devices it had,
 * even when scoring alone would pick another one. Stored through the real store. */
static void test_remembered(void)
{
	char root[1024], err[256];
	unit_scratch_dir("devices", root, sizeof(root));
	CHECK(show_store_open(root));
	CHECK(show_store_create(PROFILE_A, "DPRTMNT", err, sizeof(err)));
	CHECK(show_store_create(PROFILE_B, "Im Your Bus", err, sizeof(err)));

	/* this machine: two Cam Links */
	cj *candidates = cj_new_object();
	add_candidate(candidates, "dshow_input", CAMLINK_1, "Cam Link 4K");
	add_candidate(candidates, "dshow_input", CAMLINK_2, "Cam Link 4K");
	add_candidate(candidates, "dshow_input", VIRTUALCAM, "OBS Virtual Camera");

	/* A and B were both saved on another PC with "a Cam Link": here that's only a name match (70) for either unit */
	cj *devices_a = cj_new_array();
	add_device(devices_a, "slot-a", "dshow_input",
		   identity("Cam Link 4K", CAMLINK_OTHER_PC, "0fd9:0066", "5&deadbeef&0&0000"), "Club cam");
	cj *devices_b = cj_new_array();
	add_device(devices_b, "slot-b", "dshow_input",
		   identity("Cam Link 4K", CAMLINK_OTHER_PC, "0fd9:0066", "5&deadbeef&0&0000"), "Bus cam");

	/* switch to A: auto picks unit 1; the user fixes it to unit 2 in the dialog */
	cj *map_a = show_store_device_map(PROFILE_A, MACHINE);
	CHECK(map_a == NULL);
	map_a = cj_new_object();
	cj *mapping = show_devices_map(devices_a, candidates, map_a, NULL);
	CHECK_STR(cj_get_str(slot_entry(mapping, "slot-a"), "id"), CAMLINK_1);
	show_devices_remember_auto(map_a, mapping);
	CHECK(show_devices_remember(map_a, "slot-a", CAMLINK_2, "Cam Link 4K"));
	CHECK(show_store_write_device_map(PROFILE_A, MACHINE, map_a));
	cj_free(mapping);
	cj_free(map_a);

	/* switch to B: its own map, unit 1 by score */
	cj *map_b = cj_new_object();
	mapping = show_devices_map(devices_b, candidates, NULL, NULL);
	CHECK_STR(cj_get_str(slot_entry(mapping, "slot-b"), "id"), CAMLINK_1);
	CHECK(show_devices_remember_auto(map_b, mapping));
	CHECK(show_store_write_device_map(PROFILE_B, MACHINE, map_b));
	cj_free(mapping);
	cj_free(map_b);

	/* back to A: exactly unit 2, remembered */
	map_a = show_store_device_map(PROFILE_A, MACHINE);
	CHECK(map_a != NULL);
	mapping = show_devices_map(devices_a, candidates, map_a, NULL);
	const cj *a = slot_entry(mapping, "slot-a");
	CHECK_STR(cj_get_str(a, "status"), SHOW_DEVICE_REMEMBERED);
	CHECK_STR(cj_get_str(a, "id"), CAMLINK_2);
	CHECK(!show_devices_needs_fix(mapping));
	CHECK(!show_devices_remember_auto(map_a, mapping)); /* nothing new to remember */
	CHECK(cj_get_bool(cj_get(map_a, "slot-a"), "confirmed", false));
	cj_free(mapping);

	/* and B again: still unit 1 */
	map_b = show_store_device_map(PROFILE_B, MACHINE);
	mapping = show_devices_map(devices_b, candidates, map_b, NULL);
	CHECK_STR(cj_get_str(slot_entry(mapping, "slot-b"), "id"), CAMLINK_1);
	CHECK_STR(cj_get_str(slot_entry(mapping, "slot-b"), "status"), SHOW_DEVICE_REMEMBERED);
	cj_free(mapping);

	/* unit 2 unplugged: A's remembered device is missing -> the dialog opens, the slot falls back to a score */
	cj *fewer = cj_new_object();
	add_candidate(fewer, "dshow_input", CAMLINK_1, "Cam Link 4K");
	mapping = show_devices_map(devices_a, fewer, map_a, NULL);
	a = slot_entry(mapping, "slot-a");
	CHECK(cj_get_bool(a, "rememberedMissing", false));
	CHECK_STR(cj_get_str(a, "id"), CAMLINK_1);
	CHECK(show_devices_needs_fix(mapping));
	cj_free(mapping);

	/* aliases persist per machine */
	cj *aliases = cj_new_object();
	CHECK(show_devices_alias(aliases, "dshow_input", cj_get(cj_at(devices_a, 0), "identity"), CAMLINK_2,
				 "Cam Link 4K"));
	CHECK(show_store_write_device_aliases(MACHINE, aliases));
	cj *back = show_store_device_aliases(MACHINE);
	CHECK(cj_equal(back, aliases));
	CHECK(show_store_device_aliases("../evil") == NULL);
	cj_free(back);
	cj_free(aliases);

	/* forgetting a slot */
	CHECK(show_devices_remember(map_a, "slot-a", "", ""));
	CHECK(cj_get(map_a, "slot-a") == NULL);

	cj_free(fewer);
	cj_free(map_a);
	cj_free(map_b);
	cj_free(devices_a);
	cj_free(devices_b);
	cj_free(candidates);
	show_store_close();
}

void test_devices(void)
{
	test_names();
	test_scores();
	test_mapping();
	test_apply();
	test_remembered();
}
