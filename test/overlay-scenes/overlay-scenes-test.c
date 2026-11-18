/* src/overlay-scenes.c: a scene's URL, reading it back, and matching OBS scenes by name. */
#include "overlay-scenes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void url_is(const char *url, const char *scene, const char *want)
{
	char *got = ctd_overlay_scene_url(url, scene);
	if (!got || strcmp(got, want) != 0) {
		fprintf(stderr, "FAIL scene_url(%s, %s): got %s, want %s\n", url, scene ? scene : "NULL",
			got ? got : "NULL", want);
		failures++;
	}
	free(got);
}

static void scene_of_is(const char *url, const char *want)
{
	char *got = ctd_overlay_scene_of_url(url);
	if ((got == NULL) != (want == NULL) || (got && strcmp(got, want) != 0)) {
		fprintf(stderr, "FAIL scene_of_url(%s): got %s, want %s\n", url, got ? got : "NULL",
			want ? want : "NULL");
		failures++;
	}
	free(got);
}

static void match_is(const char *obs, const char *id, const char *name, bool want)
{
	if (ctd_overlay_scene_matches(obs, id, name) != want) {
		fprintf(stderr, "FAIL matches(%s, %s, %s) != %d\n", obs ? obs : "NULL", id ? id : "NULL",
			name ? name : "NULL", want);
		failures++;
	}
}

int main(void)
{
	/* the key and canvas stay as they are; scene goes last */
	url_is("http://h:4848/o/takeover?key=AbC_123-x&canvas=v", "brb",
	       "http://h:4848/o/takeover?key=AbC_123-x&canvas=v&scene=brb");
	url_is("http://h/o/x", "starting", "http://h/o/x?scene=starting");
	/* a scene it had is replaced, wherever it was */
	url_is("http://h/o/x?scene=brb&key=k", "outro", "http://h/o/x?key=k&scene=outro");
	url_is("http://h/o/x?key=k&scene=brb&canvas=v", "outro", "http://h/o/x?key=k&canvas=v&scene=outro");
	/* the whole overlay: no scene */
	url_is("http://h/o/x?key=k&scene=brb", NULL, "http://h/o/x?key=k");
	url_is("http://h/o/x?scene=brb", "", "http://h/o/x");
	url_is("http://h/o/x?key=k", NULL, "http://h/o/x?key=k");
	/* sceneX= is another parameter */
	url_is("http://h/o/x?scenery=1", "a", "http://h/o/x?scenery=1&scene=a");
	/* a fragment stays at the end; odd characters are encoded */
	url_is("http://h/o/x?key=k#top", "a b&c", "http://h/o/x?key=k&scene=a%20b%26c#top");
	url_is("http://h/o/x?", "a", "http://h/o/x?scene=a");
	if (ctd_overlay_scene_url(NULL, "a") != NULL) {
		fprintf(stderr, "FAIL scene_url(NULL)\n");
		failures++;
	}

	scene_of_is("http://h/o/x?key=k&canvas=v&scene=brb", "brb");
	scene_of_is("http://h/o/x?scene=a%20b%26c#top", "a b&c");
	scene_of_is("http://h/o/x?key=k", NULL);
	scene_of_is("http://h/o/x?scene=", NULL);
	scene_of_is("http://h/o/x?scenery=1", NULL);
	scene_of_is("http://h/o/x#scene=a", NULL);
	scene_of_is("http://h/o/x?scene=a&scene=b", "a");
	scene_of_is(NULL, NULL);

	/* the OBS scene "Starting Soon" is the overlay's "starting" scene labelled "Starting Soon" */
	match_is("Starting Soon", "starting", "Starting Soon", true);
	match_is("starting-soon", "starting", "Starting Soon", true);
	match_is("BRB", "brb", "Be Right Back", true);
	match_is("Be Right Back!", "brb", "Be Right Back", true);
	match_is("Starting", "starting", "Starting Soon", true);
	match_is("Starting Soon 2", "starting", "Starting Soon", false);
	match_is("Gameplay", "starting", "Starting Soon", false);
	match_is("Live · In-set", "program", "Live · In-set", true);
	match_is("---", "x", "---", false);
	match_is("", "", "", false);
	match_is(NULL, "a", "a", false);
	match_is("a", NULL, NULL, false);

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("overlay-scenes: all passed\n");
	return 0;
}
