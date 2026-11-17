/* basic.ini filter/merge tests (ticket T9). */
#include "ini.h"
#include "unit.h"

#include <stdlib.h>
#include <string.h>

static void test_allow_list(void)
{
	CHECK(show_ini_key_allowed("Video", "BaseCX"));
	CHECK(show_ini_key_allowed("video", "basecx"));
	CHECK(show_ini_key_allowed("Audio", "SampleRate"));
	CHECK(show_ini_key_allowed("Output", "Mode"));
	CHECK(show_ini_key_allowed("SimpleOutput", "VBitrate"));
	CHECK(show_ini_key_allowed("AdvOut", "Track1Bitrate"));
	CHECK(show_ini_key_allowed("AdvOut", "Track6Name"));
	CHECK(show_ini_key_allowed("AdvOut", "RecRBTime"));
	CHECK(!show_ini_key_allowed("AdvOut", "Track7Bitrate"));
	CHECK(!show_ini_key_allowed("AdvOut", "Track1Path"));
	CHECK(!show_ini_key_allowed("AdvOut", "RecFilePath"));
	CHECK(!show_ini_key_allowed("SimpleOutput", "FilePath"));
	CHECK(!show_ini_key_allowed("AdvOut", "FFFilePath"));
	CHECK(!show_ini_key_allowed("Audio", "MonitoringDeviceId"));
	CHECK(!show_ini_key_allowed("Hotkeys", "OBSBasic.StartStreaming"));
	CHECK(!show_ini_key_allowed("General", "Name"));
	CHECK(!show_ini_key_allowed("Video", "Unknown"));
	CHECK(!show_ini_key_allowed("Stream1", "Key"));
	CHECK(!show_ini_key_allowed(NULL, "x"));
	CHECK(!show_ini_key_allowed("Video", ""));
}

static void test_filter(void)
{
	char *text = unit_fixture_text("export/profile/basic.ini");
	char *f = text ? show_ini_filter(text) : NULL;
	char v[64];
	CHECK(f != NULL);
	if (!f) {
		free(text);
		return;
	}
	CHECK(show_ini_get(f, "Video", "BaseCX", v, sizeof(v)) && strcmp(v, "1920") == 0);
	CHECK(show_ini_get(f, "Video", "ColorRange", v, sizeof(v)) && strcmp(v, "Partial") == 0);
	CHECK(show_ini_get(f, "Audio", "SampleRate", v, sizeof(v)) && strcmp(v, "48000") == 0);
	CHECK(show_ini_get(f, "Output", "Mode", v, sizeof(v)) && strcmp(v, "Advanced") == 0);
	CHECK(show_ini_get(f, "AdvOut", "Track1Name", v, sizeof(v)) && strcmp(v, "Main") == 0);
	CHECK(show_ini_get(f, "AdvOut", "RecRBTime", v, sizeof(v)) && strcmp(v, "30") == 0);
	CHECK(show_ini_get(f, "SimpleOutput", "VBitrate", v, sizeof(v)) && strcmp(v, "2500") == 0);
	CHECK(!show_ini_get(f, "SimpleOutput", "FilePath", v, sizeof(v)));
	CHECK(!show_ini_get(f, "AdvOut", "RecFilePath", v, sizeof(v)));
	CHECK(!show_ini_get(f, "Audio", "MonitoringDeviceId", v, sizeof(v)));
	CHECK(!show_ini_get(f, "General", "Name", v, sizeof(v)));
	CHECK(!unit_contains(f, "Hotkeys"));
	CHECK(!unit_contains(f, "OBS_KEY"));
	CHECK(!unit_contains(f, "Users"));
	CHECK(!unit_contains(f, "geometry"));
	CHECK(!unit_contains(f, "\r"));
	/* order is kept: Video before Audio before Output */
	CHECK(strstr(f, "[Video]") < strstr(f, "[Audio]"));
	CHECK(strstr(f, "[Audio]") < strstr(f, "[Output]"));
	CHECK(strstr(f, "BaseCX") < strstr(f, "OutputCX"));
	/* filtering twice changes nothing */
	char *g = show_ini_filter(f);
	CHECK(g && strcmp(f, g) == 0);
	free(g);
	free(f);
	free(text);

	f = show_ini_filter("");
	CHECK(f && f[0] == '\0');
	free(f);
	f = show_ini_filter(NULL);
	CHECK(f && f[0] == '\0');
	free(f);
}

static void test_crlf_bom(void)
{
	const char *src =
		"\xEF\xBB\xBF[Video]\r\nBaseCX=1280\r\nRecFilePath=C:\\x\r\n\r\n[Audio]\r\nSampleRate=44100\r\n";
	char *f = show_ini_filter(src);
	char v[32];
	CHECK(f != NULL);
	if (!f)
		return;
	CHECK(show_ini_get(src, "Video", "BaseCX", v, sizeof(v)) && strcmp(v, "1280") == 0);
	CHECK(strcmp(f, "[Video]\nBaseCX=1280\n\n[Audio]\nSampleRate=44100\n") == 0);
	free(f);
	CHECK(!show_ini_get(src, "Video", "Nope", v, sizeof(v)));
	CHECK(!show_ini_get(src, "Nope", "BaseCX", v, sizeof(v)));
	CHECK(!show_ini_get(src, "Video", "BaseCX", v, 3)); /* doesn't fit */
}

static void test_merge(void)
{
	const char *local = "[General]\r\nName=Old name\r\nConfirmOnExit=true\r\n\r\n"
			    "[Video]\r\nBaseCX=1280\r\nBaseCY=720\r\n\r\n"
			    "[AdvOut]\r\nRecFilePath=D:\\Recordings\r\nEncoder=jim_nvenc\r\n\r\n"
			    "[Hotkeys]\r\nOBSBasic.StartStreaming=keep-me\r\n";
	const char *bundle = "[Video]\nBaseCX=1920\nBaseCY=1080\nFPSType=0\n\n"
			     "[Audio]\nSampleRate=48000\n\n"
			     "[AdvOut]\nEncoder=obs_x264\nRecFilePath=/evil/path\n\n"
			     "[Hotkeys]\nOBSBasic.StartStreaming=evil\n\n[General]\nName=Evil\n";
	char *m = show_ini_merge(bundle, local, "DPRTMNT");
	char v[128];
	CHECK(m != NULL);
	if (!m)
		return;
	/* carried keys take the bundle's value, in the local position */
	CHECK(show_ini_get(m, "Video", "BaseCX", v, sizeof(v)) && strcmp(v, "1920") == 0);
	CHECK(show_ini_get(m, "Video", "BaseCY", v, sizeof(v)) && strcmp(v, "1080") == 0);
	CHECK(show_ini_get(m, "Video", "FPSType", v, sizeof(v)) && strcmp(v, "0") == 0);
	CHECK(strstr(m, "BaseCX=1920") < strstr(m, "BaseCY=1080") && strstr(m, "BaseCY=1080") < strstr(m, "FPSType=0"));
	CHECK(show_ini_get(m, "AdvOut", "Encoder", v, sizeof(v)) && strcmp(v, "obs_x264") == 0);
	/* a section the machine lacked is added */
	CHECK(show_ini_get(m, "Audio", "SampleRate", v, sizeof(v)) && strcmp(v, "48000") == 0);
	/* local keys stay; the bundle can't set what isn't carried */
	CHECK(show_ini_get(m, "AdvOut", "RecFilePath", v, sizeof(v)) && strcmp(v, "D:\\Recordings") == 0);
	CHECK(show_ini_get(m, "Hotkeys", "OBSBasic.StartStreaming", v, sizeof(v)) && strcmp(v, "keep-me") == 0);
	CHECK(show_ini_get(m, "General", "ConfirmOnExit", v, sizeof(v)) && strcmp(v, "true") == 0);
	CHECK(show_ini_get(m, "General", "Name", v, sizeof(v)) && strcmp(v, "DPRTMNT") == 0);
	CHECK(!unit_contains(m, "\r"));
	CHECK(!unit_contains(m, "evil"));
	CHECK(strstr(m, "[General]") < strstr(m, "[Video]"));
	free(m);

	/* no local profile yet */
	m = show_ini_merge(bundle, NULL, "Fresh");
	CHECK(m && show_ini_get(m, "General", "Name", v, sizeof(v)) && strcmp(v, "Fresh") == 0);
	CHECK(m && show_ini_get(m, "Video", "BaseCX", v, sizeof(v)) && strcmp(v, "1920") == 0);
	free(m);
	m = show_ini_merge(bundle, "", "Fresh");
	CHECK(m && show_ini_get(m, "Audio", "SampleRate", v, sizeof(v)));
	free(m);
}

void test_ini(void)
{
	test_allow_list();
	test_filter();
	test_crlf_bom();
	test_merge();
}
