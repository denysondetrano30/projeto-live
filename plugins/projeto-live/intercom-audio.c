/*
 * Projeto Live: fone e microfone do diretor (veja intercom-audio.h).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include <stdio.h>
#include <string.h>

#include <obs-module.h>
#include <util/threading.h>

/* Só a parte de aparelhos de som da miniaudio: sem arquivos, efeitos ou mixer próprios. */
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include "deps/miniaudio/miniaudio.inc" /* o miniaudio.h original, com outro nome (veja o README) */

#include "intercom-audio.h"

#define RATE 16000
#define RING_SAMPLES 16000      /* 1 segundo */
#define CAPTURE_MAX_QUEUE 3200  /* 200 ms: se o microfone adiantar mais que isso, joga fora o velho */
#define CAPTURE_KEEP 640        /* ...e fica só com os 40 ms mais novos */
#define PLAYBACK_PREBUFFER 960  /* junta 60 ms antes de começar a tocar, para não picotar */
#define PLAYBACK_MAX_QUEUE 4800 /* 300 ms: mais que isso, o fone está atrasado; descarta o velho */

struct ring {
	int16_t data[RING_SAMPLES];
	size_t head;
	size_t count;
};

static void ring_write(struct ring *ring, const int16_t *in, size_t samples)
{
	for (size_t i = 0; i < samples; i++) {
		if (ring->count == RING_SAMPLES) {
			ring->head = (ring->head + 1) % RING_SAMPLES;
			ring->count--;
		}
		ring->data[(ring->head + ring->count) % RING_SAMPLES] = in[i];
		ring->count++;
	}
}

static size_t ring_read(struct ring *ring, int16_t *out, size_t samples)
{
	size_t take = samples < ring->count ? samples : ring->count;
	for (size_t i = 0; i < take; i++)
		out[i] = ring->data[(ring->head + i) % RING_SAMPLES];
	ring->head = (ring->head + take) % RING_SAMPLES;
	ring->count -= take;
	return take;
}

static void ring_keep_newest(struct ring *ring, size_t keep)
{
	if (ring->count <= keep)
		return;
	size_t drop = ring->count - keep;
	ring->head = (ring->head + drop) % RING_SAMPLES;
	ring->count = keep;
}

/* `mutex` protege abrir e fechar; `ring_mutex` protege só as filas, que as threads de som da
 * miniaudio usam. As threads de som nunca pegam `mutex` (fechar espera por elas). */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ring_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool context_ready;
static ma_context context;
static bool capture_open;
static bool playback_open;
static ma_device capture;
static ma_device playback;
static struct ring capture_ring;
static struct ring playback_ring;
static bool playback_started;

static void capture_callback(ma_device *device, void *output, const void *input, ma_uint32 frames)
{
	(void)device;
	(void)output;
	pthread_mutex_lock(&ring_mutex);
	ring_write(&capture_ring, input, frames);
	pthread_mutex_unlock(&ring_mutex);
}

static void playback_callback(ma_device *device, void *output, const void *input, ma_uint32 frames)
{
	(void)device;
	(void)input;
	int16_t *out = output;
	size_t got = 0;

	pthread_mutex_lock(&ring_mutex);
	if (!playback_started && playback_ring.count >= PLAYBACK_PREBUFFER)
		playback_started = true;
	if (playback_started)
		got = ring_read(&playback_ring, out, frames);
	if (got < frames && playback_ring.count == 0)
		playback_started = false;
	pthread_mutex_unlock(&ring_mutex);

	if (got < frames)
		memset(out + got, 0, (frames - got) * sizeof(int16_t));
}

static bool ensure_context(void)
{
	if (!context_ready)
		context_ready = ma_context_init(NULL, 0, NULL, &context) == MA_SUCCESS;
	return context_ready;
}

void pl_audio_enum_devices(bool capture_devices, pl_audio_device_cb callback, void *param)
{
	pthread_mutex_lock(&mutex);
	ma_device_info *playback_infos, *capture_infos;
	ma_uint32 playback_count = 0, capture_count = 0;
	if (ensure_context() && ma_context_get_devices(&context, &playback_infos, &playback_count, &capture_infos,
						       &capture_count) == MA_SUCCESS) {
		ma_device_info *infos = capture_devices ? capture_infos : playback_infos;
		ma_uint32 count = capture_devices ? capture_count : playback_count;
		for (ma_uint32 i = 0; i < count; i++)
			callback(param, infos[i].name, infos[i].isDefault != 0);
	}
	pthread_mutex_unlock(&mutex);
}

static void close_locked(void)
{
	if (capture_open)
		ma_device_uninit(&capture);
	if (playback_open)
		ma_device_uninit(&playback);
	capture_open = false;
	playback_open = false;

	pthread_mutex_lock(&ring_mutex);
	capture_ring.count = 0;
	playback_ring.count = 0;
	playback_started = false;
	pthread_mutex_unlock(&ring_mutex);
}

/* Procura o aparelho pelo nome. Devolve false (= usar o padrão) se o nome estiver vazio ou sumiu. */
static bool find_device(ma_device_info *infos, ma_uint32 count, const char *name, ma_device_id *id)
{
	if (!name || !*name)
		return false;
	for (ma_uint32 i = 0; i < count; i++) {
		if (strcmp(infos[i].name, name) == 0) {
			*id = infos[i].id;
			return true;
		}
	}
	return false;
}

static bool open_device(ma_device_type type, const ma_device_id *id, ma_device *device)
{
	ma_device_config config = ma_device_config_init(type);
	config.sampleRate = RATE;
	config.periodSizeInMilliseconds = 10;
	if (type == ma_device_type_capture) {
		config.capture.pDeviceID = id;
		config.capture.format = ma_format_s16;
		config.capture.channels = 1;
		config.dataCallback = capture_callback;
	} else {
		config.playback.pDeviceID = id;
		config.playback.format = ma_format_s16;
		config.playback.channels = 1;
		config.dataCallback = playback_callback;
	}

	if (ma_device_init(&context, &config, device) != MA_SUCCESS)
		return false;
	if (ma_device_start(device) != MA_SUCCESS) {
		ma_device_uninit(device);
		return false;
	}
	return true;
}

bool pl_audio_open(const char *capture_name, const char *playback_name, char *error, size_t error_size)
{
	if (error_size)
		error[0] = 0;

	pthread_mutex_lock(&mutex);
	close_locked();

	if (!ensure_context()) {
		snprintf(error, error_size, "%s", obs_module_text("Intercom.Error.NoAudio"));
		pthread_mutex_unlock(&mutex);
		return false;
	}

	ma_device_info *playback_infos = NULL, *capture_infos = NULL;
	ma_uint32 playback_count = 0, capture_count = 0;
	ma_context_get_devices(&context, &playback_infos, &playback_count, &capture_infos, &capture_count);

	ma_device_id capture_id, playback_id;
	bool capture_found = find_device(capture_infos, capture_count, capture_name, &capture_id);
	bool playback_found = find_device(playback_infos, playback_count, playback_name, &playback_id);

	capture_open = open_device(ma_device_type_capture, capture_found ? &capture_id : NULL, &capture);
	playback_open = open_device(ma_device_type_playback, playback_found ? &playback_id : NULL, &playback);

	if (!capture_open)
		snprintf(error, error_size, "%s", obs_module_text("Intercom.Error.Microphone"));
	else if (!playback_open)
		snprintf(error, error_size, "%s", obs_module_text("Intercom.Error.Headset"));
	else if ((capture_name && *capture_name && !capture_found) ||
		 (playback_name && *playback_name && !playback_found))
		snprintf(error, error_size, "%s", obs_module_text("Intercom.Error.Missing"));

	bool ok = capture_open && playback_open;
	pthread_mutex_unlock(&mutex);
	return ok;
}

void pl_audio_close(void)
{
	pthread_mutex_lock(&mutex);
	close_locked();
	pthread_mutex_unlock(&mutex);
}

bool pl_audio_capture_read(int16_t *out, size_t samples)
{
	bool ok = false;
	pthread_mutex_lock(&ring_mutex);
	if (capture_ring.count > CAPTURE_MAX_QUEUE)
		ring_keep_newest(&capture_ring, CAPTURE_KEEP);
	if (capture_ring.count >= samples)
		ok = ring_read(&capture_ring, out, samples) == samples;
	pthread_mutex_unlock(&ring_mutex);
	return ok;
}

void pl_audio_playback_write(const int16_t *samples_in, size_t samples)
{
	pthread_mutex_lock(&ring_mutex);
	ring_write(&playback_ring, samples_in, samples);
	if (playback_ring.count > PLAYBACK_MAX_QUEUE)
		ring_keep_newest(&playback_ring, PLAYBACK_PREBUFFER);
	pthread_mutex_unlock(&ring_mutex);
}

void pl_audio_shutdown(void)
{
	pthread_mutex_lock(&mutex);
	close_locked();
	if (context_ready)
		ma_context_uninit(&context);
	context_ready = false;
	pthread_mutex_unlock(&mutex);
}
