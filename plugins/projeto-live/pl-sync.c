/*
 * Projeto Live: atraso fixo das câmeras e atraso automático do som (veja pl-sync.h).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "pl-sync.h"

#include <string.h>

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include "phone-video.h"

#define SYNC_FILE "sincronia.json"
#define CHECK_SECONDS 2.0f
#define MS_NS 1000000LL

/* Marcas (configurações privadas da fonte de áudio). */
#define AUTO_VALUE "pl_auto_sync_ns" /* último atraso que o Projeto Live colocou */
#define AUTO_OFF "pl_auto_sync_off"  /* a pessoa mudou na mão: não mexer mais */

static volatile long delay_ms = PL_SYNC_DEFAULT_MS;
static float check_timer;
static bool loaded;

/* Entradas de som do PC (mesa, placa, microfone). Saída do PC e fontes com vídeo ficam de fora. */
static const char *audio_inputs[] = {
	"wasapi_input_capture", "coreaudio_input_capture", "pulse_input_capture",
	"alsa_input_capture",   "jack_input_client",       "asio_input_capture",
};

static bool is_audio_input(obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	for (size_t i = 0; i < sizeof(audio_inputs) / sizeof(audio_inputs[0]); i++) {
		if (id && strcmp(id, audio_inputs[i]) == 0) {
			return true;
		}
	}
	return false;
}

static int clamp_ms(int ms)
{
	if (ms < PL_SYNC_MIN_MS) {
		return PL_SYNC_MIN_MS;
	}
	if (ms > PL_SYNC_MAX_MS) {
		return PL_SYNC_MAX_MS;
	}
	return ms;
}

int pl_sync_delay_ms(void)
{
	return (int)os_atomic_load_long(&delay_ms);
}

static void save(void)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "delay_ms", pl_sync_delay_ms());
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path(SYNC_FILE);
	if (path) {
		obs_data_save_json_safe(data, path, "tmp", "bak");
		bfree(path);
	}
	obs_data_release(data);
}

void pl_sync_set_delay_ms(int ms)
{
	ms = clamp_ms(ms);
	if (ms == pl_sync_delay_ms()) {
		return;
	}
	os_atomic_set_long(&delay_ms, ms);
	save();
	blog(LOG_INFO, "[projeto-live] atraso das câmeras: %d ms", ms);
}

/* Põe o atraso `target` (ns) em cada entrada de som que ainda é ajustada sozinha. */
static bool apply_to_source(void *param, obs_source_t *source)
{
	int64_t target = *(int64_t *)param;
	if (!is_audio_input(source)) {
		return true;
	}

	obs_data_t *priv = obs_source_get_private_settings(source);
	if (!obs_data_get_bool(priv, AUTO_OFF)) {
		int64_t current = obs_source_get_sync_offset(source);
		int64_t last = obs_data_get_int(priv, AUTO_VALUE);
		if (current != last) {
			/* Mudou na mão: respeita e não mexe mais nesta entrada. */
			obs_data_set_bool(priv, AUTO_OFF, true);
			blog(LOG_INFO, "[projeto-live] '%s': atraso do som mudado na mão; sem ajuste automático",
			     obs_source_get_name(source));
		} else if (current != target) {
			obs_source_set_sync_offset(source, target);
			obs_data_set_int(priv, AUTO_VALUE, target);
			blog(LOG_INFO,
			     "[projeto-live] '%s': atraso do som ajustado para %lld ms (sincronia com as câmeras)",
			     obs_source_get_name(source), (long long)(target / MS_NS));
		}
	}
	obs_data_release(priv);
	return true;
}

static void tick(void *param, float seconds)
{
	(void)param;
	check_timer += seconds;
	if (check_timer < CHECK_SECONDS) {
		return;
	}
	check_timer = 0.0f;

	int64_t target = pl_phone_video_synced_count() > 0 ? (int64_t)pl_sync_delay_ms() * MS_NS : 0;
	obs_enum_sources(apply_to_source, &target);
}

void pl_sync_load(void)
{
	char *path = obs_module_config_path(SYNC_FILE);
	obs_data_t *data = path ? obs_data_create_from_json_file_safe(path, "bak") : NULL;
	bfree(path);
	if (data) {
		long ms = (long)obs_data_get_int(data, "delay_ms");
		if (ms > 0) {
			os_atomic_set_long(&delay_ms, clamp_ms((int)ms));
		}
		obs_data_release(data);
	}
	obs_add_tick_callback(tick, NULL);
	loaded = true;
}

void pl_sync_unload(void)
{
	if (loaded) {
		obs_remove_tick_callback(tick, NULL);
	}
	loaded = false;
}
