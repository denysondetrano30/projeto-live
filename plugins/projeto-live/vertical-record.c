/*
 * Projeto Live: gravação vertical e preferências da tela vertical.
 *
 * Quando a gravação normal do OBS começa, grava também a tela vertical:
 * "2026-11-07 19-30-00.mp4" ganha o irmão "2026-11-07 19-30-00 (vertical).mp4", na mesma pasta
 * ou na pasta escolhida na aba Multistream. Pausa, volta e para junto com a gravação normal. O
 * vídeo vertical tem codificador próprio (o melhor do PC, 10 Mbps ou o escolhido); o som é o
 * mesmo da gravação normal.
 *
 * As preferências ("gravar o vertical junto", "mostrar ao lado do preview") ficam em
 * vertical.json, na pasta de configuração do plugin.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "vertical-record.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <string.h>

#include "multistream.h"
#include "vertical.h"

#define RECORD_KBPS 10000
#define AUDIO_KBPS 160

static obs_output_t *output;
static obs_encoder_t *video_encoder;
static obs_encoder_t *own_audio;
static obs_data_t *prefs;
static bool loaded;

/* ------------------------------------------------------------------------- */
/* Preferências                                                               */

static void load_prefs(void)
{
	char *path = obs_module_config_path("vertical.json");
	prefs = path ? obs_data_create_from_json_file_safe(path, "bak") : NULL;
	bfree(path);
	if (!prefs) {
		prefs = obs_data_create();
	}
	obs_data_set_default_bool(prefs, "record", true);
	obs_data_set_default_bool(prefs, "embed", true);
	obs_data_set_default_string(prefs, "record_folder", "");
	obs_data_set_default_int(prefs, "record_kbps", RECORD_KBPS);
	obs_data_set_default_bool(prefs, "transition_own", false);
	obs_data_set_default_string(prefs, "transition_id", "fade_transition");
	obs_data_set_default_int(prefs, "transition_ms", 300);
}

bool pl_vertical_pref(const char *key)
{
	return prefs ? obs_data_get_bool(prefs, key) : true;
}

static void save_prefs(void)
{
	char *dir = obs_module_config_path("");
	char *path = obs_module_config_path("vertical.json");
	if (dir && path) {
		os_mkdirs(dir);
		obs_data_save_json_safe(prefs, path, "tmp", "bak");
	}
	bfree(dir);
	bfree(path);
}

void pl_vertical_set_pref(const char *key, bool value)
{
	if (prefs) {
		obs_data_set_bool(prefs, key, value);
		save_prefs();
	}
}

const char *pl_vertical_pref_string(const char *key)
{
	return prefs ? obs_data_get_string(prefs, key) : "";
}

void pl_vertical_set_pref_string(const char *key, const char *value)
{
	if (prefs) {
		obs_data_set_string(prefs, key, value ? value : "");
		save_prefs();
	}
}

int pl_vertical_pref_int(const char *key)
{
	return prefs ? (int)obs_data_get_int(prefs, key) : 0;
}

void pl_vertical_set_pref_int(const char *key, int value)
{
	if (prefs) {
		obs_data_set_int(prefs, key, value);
		save_prefs();
	}
}

obs_data_t *pl_vertical_pref_obj(const char *key)
{
	return prefs ? obs_data_get_obj(prefs, key) : NULL;
}

void pl_vertical_set_pref_obj(const char *key, obs_data_t *value)
{
	if (prefs) {
		obs_data_set_obj(prefs, key, value);
		save_prefs();
	}
}

/* ------------------------------------------------------------------------- */
/* Gravação                                                                   */

static void release_recording(void)
{
	if (output) {
		if (obs_output_active(output)) {
			/* Deixa o arquivo terminar direito (até 5 s) antes de soltar. */
			obs_output_stop(output);
			for (int i = 0; i < 50 && obs_output_active(output); i++) {
				os_sleep_ms(100);
			}
			if (obs_output_active(output)) {
				obs_output_force_stop(output);
			}
		}
		obs_output_release(output);
		output = NULL;
	}
	obs_encoder_release(video_encoder);
	obs_encoder_release(own_audio);
	video_encoder = NULL;
	own_audio = NULL;
}

/*
 * "pasta/2026-11-07 19-30-00.mp4" -> "pasta/2026-11-07 19-30-00 (vertical).mp4"; com `folder`,
 * o arquivo vai para lá: "folder/2026-11-07 19-30-00 (vertical).mp4".
 */
static void vertical_path(const char *path, const char *folder, struct dstr *out)
{
	const char *slash = strrchr(path, '/');
	const char *backslash = strrchr(path, '\\');
	if (backslash > slash) {
		slash = backslash;
	}
	const char *dot = strrchr(path, '.');
	if (!dot || (slash && dot < slash)) {
		dot = path + strlen(path);
	}
	if (folder && *folder) {
		const char *name = slash ? slash + 1 : path;
		dstr_copy(out, folder);
		if (!dstr_is_empty(out) && out->array[out->len - 1] != '/' && out->array[out->len - 1] != '\\') {
			dstr_cat(out, "/");
		}
		dstr_ncat(out, name, (size_t)(dot - name));
		os_mkdirs(folder);
	} else {
		dstr_ncopy(out, path, (size_t)(dot - path));
	}
	dstr_cat(out, " (vertical)");
	dstr_cat(out, dot);
}

static void start_recording(void)
{
	release_recording();
	if (!pl_vertical_pref("record")) {
		return;
	}

	video_t *video = pl_vertical_get_video();
	obs_output_t *main_output = obs_frontend_get_recording_output();
	if (!video || !main_output) {
		obs_output_release(main_output);
		blog(LOG_WARNING, "[projeto-live] gravação vertical: sem tela vertical ou sem gravação");
		return;
	}

	obs_data_t *main_settings = obs_output_get_settings(main_output);
	const char *path = obs_data_get_string(main_settings, "path");
	if (!path || !*path) {
		/* Gravação por "saída FFmpeg personalizada" (usa "url"): ainda não. */
		blog(LOG_WARNING, "[projeto-live] gravação vertical: este tipo de gravação não tem arquivo");
		obs_data_release(main_settings);
		obs_output_release(main_output);
		return;
	}

	struct dstr file = {0};
	vertical_path(path, pl_vertical_pref_string("record_folder"), &file);
	obs_data_release(main_settings);

	obs_encoder_t *audio = obs_output_get_audio_encoder(main_output, 0);
	if (!audio) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_int(settings, "bitrate", AUDIO_KBPS);
		own_audio = obs_audio_encoder_create("ffmpeg_aac", "Projeto Live: audio gravação vertical", settings, 0,
						     NULL);
		obs_data_release(settings);
		if (own_audio) {
			obs_encoder_set_audio(own_audio, obs_get_audio());
		}
		audio = own_audio;
	}
	obs_output_release(main_output);

	const char *id = pl_best_h264_encoder();
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "rate_control", "CBR");
	int kbps = pl_vertical_pref_int("record_kbps");
	obs_data_set_int(settings, "bitrate", kbps > 0 ? kbps : RECORD_KBPS);
	obs_data_set_int(settings, "keyint_sec", 2);
	if (strcmp(id, "obs_x264") == 0) {
		obs_data_set_string(settings, "preset", "veryfast");
	}
	video_encoder = obs_video_encoder_create(id, "Projeto Live: video gravação vertical", settings, NULL);
	obs_data_release(settings);
	if (video_encoder) {
		obs_encoder_set_video(video_encoder, video);
	}

	obs_data_t *output_settings = obs_data_create();
	obs_data_set_string(output_settings, "path", file.array);
	output = obs_output_create("ffmpeg_muxer", "Projeto Live: gravação vertical", output_settings, NULL);
	obs_data_release(output_settings);

	if (!output || !video_encoder || !audio) {
		blog(LOG_WARNING, "[projeto-live] gravação vertical: não foi possível preparar");
		release_recording();
		dstr_free(&file);
		return;
	}

	obs_output_set_video_encoder(output, video_encoder);
	obs_output_set_audio_encoder(output, audio, 0);
	if (obs_output_start(output)) {
		blog(LOG_INFO, "[projeto-live] gravação vertical em '%s' (%s)", file.array, id);
	} else {
		const char *error = obs_output_get_last_error(output);
		blog(LOG_WARNING, "[projeto-live] gravação vertical não começou: %s", error ? error : "");
		release_recording();
	}
	dstr_free(&file);
}

static void frontend_event(enum obs_frontend_event event, void *param)
{
	UNUSED_PARAMETER(param);
	switch (event) {
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		start_recording();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
		if (output && obs_output_active(output)) {
			obs_output_stop(output);
		}
		break;
	case OBS_FRONTEND_EVENT_RECORDING_PAUSED:
		if (output) {
			obs_output_pause(output, true);
		}
		break;
	case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED:
		if (output) {
			obs_output_pause(output, false);
		}
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_EXIT:
		/* A tela vertical vai embora: o arquivo precisa fechar antes. */
		release_recording();
		break;
	default:
		break;
	}
}

void pl_vertical_record_load(void)
{
	if (loaded) {
		return;
	}
	load_prefs();
	obs_frontend_add_event_callback(frontend_event, NULL);
	loaded = true;
}

void pl_vertical_record_unload(void)
{
	if (!loaded) {
		return;
	}
	obs_frontend_remove_event_callback(frontend_event, NULL);
	release_recording();
	obs_data_release(prefs);
	prefs = NULL;
	loaded = false;
}
