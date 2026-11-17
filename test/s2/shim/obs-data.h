/* Test shim: the slice of libobs' obs-data.h that src/http-client.c uses. */
#pragma once
typedef struct obs_data obs_data_t;
obs_data_t *obs_data_create_from_json(const char *json);
