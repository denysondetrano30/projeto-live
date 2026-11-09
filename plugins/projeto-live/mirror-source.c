/*
 * Projeto Live: fonte "Telão (projeção)" — espelho ao vivo do telão do app de projeção, com som.
 *
 * Enquanto existir alguma fonte "Telão (projeção)", o OBS pede à projeção o espelho
 * ("MIRROR <porta>", veja projection-link.h) e escuta na porta TCP 9030 um vídeo FLV (H.264 + som)
 * que a projeção manda com o envio RTMP do FreeShow. Quem recebe é uma "Fonte de mídia" do próprio
 * OBS (ffmpeg_source), guardada aqui dentro e compartilhada por todas as fontes "Telão".
 *
 * O som da fonte de mídia é repassado para cada "Telão", que aparece no mixer com volume próprio.
 * A imagem é encaixada numa área fixa de 1920x1080 (o tamanho do telão não muda o layout da cena).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include <obs-module.h>
#include <util/threading.h>

#include <stdio.h>
#include <string.h>

#include "projection-link.h"

#define MIRROR_PORT 9030
#define MIRROR_WIDTH 1920
#define MIRROR_HEIGHT 1080
#define MAX_MIRRORS 16

struct mirror {
	obs_source_t *self;
};

/* Fonte de mídia compartilhada e a lista de "Telão" que recebem o som dela. */
static obs_source_t *receiver;
static struct mirror *mirrors[MAX_MIRRORS];
static int mirror_count;
static pthread_mutex_t mirrors_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *mirror_name(void *unused)
{
	(void)unused;
	return obs_module_text("Mirror.Name");
}

/* Som da fonte de mídia: repassa para cada "Telão". */
static void receiver_audio(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	(void)param;
	(void)source;
	(void)muted;
	struct obs_audio_info info;
	if (!audio || !audio->frames || !obs_get_audio_info(&info)) {
		return;
	}
	struct obs_source_audio out = {0};
	size_t channels = get_audio_channels(info.speakers);
	for (size_t i = 0; i < channels && i < MAX_AV_PLANES; i++) {
		out.data[i] = audio->data[i];
	}
	out.frames = audio->frames;
	out.speakers = info.speakers;
	out.format = AUDIO_FORMAT_FLOAT_PLANAR;
	out.samples_per_sec = info.samples_per_sec;
	out.timestamp = audio->timestamp;

	pthread_mutex_lock(&mirrors_mutex);
	for (int i = 0; i < mirror_count; i++) {
		obs_source_output_audio(mirrors[i]->self, &out);
	}
	pthread_mutex_unlock(&mirrors_mutex);
}

static void create_receiver(void)
{
	char input[96];
	snprintf(input, sizeof(input), "tcp://0.0.0.0:%d?listen=1", MIRROR_PORT);
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", false);
	obs_data_set_string(settings, "input", input);
	obs_data_set_string(settings, "input_format", "flv");
	/* Sem buffer (o OBS lê o que chegar, sem guardar segundos de vídeo) e começo rápido. */
	obs_data_set_int(settings, "buffering_mb", 0);
	obs_data_set_int(settings, "reconnect_delay_sec", 1);
	obs_data_set_bool(settings, "restart_on_activate", false);
	obs_data_set_bool(settings, "close_when_inactive", false);
	obs_data_set_bool(settings, "clear_on_media_end", true);
	obs_data_set_bool(settings, "hw_decode", true);
	obs_data_set_string(settings, "ffmpeg_options", "probesize=65536 analyzeduration=1000000");
	receiver = obs_source_create_private("ffmpeg_source", "Projeto Live: telão (recebe)", settings);
	obs_data_release(settings);
	if (receiver) {
		obs_source_add_audio_capture_callback(receiver, receiver_audio, NULL);
	}
	pl_projection_set_mirror(MIRROR_PORT);
}

static void destroy_receiver(void)
{
	pl_projection_set_mirror(0);
	if (receiver) {
		obs_source_remove_audio_capture_callback(receiver, receiver_audio, NULL);
		obs_source_release(receiver);
		receiver = NULL;
	}
}

static void *mirror_create(obs_data_t *settings, obs_source_t *source)
{
	(void)settings;
	struct mirror *mirror = bzalloc(sizeof(*mirror));
	mirror->self = source;

	pthread_mutex_lock(&mirrors_mutex);
	bool first = mirror_count == 0;
	if (mirror_count < MAX_MIRRORS) {
		mirrors[mirror_count++] = mirror;
	}
	pthread_mutex_unlock(&mirrors_mutex);
	if (first) {
		create_receiver();
	}
	return mirror;
}

static void mirror_destroy(void *data)
{
	struct mirror *mirror = data;
	pthread_mutex_lock(&mirrors_mutex);
	for (int i = 0; i < mirror_count; i++) {
		if (mirrors[i] == mirror) {
			mirrors[i] = mirrors[--mirror_count];
			break;
		}
	}
	bool last = mirror_count == 0;
	pthread_mutex_unlock(&mirrors_mutex);
	if (last) {
		destroy_receiver();
	}
	bfree(mirror);
}

static obs_properties_t *mirror_properties(void *unused)
{
	(void)unused;
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "info", obs_module_text("Mirror.Info"), OBS_TEXT_INFO);
	return props;
}

static uint32_t mirror_width(void *data)
{
	(void)data;
	return MIRROR_WIDTH;
}

static uint32_t mirror_height(void *data)
{
	(void)data;
	return MIRROR_HEIGHT;
}

static void mirror_render(void *data, gs_effect_t *effect)
{
	(void)data;
	(void)effect;
	if (!receiver) {
		return;
	}
	uint32_t width = obs_source_get_width(receiver);
	uint32_t height = obs_source_get_height(receiver);
	if (!width || !height) {
		return;
	}
	/* Encaixa a imagem do telão na área de 1920x1080 sem distorcer. */
	float scale_x = (float)MIRROR_WIDTH / (float)width;
	float scale_y = (float)MIRROR_HEIGHT / (float)height;
	float scale = scale_x < scale_y ? scale_x : scale_y;
	float x = ((float)MIRROR_WIDTH - (float)width * scale) / 2.0f;
	float y = ((float)MIRROR_HEIGHT - (float)height * scale) / 2.0f;

	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	gs_matrix_scale3f(scale, scale, 1.0f);
	obs_source_video_render(receiver);
	gs_matrix_pop();
}

static void mirror_enum_sources(void *data, obs_source_enum_proc_t enum_callback, void *param)
{
	struct mirror *mirror = data;
	if (receiver) {
		enum_callback(mirror->self, receiver, param);
	}
}

struct obs_source_info pl_mirror_source = {
	.id = "projeto_live_mirror",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = mirror_name,
	.create = mirror_create,
	.destroy = mirror_destroy,
	.get_properties = mirror_properties,
	.video_render = mirror_render,
	.get_width = mirror_width,
	.get_height = mirror_height,
	.enum_active_sources = mirror_enum_sources,
	.icon_type = OBS_ICON_TYPE_MEDIA,
};
