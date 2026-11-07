/*
 * Projeto Live: destinos extras da transmissão (multistream direto do PC, sem serviço no meio).
 *
 * A transmissão principal continua sendo a de Configurações > Transmissão. Cada destino extra
 * (YouTube, Facebook, Instagram, TikTok ou outro endereço RTMP) é uma saída "rtmp_output" própria,
 * ligada junto com o botão "Iniciar transmissão" e desligada junto com "Parar transmissão".
 *
 * Com a "mesma qualidade da principal", o destino usa o vídeo já comprimido da transmissão
 * principal: o PC comprime uma vez só e manda cópias. Com qualidade própria, o destino ganha um
 * codificador só dele (o melhor que o PC tiver: placa de vídeo primeiro, x264 por último). O áudio
 * é sempre o da principal.
 *
 * Se um destino cair, só ele tenta voltar sozinho (até 20 vezes, a cada 10 s); os outros seguem.
 * Os destinos ficam em destinos.json, na pasta de configuração do plugin.
 *
 * Instagram e TikTok recebem a tela vertical (vertical.c), sempre com um codificador próprio
 * (na "mesma qualidade", com a velocidade da principal).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "multistream.h"
#include "vertical.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/darray.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdio.h>
#include <string.h>

#define RECONNECT_TRIES 20
#define RECONNECT_SECONDS 10
#define DEFAULT_KBPS 6000
#define AUDIO_KBPS 160

struct dest {
	struct pl_dest_info info;

	/* Só na thread da interface. */
	obs_output_t *output;
	obs_service_t *service;
	/* Codificadores próprios (qualidade própria, ou a principal sem vídeo ou sem som). */
	obs_encoder_t *own_video;
	obs_encoder_t *own_audio;

	/* Com state_mutex (os avisos das saídas chegam em outras threads). */
	enum pl_dest_state state;
	char error[PL_DEST_ERROR_MAX];
};

static DARRAY(struct dest *) dests;
static pthread_mutex_t state_mutex;
static bool loaded;
/* A saída de transmissão do OBS só existe depois que ele termina de abrir e até começar a fechar;
 * antes disso, obs_frontend_get_streaming_output() derruba o OBS. */
static bool frontend_ready;

/* ------------------------------------------------------------------------- */
/* Plataformas                                                                */

const char *pl_multistream_default_server(const char *platform)
{
	if (strcmp(platform, "youtube") == 0) {
		return "rtmp://a.rtmp.youtube.com/live2";
	}
	if (strcmp(platform, "facebook") == 0) {
		return "rtmps://live-api-s.facebook.com:443/rtmp/";
	}
	/* Instagram e TikTok dão um endereço próprio a cada conta ou a cada live. */
	return "";
}

bool pl_multistream_platform_vertical(const char *platform)
{
	return strcmp(platform, "instagram") == 0 || strcmp(platform, "tiktok") == 0;
}

/* ------------------------------------------------------------------------- */
/* Estado                                                                     */

static void set_state(struct dest *d, enum pl_dest_state state, const char *error)
{
	pthread_mutex_lock(&state_mutex);
	d->state = state;
	snprintf(d->error, sizeof(d->error), "%s", error ? error : "");
	pthread_mutex_unlock(&state_mutex);
}

static void on_start(void *param, calldata_t *data)
{
	UNUSED_PARAMETER(data);
	struct dest *d = param;
	set_state(d, PL_DEST_LIVE, NULL);
	blog(LOG_INFO, "[projeto-live] destino '%s' no ar", d->info.name);
}

static void on_reconnect(void *param, calldata_t *data)
{
	UNUSED_PARAMETER(data);
	struct dest *d = param;
	set_state(d, PL_DEST_RECONNECTING, NULL);
	blog(LOG_WARNING, "[projeto-live] destino '%s' caiu; tentando de novo", d->info.name);
}

static void on_stop(void *param, calldata_t *data)
{
	struct dest *d = param;
	long long code = calldata_int(data, "code");
	const char *last_error = calldata_string(data, "last_error");

	if (code == OBS_OUTPUT_SUCCESS) {
		set_state(d, d->info.enabled ? PL_DEST_WAITING : PL_DEST_OFF, NULL);
		blog(LOG_INFO, "[projeto-live] destino '%s' parou", d->info.name);
		return;
	}

	const char *reason;
	switch (code) {
	case OBS_OUTPUT_BAD_PATH:
		reason = obs_module_text("Stream.Error.BadPath");
		break;
	case OBS_OUTPUT_CONNECT_FAILED:
		reason = obs_module_text("Stream.Error.Connect");
		break;
	case OBS_OUTPUT_INVALID_STREAM:
		reason = obs_module_text("Stream.Error.Key");
		break;
	case OBS_OUTPUT_DISCONNECTED:
		reason = obs_module_text("Stream.Error.Disconnected");
		break;
	default:
		reason = obs_module_text("Stream.Error.Other");
		break;
	}

	char text[PL_DEST_ERROR_MAX];
	if (last_error && *last_error) {
		snprintf(text, sizeof(text), "%s (%s)", reason, last_error);
	} else {
		snprintf(text, sizeof(text), "%s", reason);
	}
	set_state(d, PL_DEST_ERROR, text);
	blog(LOG_WARNING, "[projeto-live] destino '%s' parou com erro %lld: %s", d->info.name, code,
	     last_error ? last_error : "");
}

/* ------------------------------------------------------------------------- */
/* Ligar e desligar cada destino (thread da interface)                         */

/* Solta tudo o que o destino criou. A saída precisa já estar parada ou é parada à força. */
static void dest_release(struct dest *d)
{
	if (d->output) {
		signal_handler_t *signals = obs_output_get_signal_handler(d->output);
		signal_handler_disconnect(signals, "start", on_start, d);
		signal_handler_disconnect(signals, "reconnect", on_reconnect, d);
		signal_handler_disconnect(signals, "reconnect_success", on_start, d);
		signal_handler_disconnect(signals, "stop", on_stop, d);
		if (obs_output_active(d->output)) {
			obs_output_force_stop(d->output);
		}
		obs_output_release(d->output);
		d->output = NULL;
	}
	obs_service_release(d->service);
	obs_encoder_release(d->own_video);
	obs_encoder_release(d->own_audio);
	d->service = NULL;
	d->own_video = NULL;
	d->own_audio = NULL;
}

/* O melhor codificador H.264 do PC: placa de vídeo (NVIDIA, AMD, Intel, Mac) e, por último, x264. */
const char *pl_best_h264_encoder(void)
{
	static const char *preferred[] = {
		"obs_nvenc_h264_tex", "jim_nvenc", "h264_texture_amf",
		"obs_qsv11_v2",       "obs_qsv11", "com.apple.videotoolbox.videoencoder.ave.avc",
		"obs_x264",
	};

	for (size_t p = 0; p < sizeof(preferred) / sizeof(preferred[0]); p++) {
		const char *id;
		for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
			if (strcmp(id, preferred[p]) == 0) {
				return id;
			}
		}
	}
	return "obs_x264";
}

static obs_encoder_t *create_video_encoder(const struct dest *d, int kbps, video_t *video)
{
	const char *id = pl_best_h264_encoder();
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "rate_control", "CBR");
	obs_data_set_int(settings, "bitrate", kbps);
	obs_data_set_int(settings, "keyint_sec", 2);
	if (strcmp(id, "obs_x264") == 0) {
		obs_data_set_string(settings, "preset", "veryfast");
	}

	char name[PL_DEST_NAME_MAX + 32];
	snprintf(name, sizeof(name), "Projeto Live: video %s", d->info.name);
	obs_encoder_t *encoder = obs_video_encoder_create(id, name, settings, NULL);
	obs_data_release(settings);
	if (encoder) {
		obs_encoder_set_video(encoder, video);
		blog(LOG_INFO, "[projeto-live] destino '%s' com codificador próprio %s, %d kbps", d->info.name, id,
		     kbps);
	}
	return encoder;
}

static obs_encoder_t *create_audio_encoder(const struct dest *d)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "bitrate", AUDIO_KBPS);
	char name[PL_DEST_NAME_MAX + 32];
	snprintf(name, sizeof(name), "Projeto Live: audio %s", d->info.name);
	obs_encoder_t *encoder = obs_audio_encoder_create("ffmpeg_aac", name, settings, 0, NULL);
	obs_data_release(settings);
	if (encoder) {
		obs_encoder_set_audio(encoder, obs_get_audio());
	}
	return encoder;
}

static int encoder_kbps(obs_encoder_t *encoder)
{
	if (!encoder) {
		return 0;
	}
	obs_data_t *settings = obs_encoder_get_settings(encoder);
	int kbps = (int)obs_data_get_int(settings, "bitrate");
	obs_data_release(settings);
	return kbps;
}

static void dest_start(struct dest *d)
{
	dest_release(d);

	if (!d->info.enabled) {
		set_state(d, PL_DEST_OFF, NULL);
		return;
	}
	if (!*d->info.server || !*d->info.key) {
		set_state(d, PL_DEST_ERROR, obs_module_text("Stream.Error.Missing"));
		return;
	}

	obs_output_t *main_output = frontend_ready ? obs_frontend_get_streaming_output() : NULL;
	if (!main_output || !obs_output_active(main_output)) {
		obs_output_release(main_output);
		set_state(d, PL_DEST_WAITING, NULL);
		return;
	}

	bool vertical = pl_multistream_platform_vertical(d->info.platform);
	obs_encoder_t *main_video = obs_output_get_video_encoder(main_output);
	obs_encoder_t *video = (d->info.own_quality || vertical) ? NULL : main_video;
	obs_encoder_t *audio = obs_output_get_audio_encoder(main_output, 0);
	if (!video) {
		video_t *picture = vertical ? pl_vertical_get_video() : obs_get_video();
		if (!picture) {
			obs_output_release(main_output);
			set_state(d, PL_DEST_ERROR, obs_module_text("Stream.Error.NoVertical"));
			return;
		}
		int kbps = d->info.own_quality ? d->info.bitrate_kbps : encoder_kbps(main_video);
		d->own_video = create_video_encoder(d, kbps > 0 ? kbps : DEFAULT_KBPS, picture);
		video = d->own_video;
	}
	if (!audio) {
		d->own_audio = create_audio_encoder(d);
		audio = d->own_audio;
	}
	obs_output_release(main_output);

	if (!video || !audio) {
		dest_release(d);
		set_state(d, PL_DEST_ERROR, obs_module_text("Stream.Error.Encoder"));
		return;
	}

	char name[PL_DEST_NAME_MAX + 32];
	snprintf(name, sizeof(name), "Projeto Live: %s", d->info.name);

	obs_data_t *service_settings = obs_data_create();
	obs_data_set_string(service_settings, "server", d->info.server);
	obs_data_set_string(service_settings, "key", d->info.key);
	d->service = obs_service_create_private("rtmp_custom", name, service_settings);
	obs_data_release(service_settings);

	d->output = obs_output_create("rtmp_output", name, NULL, NULL);
	if (!d->service || !d->output) {
		dest_release(d);
		set_state(d, PL_DEST_ERROR, obs_module_text("Stream.Error.Other"));
		return;
	}

	obs_output_set_service(d->output, d->service);
	obs_output_set_video_encoder(d->output, video);
	obs_output_set_audio_encoder(d->output, audio, 0);
	obs_output_set_reconnect_settings(d->output, RECONNECT_TRIES, RECONNECT_SECONDS);

	signal_handler_t *signals = obs_output_get_signal_handler(d->output);
	signal_handler_connect(signals, "start", on_start, d);
	signal_handler_connect(signals, "reconnect", on_reconnect, d);
	signal_handler_connect(signals, "reconnect_success", on_start, d);
	signal_handler_connect(signals, "stop", on_stop, d);

	set_state(d, PL_DEST_CONNECTING, NULL);
	blog(LOG_INFO, "[projeto-live] ligando o destino '%s' (%s)", d->info.name,
	     d->own_video ? "qualidade própria" : "mesmo vídeo da principal");
	if (!obs_output_start(d->output)) {
		const char *error = obs_output_get_last_error(d->output);
		char text[PL_DEST_ERROR_MAX];
		snprintf(text, sizeof(text), "%s%s%s", obs_module_text("Stream.Error.Connect"),
			 error && *error ? ": " : "", error ? error : "");
		dest_release(d);
		set_state(d, PL_DEST_ERROR, text);
	}
}

/* Para o destino; o aviso "stop" da saída muda o estado quando ela terminar. */
static void dest_stop(struct dest *d)
{
	if (d->output && obs_output_active(d->output)) {
		obs_output_stop(d->output);
	} else {
		dest_release(d);
		set_state(d, d->info.enabled ? PL_DEST_WAITING : PL_DEST_OFF, NULL);
	}
}

static void start_all(void)
{
	for (size_t i = 0; i < dests.num; i++) {
		if (dests.array[i]->info.enabled) {
			dest_start(dests.array[i]);
		}
	}
}

static void stop_all(void)
{
	for (size_t i = 0; i < dests.num; i++) {
		dest_stop(dests.array[i]);
	}
}

static void frontend_event(enum obs_frontend_event event, void *param)
{
	UNUSED_PARAMETER(param);
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		frontend_ready = true;
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		start_all();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		stop_all();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		frontend_ready = false;
		for (size_t i = 0; i < dests.num; i++) {
			dest_release(dests.array[i]);
		}
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------------- */
/* destinos.json                                                              */

static void copy_text(char *out, size_t size, const char *text)
{
	snprintf(out, size, "%s", text ? text : "");
}

static void save_dests(void)
{
	char *dir = obs_module_config_path("");
	char *path = obs_module_config_path("destinos.json");
	if (dir && path) {
		os_mkdirs(dir);
		obs_data_t *data = obs_data_create();
		obs_data_array_t *array = obs_data_array_create();
		for (size_t i = 0; i < dests.num; i++) {
			const struct pl_dest_info *info = &dests.array[i]->info;
			obs_data_t *item = obs_data_create();
			obs_data_set_string(item, "name", info->name);
			obs_data_set_string(item, "platform", info->platform);
			obs_data_set_string(item, "server", info->server);
			obs_data_set_string(item, "key", info->key);
			obs_data_set_bool(item, "enabled", info->enabled);
			obs_data_set_bool(item, "own_quality", info->own_quality);
			obs_data_set_int(item, "bitrate", info->bitrate_kbps);
			obs_data_array_push_back(array, item);
			obs_data_release(item);
		}
		obs_data_set_array(data, "destinations", array);
		obs_data_save_json_safe(data, path, "tmp", "bak");
		obs_data_array_release(array);
		obs_data_release(data);
	}
	bfree(dir);
	bfree(path);
}

static struct dest *new_dest(const struct pl_dest_info *info)
{
	struct dest *d = bzalloc(sizeof(*d));
	d->info = *info;
	d->info.name[sizeof(d->info.name) - 1] = 0;
	d->info.platform[sizeof(d->info.platform) - 1] = 0;
	d->info.server[sizeof(d->info.server) - 1] = 0;
	d->info.key[sizeof(d->info.key) - 1] = 0;
	d->state = info->enabled ? PL_DEST_WAITING : PL_DEST_OFF;
	return d;
}

static void load_dests(void)
{
	char *path = obs_module_config_path("destinos.json");
	obs_data_t *data = path ? obs_data_create_from_json_file_safe(path, "bak") : NULL;
	bfree(path);
	if (!data) {
		return;
	}

	obs_data_array_t *array = obs_data_get_array(data, "destinations");
	size_t count = obs_data_array_count(array);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		struct pl_dest_info info = {0};
		copy_text(info.name, sizeof(info.name), obs_data_get_string(item, "name"));
		copy_text(info.platform, sizeof(info.platform), obs_data_get_string(item, "platform"));
		copy_text(info.server, sizeof(info.server), obs_data_get_string(item, "server"));
		copy_text(info.key, sizeof(info.key), obs_data_get_string(item, "key"));
		info.enabled = obs_data_get_bool(item, "enabled");
		info.own_quality = obs_data_get_bool(item, "own_quality");
		info.bitrate_kbps = (int)obs_data_get_int(item, "bitrate");
		if (!*info.platform) {
			copy_text(info.platform, sizeof(info.platform), "custom");
		}
		struct dest *d = new_dest(&info);
		da_push_back(dests, &d);
		obs_data_release(item);
	}
	obs_data_array_release(array);
	obs_data_release(data);
}

/* ------------------------------------------------------------------------- */
/* Funções para o painel                                                      */

void pl_multistream_load(void)
{
	if (loaded) {
		return;
	}
	pthread_mutex_init(&state_mutex, NULL);
	da_init(dests);
	load_dests();
	obs_frontend_add_event_callback(frontend_event, NULL);
	loaded = true;
}

void pl_multistream_unload(void)
{
	if (!loaded) {
		return;
	}
	obs_frontend_remove_event_callback(frontend_event, NULL);
	for (size_t i = 0; i < dests.num; i++) {
		dest_release(dests.array[i]);
		bfree(dests.array[i]);
	}
	da_free(dests);
	pthread_mutex_destroy(&state_mutex);
	loaded = false;
}

size_t pl_multistream_count(void)
{
	return dests.num;
}

bool pl_multistream_get(size_t index, struct pl_dest_info *info)
{
	if (index >= dests.num) {
		return false;
	}
	*info = dests.array[index]->info;
	return true;
}

void pl_multistream_get_status(size_t index, struct pl_dest_status *status)
{
	memset(status, 0, sizeof(*status));
	if (index >= dests.num) {
		return;
	}
	struct dest *d = dests.array[index];
	pthread_mutex_lock(&state_mutex);
	status->state = d->state;
	snprintf(status->error, sizeof(status->error), "%s", d->error);
	pthread_mutex_unlock(&state_mutex);
	status->total_bytes = d->output ? obs_output_get_total_bytes(d->output) : 0;
}

void pl_multistream_add(const struct pl_dest_info *info)
{
	struct dest *d = new_dest(info);
	da_push_back(dests, &d);
	save_dests();
	if (d->info.enabled && obs_frontend_streaming_active()) {
		dest_start(d);
	}
}

void pl_multistream_update(size_t index, const struct pl_dest_info *info)
{
	if (index >= dests.num) {
		return;
	}
	struct dest *d = dests.array[index];
	bool was_running = d->output != NULL;
	dest_release(d);

	struct dest *fresh = new_dest(info);
	d->info = fresh->info;
	bfree(fresh);
	save_dests();

	if (d->info.enabled && (was_running || obs_frontend_streaming_active())) {
		dest_start(d);
	} else {
		set_state(d, d->info.enabled ? PL_DEST_WAITING : PL_DEST_OFF, NULL);
	}
}

void pl_multistream_remove(size_t index)
{
	if (index >= dests.num) {
		return;
	}
	struct dest *d = dests.array[index];
	dest_release(d);
	da_erase(dests, index);
	bfree(d);
	save_dests();
}

void pl_multistream_set_enabled(size_t index, bool enabled)
{
	if (index >= dests.num) {
		return;
	}
	struct dest *d = dests.array[index];
	if (d->info.enabled == enabled) {
		return;
	}
	d->info.enabled = enabled;
	save_dests();

	if (enabled && obs_frontend_streaming_active()) {
		dest_start(d);
	} else if (!enabled) {
		dest_stop(d);
	} else {
		set_state(d, PL_DEST_WAITING, NULL);
	}
}

bool pl_multistream_main_live(void)
{
	return frontend_ready && obs_frontend_streaming_active();
}

int pl_multistream_main_kbps(void)
{
	if (!frontend_ready) {
		return 0;
	}
	obs_output_t *main_output = obs_frontend_get_streaming_output();
	int kbps = main_output ? encoder_kbps(obs_output_get_video_encoder(main_output)) : 0;
	obs_output_release(main_output);
	return kbps;
}
