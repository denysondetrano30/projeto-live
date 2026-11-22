/*
 * somun live: a transmissão (multistream direto do PC, sem serviço no meio).
 *
 * Parte G (22/11): a nossa lista de destinos é a DONA da transmissão. O "Transmitir" da tela
 * Operar chama pl_multistream_start(), que liga todos os destinos marcados; não existe principal
 * nem secundário, e a transmissão do próprio OBS (Configurações > Transmissão) não é usada (fica
 * só para o Modo OBS). Uma live só vertical é um destino vertical e pronto.
 *
 * Cada destino (YouTube, Facebook, Instagram, TikTok ou outro endereço RTMP) é uma saída
 * "rtmp_output" própria. Os horizontais dividem UM codificador (imagem do Programa) e os verticais
 * outro (tela vertical, vertical.c, 1080x1920); o áudio é um só para todos. Assim 3 destinos
 * horizontais custam quase o mesmo que 1. A qualidade é a da aba Saída do OBS (taxa de bits,
 * codificador e predefinição), a mesma para horizontal e vertical.
 *
 * Se um destino cair, só ele tenta voltar sozinho (até 20 vezes, a cada 10 s); os outros seguem.
 * Os destinos ficam em destinos.json, na pasta de configuração do plugin. Na primeira
 * vez, a transmissão que estava configurada no OBS vira um destino horizontal da lista.
 *
 * A lista é editada nas Configurações (stream-dock.cpp) e salva inteira com pl_multistream_set_all.
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
#include <util/config-file.h>
#include <util/threading.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#define RECONNECT_TRIES 20
#define RECONNECT_SECONDS 10
#define DEFAULT_KBPS 6000

struct dest {
	struct pl_dest_info info;

	/* Só na thread da interface. */
	obs_output_t *output;
	obs_service_t *service;

	/* Com state_mutex (os avisos das saídas chegam em outras threads). */
	enum pl_dest_state state;
	char error[PL_DEST_ERROR_MAX];
};

static DARRAY(struct dest *) dests;
static pthread_mutex_t state_mutex;
static bool loaded;
/* O OBS terminou de abrir (a transmissão antiga dele já pode ser lida para a mudança). */
static bool frontend_ready;

/* A transmissão (só na thread da interface). */
static bool active;
static uint64_t started_ns;
/* destinos.json ainda não tinha a qualidade: na primeira vez, traz a transmissão antiga do OBS. */
static bool needs_migration;
/* Codificadores divididos: um horizontal, um vertical e um de áudio para todos. */
static obs_encoder_t *shared_horizontal;
static obs_encoder_t *shared_vertical;
static obs_encoder_t *shared_audio;

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
	d->service = NULL;
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

void pl_multistream_enum_encoders(pl_encoder_cb callback, void *param)
{
	const char *id;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		const char *codec = obs_get_encoder_codec(id);
		uint32_t caps = obs_get_encoder_caps(id);
		if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO || !codec || strcmp(codec, "h264") != 0 ||
		    (caps & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL))) {
			continue;
		}
		const char *name = obs_encoder_get_display_name(id);
		callback(param, id, name ? name : id);
	}
}

/* ------------------------------------------------------------------------- */
/* Qualidade: a mesma da aba Saída do OBS (taxa de bits, codificador e predefinição)              */

static bool encoder_exists(const char *wanted)
{
	const char *id;
	for (size_t i = 0; wanted && *wanted && obs_enum_encoder_types(i, &id); i++) {
		if (strcmp(id, wanted) == 0) {
			return true;
		}
	}
	return false;
}

static bool is_h264(const char *id)
{
	const char *codec = id ? obs_get_encoder_codec(id) : NULL;
	return codec && strcmp(codec, "h264") == 0;
}

/* Os nomes curtos do modo Simples ("x264", "nvenc", "qsv", "amd"...) viram o codificador de verdade. */
static const char *simple_encoder_id(const char *name)
{
	if (!name) {
		return NULL;
	}
	if (strcmp(name, "x264") == 0 || strcmp(name, "x264_lowcpu") == 0) {
		return "obs_x264";
	}
	if (strcmp(name, "nvenc") == 0) {
		static const char *nvenc[] = {"obs_nvenc_h264_tex", "jim_nvenc", "ffmpeg_nvenc"};
		for (size_t i = 0; i < sizeof(nvenc) / sizeof(nvenc[0]); i++) {
			if (encoder_exists(nvenc[i])) {
				return nvenc[i];
			}
		}
	}
	if (strcmp(name, "qsv") == 0) {
		return encoder_exists("obs_qsv11_v2") ? "obs_qsv11_v2" : "obs_qsv11";
	}
	if (strcmp(name, "amd") == 0) {
		return "h264_texture_amf";
	}
	return NULL;
}

/*
 * Lê a qualidade da transmissão na aba Saída do OBS. Devolve as configurações do codificador de
 * vídeo (para soltar com obs_data_release) e preenche o codificador, a velocidade do áudio e a
 * faixa de áudio. HEVC e AV1 não servem para todas as plataformas (Instagram, Facebook): nesses
 * casos usa o melhor H.264 do PC com a mesma taxa de bits.
 */
static obs_data_t *obs_quality(const char **id_out, int *audio_kbps, size_t *audio_track)
{
	config_t *profile = obs_frontend_get_profile_config();
	const char *mode = profile ? config_get_string(profile, "Output", "Mode") : NULL;
	bool advanced = mode && strcmp(mode, "Advanced") == 0;
	const char *id = NULL;
	obs_data_t *settings = NULL;
	*audio_kbps = 160;
	*audio_track = 0;

	if (advanced) {
		id = config_get_string(profile, "AdvOut", "Encoder");
		char *dir = obs_frontend_get_current_profile_path();
		if (dir) {
			char path[1024];
			snprintf(path, sizeof(path), "%s/streamEncoder.json", dir);
			settings = obs_data_create_from_json_file(path);
			bfree(dir);
		}
		if (!settings) {
			settings = obs_data_create();
		}
		if (id && *id) {
			/* Os padrões do codificador (ex.: 2500 kbps) valem para o que a pessoa não mudou. */
			obs_data_t *defaults = obs_encoder_defaults(id);
			if (defaults) {
				obs_data_apply(defaults, settings);
				obs_data_release(settings);
				settings = defaults;
			}
		}
		int track = (int)config_get_int(profile, "AdvOut", "TrackIndex");
		if (track < 1 || track > 6) {
			track = 1;
		}
		*audio_track = (size_t)(track - 1);
		char key[32];
		snprintf(key, sizeof(key), "Track%dBitrate", track);
		int kbps = (int)config_get_int(profile, "AdvOut", key);
		if (kbps > 0) {
			*audio_kbps = kbps;
		}
	} else {
		settings = obs_data_create();
		int kbps = profile ? (int)config_get_int(profile, "SimpleOutput", "VBitrate") : 0;
		obs_data_set_int(settings, "bitrate", kbps > 0 ? kbps : 2500);
		obs_data_set_string(settings, "rate_control", "CBR");
		id = simple_encoder_id(profile ? config_get_string(profile, "SimpleOutput", "StreamEncoder") : NULL);
		const char *preset = profile ? config_get_string(profile, "SimpleOutput", "Preset") : NULL;
		if (id && strcmp(id, "obs_x264") == 0) {
			obs_data_set_string(settings, "preset", preset && *preset ? preset : "veryfast");
		}
		const char *nvenc_preset = profile ? config_get_string(profile, "SimpleOutput", "NVENCPreset2") : NULL;
		if (id && strstr(id, "nvenc") && nvenc_preset && *nvenc_preset) {
			obs_data_set_string(settings, "preset2", nvenc_preset);
		}
		int audio = profile ? (int)config_get_int(profile, "SimpleOutput", "ABitrate") : 0;
		if (audio > 0) {
			*audio_kbps = audio;
		}
	}

	if (!encoder_exists(id) || !is_h264(id)) {
		/* Codificador que não existe ou não é H.264: o melhor H.264 com a mesma taxa de bits. */
		int kbps = (int)obs_data_get_int(settings, "bitrate");
		obs_data_release(settings);
		id = pl_best_h264_encoder();
		settings = obs_data_create();
		obs_data_set_int(settings, "bitrate", kbps > 0 ? kbps : DEFAULT_KBPS);
		obs_data_set_string(settings, "rate_control", "CBR");
		if (strcmp(id, "obs_x264") == 0) {
			obs_data_set_string(settings, "preset", "veryfast");
		}
	}
	/* As plataformas pedem um quadro-chave a cada 2 s (sem isso o Instagram recusa). */
	if (obs_data_get_int(settings, "keyint_sec") <= 0 || obs_data_get_int(settings, "keyint_sec") > 4) {
		obs_data_set_int(settings, "keyint_sec", 2);
	}
	*id_out = id;
	return settings;
}

static obs_encoder_t *create_video_encoder(const char *label, video_t *video)
{
	const char *id = NULL;
	int audio_kbps;
	size_t audio_track;
	obs_data_t *settings = obs_quality(&id, &audio_kbps, &audio_track);
	char name[64];
	snprintf(name, sizeof(name), "somun live: video %s", label);
	obs_encoder_t *encoder = obs_video_encoder_create(id, name, settings, NULL);
	if (encoder) {
		obs_encoder_set_video(encoder, video);
		blog(LOG_INFO, "[projeto-live] codificador %s (%s), %d kbps", label, id,
		     (int)obs_data_get_int(settings, "bitrate"));
	}
	obs_data_release(settings);
	return encoder;
}

static obs_encoder_t *create_audio_encoder(void)
{
	const char *id = NULL;
	int audio_kbps;
	size_t audio_track;
	obs_data_t *video_settings = obs_quality(&id, &audio_kbps, &audio_track);
	obs_data_release(video_settings);

	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "bitrate", audio_kbps);
	obs_encoder_t *encoder =
		obs_audio_encoder_create("ffmpeg_aac", "somun live: audio", settings, audio_track, NULL);
	obs_data_release(settings);
	if (encoder) {
		obs_encoder_set_audio(encoder, obs_get_audio());
	}
	return encoder;
}

int pl_multistream_quality_kbps(void)
{
	const char *id = NULL;
	int audio_kbps;
	size_t audio_track;
	obs_data_t *settings = obs_quality(&id, &audio_kbps, &audio_track);
	int kbps = (int)obs_data_get_int(settings, "bitrate");
	obs_data_release(settings);
	return kbps;
}

/* Solta os codificadores divididos (as saídas que ainda os usam seguram a parte delas). */
static void release_shared_encoders(void)
{
	obs_encoder_release(shared_horizontal);
	obs_encoder_release(shared_vertical);
	obs_encoder_release(shared_audio);
	shared_horizontal = NULL;
	shared_vertical = NULL;
	shared_audio = NULL;
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
	if (!active) {
		set_state(d, PL_DEST_WAITING, NULL);
		return;
	}

	/* Os codificadores divididos nascem com o primeiro destino que precisa deles. */
	bool vertical = d->info.vertical;
	obs_encoder_t **shared = vertical ? &shared_vertical : &shared_horizontal;
	if (!*shared) {
		video_t *picture = vertical ? pl_vertical_get_video() : obs_get_video();
		if (!picture) {
			set_state(d, PL_DEST_ERROR, obs_module_text("Stream.Error.NoVertical"));
			return;
		}
		*shared = create_video_encoder(vertical ? "vertical" : "horizontal", picture);
	}
	if (!shared_audio) {
		shared_audio = create_audio_encoder();
	}
	if (!*shared || !shared_audio) {
		set_state(d, PL_DEST_ERROR, obs_module_text("Stream.Error.Encoder"));
		return;
	}

	char name[PL_DEST_NAME_MAX + 32];
	snprintf(name, sizeof(name), "somun live: %s", d->info.name);

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
	obs_output_set_video_encoder(d->output, *shared);
	obs_output_set_audio_encoder(d->output, shared_audio, 0);
	obs_output_set_reconnect_settings(d->output, RECONNECT_TRIES, RECONNECT_SECONDS);

	signal_handler_t *signals = obs_output_get_signal_handler(d->output);
	signal_handler_connect(signals, "start", on_start, d);
	signal_handler_connect(signals, "reconnect", on_reconnect, d);
	signal_handler_connect(signals, "reconnect_success", on_start, d);
	signal_handler_connect(signals, "stop", on_stop, d);

	set_state(d, PL_DEST_CONNECTING, NULL);
	blog(LOG_INFO, "[projeto-live] ligando o destino '%s' (%s)", d->info.name,
	     vertical ? "vertical" : "horizontal");
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

static void migrate_obs_stream(void);

/* A transmissão do OBS (Configurações > Transmissão) não liga mais os destinos: só o nosso
 * "Transmitir" (pl_multistream_start). Ela continua existindo sozinha no Modo OBS. */
static void frontend_event(enum obs_frontend_event event, void *param)
{
	UNUSED_PARAMETER(param);
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		frontend_ready = true;
		if (needs_migration) {
			migrate_obs_stream();
		}
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		frontend_ready = false;
		active = false;
		for (size_t i = 0; i < dests.num; i++) {
			dest_release(dests.array[i]);
		}
		release_shared_encoders();
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
			obs_data_set_string(item, "id", info->id);
			obs_data_set_bool(item, "vertical", info->vertical);
			obs_data_set_string(item, "encoder", info->encoder);
			obs_data_set_int(item, "width", info->width);
			obs_data_set_int(item, "height", info->height);
			obs_data_set_string(item, "key_date", info->key_date);
			obs_data_array_push_back(array, item);
			obs_data_release(item);
		}
		obs_data_set_array(data, "destinations", array);
		obs_data_set_int(data, "owner_version", 2);
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
	d->info.id[sizeof(d->info.id) - 1] = 0;
	d->info.encoder[sizeof(d->info.encoder) - 1] = 0;
	d->info.key_date[sizeof(d->info.key_date) - 1] = 0;
	if (!*d->info.id) {
		char *uuid = os_generate_uuid();
		copy_text(d->info.id, sizeof(d->info.id), uuid);
		bfree(uuid);
	}
	d->state = info->enabled ? PL_DEST_WAITING : PL_DEST_OFF;
	return d;
}

static void load_dests(void)
{
	char *path = obs_module_config_path("destinos.json");
	obs_data_t *data = path ? obs_data_create_from_json_file_safe(path, "bak") : NULL;
	bfree(path);
	if (!data) {
		needs_migration = true;
		return;
	}

	/* Sem a marca da parte G ("owner_version"; no G1 era "quality_kbps"), o arquivo é de antes:
	 * traz a transmissão antiga do OBS. */
	needs_migration = !obs_data_has_user_value(data, "owner_version") &&
			  !obs_data_has_user_value(data, "quality_kbps");

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
		copy_text(info.id, sizeof(info.id), obs_data_get_string(item, "id"));
		copy_text(info.encoder, sizeof(info.encoder), obs_data_get_string(item, "encoder"));
		/* Destinos de antes da aba Multistream não tinham "vertical": vale a plataforma. */
		info.vertical = obs_data_has_user_value(item, "vertical")
					? obs_data_get_bool(item, "vertical")
					: pl_multistream_platform_vertical(info.platform);
		info.width = (int)obs_data_get_int(item, "width");
		info.height = (int)obs_data_get_int(item, "height");
		copy_text(info.key_date, sizeof(info.key_date), obs_data_get_string(item, "key_date"));
		struct dest *d = new_dest(&info);
		da_push_back(dests, &d);
		obs_data_release(item);
	}
	obs_data_array_release(array);
	obs_data_release(data);
}

static void today_text(char *out, size_t size)
{
	time_t now = time(NULL);
	struct tm local;
#ifdef _WIN32
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif
	strftime(out, size, "%Y-%m-%d", &local);
}

static const char *guess_platform(const char *url)
{
	if (strstr(url, "youtube.com")) {
		return "youtube";
	}
	if (strstr(url, "facebook.com") || strstr(url, "fbcdn.net")) {
		return "facebook";
	}
	if (strstr(url, "instagram.com")) {
		return "instagram";
	}
	if (strstr(url, "tiktok")) {
		return "tiktok";
	}
	return "custom";
}

/*
 * Na primeira vez depois da parte G: a transmissão que estava em Configurações > Transmissão vira
 * um destino horizontal no topo da lista (para nada parar de funcionar).
 * Só endereços RTMP/RTMPS entram (SRT, WHIP etc. ficam só no Modo OBS).
 */
static void migrate_obs_stream(void)
{
	needs_migration = false;

	obs_service_t *service = obs_frontend_get_streaming_service();
	const char *url = service ? obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL) : NULL;
	const char *key = service ? obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_STREAM_KEY) : NULL;
	bool usable = url && key && *key && strncmp(url, "rtmp", 4) == 0;

	for (size_t i = 0; usable && i < dests.num; i++) {
		if (strcmp(dests.array[i]->info.server, url) == 0 && strcmp(dests.array[i]->info.key, key) == 0) {
			usable = false; /* Já está na lista. */
		}
	}

	if (usable) {
		struct pl_dest_info info = {0};
		const char *platform = guess_platform(url);
		copy_text(info.platform, sizeof(info.platform), platform);
		copy_text(info.name, sizeof(info.name),
			  strcmp(platform, "youtube") == 0    ? "YouTube"
			  : strcmp(platform, "facebook") == 0 ? "Facebook"
							      : obs_module_text("Stream.MigratedName"));
		copy_text(info.server, sizeof(info.server), url);
		copy_text(info.key, sizeof(info.key), key);
		info.enabled = true;
		info.vertical = false;
		today_text(info.key_date, sizeof(info.key_date));
		struct dest *d = new_dest(&info);
		da_insert(dests, 0, &d);
		blog(LOG_INFO, "[projeto-live] a transmissão do OBS virou o destino '%s'", info.name);
	}
	save_dests();
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
	release_shared_encoders();
	active = false;
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

static bool same_info(const struct pl_dest_info *a, const struct pl_dest_info *b)
{
	return strcmp(a->name, b->name) == 0 && strcmp(a->platform, b->platform) == 0 &&
	       strcmp(a->server, b->server) == 0 && strcmp(a->key, b->key) == 0 && a->enabled == b->enabled &&
	       a->vertical == b->vertical;
}

void pl_multistream_set_all(const struct pl_dest_info *infos, size_t count)
{
	DARRAY(struct dest *) fresh;
	da_init(fresh);
	char today[16];
	today_text(today, sizeof(today));

	for (size_t i = 0; i < count; i++) {
		struct dest *d = NULL;
		for (size_t j = 0; j < dests.num; j++) {
			if (*infos[i].id && strcmp(dests.array[j]->info.id, infos[i].id) == 0) {
				d = dests.array[j];
				da_erase(dests, j);
				break;
			}
		}

		/* A data da chave ("chave de hoje" ou "chave velha") muda quando a chave muda. */
		struct pl_dest_info info = infos[i];
		bool new_key = d ? strcmp(d->info.key, info.key) != 0 : !*info.key_date;
		if (new_key && *info.key) {
			copy_text(info.key_date, sizeof(info.key_date), today);
		}

		if (d && same_info(&d->info, &info)) {
			/* Nada mudou: continua como está (no ar, se estava). */
		} else if (d) {
			dest_release(d);
			struct dest *changed = new_dest(&info);
			d->info = changed->info;
			bfree(changed);
			if (d->info.enabled && active) {
				dest_start(d);
			} else {
				set_state(d, d->info.enabled ? PL_DEST_WAITING : PL_DEST_OFF, NULL);
			}
		} else {
			d = new_dest(&info);
			if (d->info.enabled && active) {
				dest_start(d);
			}
		}
		da_push_back(fresh, &d);
	}

	/* Os que sobraram foram removidos. */
	for (size_t j = 0; j < dests.num; j++) {
		dest_release(dests.array[j]);
		bfree(dests.array[j]);
	}
	da_free(dests);
	dests.da = fresh.da;
	save_dests();
}

/* ------------------------------------------------------------------------- */
/* A transmissão                                                              */

int pl_multistream_start(void)
{
	size_t enabled = 0;
	for (size_t i = 0; i < dests.num; i++) {
		if (dests.array[i]->info.enabled) {
			enabled++;
		}
	}
	if (!enabled || active) {
		return (int)enabled;
	}

	/* Restos da live anterior saem antes; os codificadores nascem de novo com a qualidade de agora. */
	for (size_t i = 0; i < dests.num; i++) {
		dest_release(dests.array[i]);
	}
	release_shared_encoders();

	active = true;
	started_ns = os_gettime_ns();
	blog(LOG_INFO, "[projeto-live] Transmitir: %zu destino(s)", enabled);
	start_all();
	return (int)enabled;
}

void pl_multistream_stop(void)
{
	if (!active) {
		return;
	}
	active = false;
	blog(LOG_INFO, "[projeto-live] Parar a transmissão");
	stop_all();
}

bool pl_multistream_active(void)
{
	return active;
}

uint64_t pl_multistream_seconds(void)
{
	return active ? (os_gettime_ns() - started_ns) / 1000000000ULL : 0;
}
