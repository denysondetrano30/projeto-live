/*
 * Projeto Live: fonte "Telão (projeção)" — espelho ao vivo do telão do app de projeção, com som.
 *
 * Dois caminhos (17/11):
 *
 * 1. NDI (principal): o OBS pede "NDI on" à projeção (veja projection-link.h), que passa a mandar a
 *    saída do telão por NDI com o nome "Projeto Live - Telão". Uma linha de execução daqui acha essa
 *    fonte na rede, recebe os quadros (BGRA) e o som, e a fonte desenha o último quadro direto, sem
 *    fila: atraso de poucos quadros. A biblioteca do NDI (Processing.NDI.Lib.x64.dll) é carregada na
 *    hora: a que o instalador põe ao lado do plugin ou a do NDI Runtime instalado no computador.
 *
 * 2. Modo antigo (reserva): se o NDI não chegar em alguns segundos (biblioteca faltando, rede
 *    bloqueando, projeção antiga), o OBS pede o espelho em FLV ("MIRROR <porta>") e escuta na porta
 *    TCP 9030 com uma "Fonte de mídia" do próprio OBS (ffmpeg_source), como antes. Quando o NDI volta,
 *    o modo antigo é desligado sozinho.
 *
 * O som é repassado para cada "Telão", que aparece no mixer com volume próprio. A imagem é encaixada
 * numa área fixa de 1920x1080 (o tamanho do telão não muda o layout da cena).
 *
 * NDI® é marca registrada da Vizrt NDI AB (https://ndi.video).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "projection-link.h"

#define MIRROR_PORT 9030
#define MIRROR_WIDTH 1920
#define MIRROR_HEIGHT 1080
#define MAX_MIRRORS 16

#define NS_PER_SEC 1000000000ULL
/* Quadro do NDI mais novo que isto = NDI funcionando. */
#define NDI_FRESH_NS (1500ULL * 1000000ULL)
/* Sem NDI por este tempo: liga o modo antigo. */
#define NDI_FALLBACK_NS (6ULL * NS_PER_SEC)
/* NDI de volta e estável por este tempo: desliga o modo antigo. */
#define NDI_RECOVER_NS (3ULL * NS_PER_SEC)
/* Conectado mas sem imagem por este tempo: procura a fonte de novo. */
#define NDI_RECONNECT_NS (4ULL * NS_PER_SEC)

#define NDI_NAME_TAG "(Projeto Live - Tel"

struct mirror {
	obs_source_t *self;
};

static struct mirror *mirrors[MAX_MIRRORS];
static int mirror_count;
static pthread_mutex_t mirrors_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Fonte de mídia do modo antigo (NULL quando desligado). Protegida por receiver_mutex. */
static obs_source_t *receiver;
static pthread_mutex_t receiver_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------------- */
/* NDI: só o pedaço da biblioteca que usamos (mesma forma do SDK do NDI 5/6)  */

typedef struct {
	const char *p_ndi_name;
	const char *p_url_address;
} ndi_source_t;

typedef struct {
	bool show_local_sources;
	const char *p_groups;
	const char *p_extra_ips;
} ndi_find_create_t;

typedef struct {
	ndi_source_t source_to_connect_to;
	int color_format;
	int bandwidth;
	bool allow_video_fields;
	const char *p_ndi_recv_name;
} ndi_recv_create_v3_t;

typedef struct {
	int xres;
	int yres;
	uint32_t fourcc;
	int frame_rate_N;
	int frame_rate_D;
	float picture_aspect_ratio;
	int frame_format_type;
	int64_t timecode;
	uint8_t *p_data;
	int line_stride_in_bytes;
	const char *p_metadata;
	int64_t timestamp;
} ndi_video_frame_v2_t;

typedef struct {
	int sample_rate;
	int no_channels;
	int no_samples;
	int64_t timecode;
	uint32_t fourcc;
	uint8_t *p_data;
	int channel_stride_in_bytes;
	const char *p_metadata;
	int64_t timestamp;
} ndi_audio_frame_v3_t;

#define NDI_FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define NDI_FOURCC_BGRA NDI_FOURCC('B', 'G', 'R', 'A')
#define NDI_FOURCC_BGRX NDI_FOURCC('B', 'G', 'R', 'X')
#define NDI_FOURCC_FLTP NDI_FOURCC('F', 'L', 'T', 'p')

#define NDI_FRAME_VIDEO 1
#define NDI_FRAME_AUDIO 2
#define NDI_COLOR_BGRX_BGRA 0
#define NDI_BANDWIDTH_HIGHEST 100

static struct {
	void *lib;
	bool (*initialize)(void);
	void *(*find_create_v2)(const ndi_find_create_t *);
	void (*find_destroy)(void *);
	bool (*find_wait_for_sources)(void *, uint32_t);
	const ndi_source_t *(*find_get_current_sources)(void *, uint32_t *);
	void *(*recv_create_v3)(const ndi_recv_create_v3_t *);
	void (*recv_destroy)(void *);
	int (*recv_capture_v3)(void *, ndi_video_frame_v2_t *, ndi_audio_frame_v3_t *, void *, uint32_t);
	void (*recv_free_video_v2)(void *, const ndi_video_frame_v2_t *);
	void (*recv_free_audio_v3)(void *, const ndi_audio_frame_v3_t *);
} ndi;
static bool ndi_tried;
static bool ndi_ok;

static void *try_open(const char *dir, const char *file)
{
	struct dstr path = {0};
	if (dir && *dir) {
		dstr_copy(&path, dir);
		dstr_replace(&path, "\\", "/");
		if (path.len && path.array[path.len - 1] != '/') {
			dstr_cat_ch(&path, '/');
		}
	}
	dstr_cat(&path, file);
	void *lib = os_dlopen(path.array);
	if (lib) {
		blog(LOG_INFO, "[projeto-live] telão: NDI carregado de %s", path.array);
	}
	dstr_free(&path);
	return lib;
}

static void *open_ndi_library(void)
{
#ifdef _WIN32
	const char *file = "Processing.NDI.Lib.x64.dll";
#elif defined(__APPLE__)
	const char *file = "libndi.dylib";
#else
	const char *file = "libndi.so.6";
#endif
	/* 1. A que o instalador põe ao lado do plugin. */
	void *lib = NULL;
	const char *binary = obs_get_module_binary_path(obs_current_module());
	if (binary) {
		struct dstr dir = {0};
		dstr_copy(&dir, binary);
		dstr_replace(&dir, "\\", "/");
		const char *slash = strrchr(dir.array, '/');
		if (slash) {
			dstr_resize(&dir, (size_t)(slash - dir.array));
			lib = try_open(dir.array, file);
		}
		dstr_free(&dir);
	}
	/* 2. A do NDI Runtime / NDI Tools instalado no computador. */
	if (!lib) {
		lib = try_open(getenv("NDI_RUNTIME_DIR_V6"), file);
	}
	if (!lib) {
		lib = try_open(getenv("NDI_RUNTIME_DIR_V5"), file);
	}
#ifdef __APPLE__
	if (!lib) {
		lib = try_open("/usr/local/lib", file);
	}
#endif
	if (!lib) {
		lib = try_open(NULL, file);
	}
#if !defined(_WIN32) && !defined(__APPLE__)
	if (!lib) {
		lib = try_open(NULL, "libndi.so.5");
	}
#endif
	return lib;
}

#define NDI_SYM(field, name)                                      \
	do {                                                      \
		ndi.field = os_dlsym(ndi.lib, name);              \
		if (!ndi.field) {                                 \
			missing = name;                           \
		}                                                 \
	} while (0)

static bool load_ndi(void)
{
	if (ndi_tried) {
		return ndi_ok;
	}
	ndi_tried = true;
	ndi.lib = open_ndi_library();
	if (!ndi.lib) {
		blog(LOG_WARNING, "[projeto-live] telão: biblioteca do NDI não encontrada; usando o modo antigo");
		return false;
	}
	const char *missing = NULL;
	NDI_SYM(initialize, "NDIlib_initialize");
	NDI_SYM(find_create_v2, "NDIlib_find_create_v2");
	NDI_SYM(find_destroy, "NDIlib_find_destroy");
	NDI_SYM(find_wait_for_sources, "NDIlib_find_wait_for_sources");
	NDI_SYM(find_get_current_sources, "NDIlib_find_get_current_sources");
	NDI_SYM(recv_create_v3, "NDIlib_recv_create_v3");
	NDI_SYM(recv_destroy, "NDIlib_recv_destroy");
	NDI_SYM(recv_capture_v3, "NDIlib_recv_capture_v3");
	NDI_SYM(recv_free_video_v2, "NDIlib_recv_free_video_v2");
	NDI_SYM(recv_free_audio_v3, "NDIlib_recv_free_audio_v3");
	if (missing) {
		blog(LOG_WARNING, "[projeto-live] telão: NDI sem a função %s; usando o modo antigo", missing);
		return false;
	}
	if (!ndi.initialize()) {
		blog(LOG_WARNING, "[projeto-live] telão: o NDI não iniciou (processador sem suporte?); usando o modo antigo");
		return false;
	}
	ndi_ok = true;
	return true;
}

/* ------------------------------------------------------------------------- */
/* Último quadro do NDI (escrito pela linha do NDI, desenhado pela fonte)      */

static pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *frame_data;
static size_t frame_capacity;
static uint32_t frame_width;
static uint32_t frame_height;
static bool frame_bgrx;
static uint64_t frame_seq;
static volatile uint64_t last_video_ns;

/* Só na linha de desenho. */
static gs_texture_t *frame_texture;
static uint64_t texture_seq;
static uint32_t texture_width;
static uint32_t texture_height;
static bool texture_bgrx;

static void store_frame(const ndi_video_frame_v2_t *video)
{
	if (video->xres <= 0 || video->yres <= 0 || !video->p_data ||
	    (video->fourcc != NDI_FOURCC_BGRA && video->fourcc != NDI_FOURCC_BGRX)) {
		return;
	}
	uint32_t width = (uint32_t)video->xres;
	uint32_t height = (uint32_t)video->yres;
	size_t row = (size_t)width * 4;
	size_t size = row * height;
	pthread_mutex_lock(&frame_mutex);
	if (frame_capacity < size) {
		frame_data = brealloc(frame_data, size);
		frame_capacity = size;
	}
	int stride = video->line_stride_in_bytes ? video->line_stride_in_bytes : (int)row;
	if ((size_t)stride == row) {
		memcpy(frame_data, video->p_data, size);
	} else {
		for (uint32_t y = 0; y < height; y++) {
			memcpy(frame_data + row * y, video->p_data + (size_t)stride * y, row);
		}
	}
	frame_width = width;
	frame_height = height;
	frame_bgrx = video->fourcc == NDI_FOURCC_BGRX;
	frame_seq++;
	pthread_mutex_unlock(&frame_mutex);
	last_video_ns = os_gettime_ns();
}

static void output_to_mirrors(const struct obs_source_audio *out)
{
	pthread_mutex_lock(&mirrors_mutex);
	for (int i = 0; i < mirror_count; i++) {
		obs_source_output_audio(mirrors[i]->self, out);
	}
	pthread_mutex_unlock(&mirrors_mutex);
}

static enum speaker_layout speakers_for(int channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return SPEAKERS_STEREO;
	}
}

static void output_ndi_audio(const ndi_audio_frame_v3_t *audio)
{
	if (audio->fourcc != NDI_FOURCC_FLTP || audio->no_samples <= 0 || audio->no_channels <= 0 || !audio->p_data) {
		return;
	}
	int channels = audio->no_channels;
	if (channels == 7 || channels > 8) {
		channels = 2;
	}
	struct obs_source_audio out = {0};
	for (int i = 0; i < channels && i < MAX_AV_PLANES; i++) {
		out.data[i] = audio->p_data + (size_t)audio->channel_stride_in_bytes * (size_t)i;
	}
	out.frames = (uint32_t)audio->no_samples;
	out.speakers = speakers_for(channels);
	out.format = AUDIO_FORMAT_FLOAT_PLANAR;
	out.samples_per_sec = (uint32_t)audio->sample_rate;
	out.timestamp = os_gettime_ns();
	output_to_mirrors(&out);
}

/* ------------------------------------------------------------------------- */
/* Modo antigo: fonte de mídia escutando o FLV                                */

static volatile bool ndi_active;
static volatile bool old_mode;

/* Som da fonte de mídia: repassa para cada "Telão" (só quando o NDI não está trazendo o som). */
static void receiver_audio(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	(void)param;
	(void)source;
	(void)muted;
	struct obs_audio_info info;
	if (ndi_active || !audio || !audio->frames || !obs_get_audio_info(&info)) {
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
	output_to_mirrors(&out);
}

static obs_data_t *receiver_settings(void)
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
	/* Teste de 16/11: o melhor foi o padrão (decodificação por hardware, sem "nobuffer"). */
	obs_data_set_bool(settings, "hw_decode", true);
	obs_data_set_string(settings, "ffmpeg_options", "probesize=32768 analyzeduration=200000");
	return settings;
}

static void start_old_mode(void)
{
	pthread_mutex_lock(&receiver_mutex);
	if (!receiver) {
		obs_data_t *settings = receiver_settings();
		receiver = obs_source_create_private("ffmpeg_source", "Projeto Live: telão (recebe)", settings);
		obs_data_release(settings);
		if (receiver) {
			obs_source_add_audio_capture_callback(receiver, receiver_audio, NULL);
		}
		blog(LOG_INFO, "[projeto-live] telão: modo antigo (FLV) ligado");
	}
	pthread_mutex_unlock(&receiver_mutex);
	old_mode = true;
	pl_projection_set_mirror(MIRROR_PORT);
}

static void stop_old_mode(void)
{
	pl_projection_set_mirror(0);
	old_mode = false;
	pthread_mutex_lock(&receiver_mutex);
	obs_source_t *old = receiver;
	receiver = NULL;
	pthread_mutex_unlock(&receiver_mutex);
	if (old) {
		obs_source_remove_audio_capture_callback(old, receiver_audio, NULL);
		obs_source_release(old);
		blog(LOG_INFO, "[projeto-live] telão: modo antigo (FLV) desligado");
	}
}

static obs_source_t *get_receiver(void)
{
	pthread_mutex_lock(&receiver_mutex);
	obs_source_t *source = receiver ? obs_source_get_ref(receiver) : NULL;
	pthread_mutex_unlock(&receiver_mutex);
	return source;
}

/* ------------------------------------------------------------------------- */
/* Linha do NDI: acha a fonte "Projeto Live - Telão", recebe, decide o modo   */

static pthread_t ndi_thread;
static bool ndi_thread_running;
static volatile bool ndi_stop;

/* "PC-DA-IGREJA (Projeto Live - Telão)": a parte antes do " (" é o nome do computador. */
static bool machine_matches(const char *ndi_name, const char *names)
{
	const char *paren = strstr(ndi_name, " (");
	if (!paren) {
		return false;
	}
	size_t len = (size_t)(paren - ndi_name);
	const char *p = names;
	while (p && *p) {
		const char *end = strstr(p, ", ");
		size_t n = end ? (size_t)(end - p) : strlen(p);
		if (n == len) {
			bool same = true;
			for (size_t i = 0; i < n; i++) {
				if (tolower((unsigned char)p[i]) != tolower((unsigned char)ndi_name[i])) {
					same = false;
					break;
				}
			}
			if (same) {
				return true;
			}
		}
		p = end ? end + 2 : NULL;
	}
	return false;
}

/* Escolhe a fonte do telão: de preferência a do computador da projeção conectada. */
static bool pick_source(void *find, char *name, size_t name_size, char *url, size_t url_size)
{
	uint32_t count = 0;
	const ndi_source_t *sources = ndi.find_get_current_sources(find, &count);
	char names[512];
	pl_projection_status(names, sizeof(names));
	int best = -1;
	for (uint32_t i = 0; i < count; i++) {
		const char *n = sources[i].p_ndi_name;
		if (!n || !strstr(n, NDI_NAME_TAG)) {
			continue;
		}
		if (machine_matches(n, names)) {
			best = (int)i;
			break;
		}
		if (best < 0) {
			best = (int)i;
		}
	}
	if (best < 0) {
		return false;
	}
	snprintf(name, name_size, "%s", sources[best].p_ndi_name);
	snprintf(url, url_size, "%s", sources[best].p_url_address ? sources[best].p_url_address : "");
	return true;
}

static void decide_mode(uint64_t now, uint64_t started_ns, uint64_t *fresh_since)
{
	uint64_t last = last_video_ns;
	bool fresh = last && now - last < NDI_FRESH_NS;
	ndi_active = fresh;
	if (fresh) {
		if (!*fresh_since) {
			*fresh_since = now;
		}
		if (old_mode && now - *fresh_since > NDI_RECOVER_NS) {
			blog(LOG_INFO, "[projeto-live] telão: NDI de volta");
			stop_old_mode();
		}
		return;
	}
	*fresh_since = 0;
	uint64_t since = last ? last : started_ns;
	if (!old_mode && now - since > NDI_FALLBACK_NS) {
		blog(LOG_INFO, "[projeto-live] telão: NDI não chegou; indo para o modo antigo");
		start_old_mode();
	}
}

static void *ndi_thread_main(void *unused)
{
	(void)unused;
	os_set_thread_name("projeto-live: telão NDI");
	ndi_find_create_t find_settings = {true, NULL, NULL};
	void *find = ndi.find_create_v2(&find_settings);
	void *recv = NULL;
	uint64_t started_ns = os_gettime_ns();
	uint64_t recv_since = 0;
	uint64_t fresh_since = 0;
	char name[256];
	char url[256];

	while (!ndi_stop) {
		uint64_t now = os_gettime_ns();
		decide_mode(now, started_ns, &fresh_since);

		if (!recv) {
			if (!find) {
				os_sleep_ms(500);
				find = ndi.find_create_v2(&find_settings);
				continue;
			}
			ndi.find_wait_for_sources(find, 500);
			if (ndi_stop || !pick_source(find, name, sizeof(name), url, sizeof(url))) {
				continue;
			}
			ndi_recv_create_v3_t settings = {0};
			settings.source_to_connect_to.p_ndi_name = name;
			settings.source_to_connect_to.p_url_address = url[0] ? url : NULL;
			settings.color_format = NDI_COLOR_BGRX_BGRA;
			settings.bandwidth = NDI_BANDWIDTH_HIGHEST;
			settings.allow_video_fields = false;
			settings.p_ndi_recv_name = "Projeto Live (OBS)";
			recv = ndi.recv_create_v3(&settings);
			recv_since = os_gettime_ns();
			blog(LOG_INFO, "[projeto-live] telão: conectando no NDI \"%s\" %s", name, recv ? "" : "(falhou)");
			if (!recv) {
				os_sleep_ms(1000);
			}
			continue;
		}

		ndi_video_frame_v2_t video;
		ndi_audio_frame_v3_t audio;
		int type = ndi.recv_capture_v3(recv, &video, &audio, NULL, 100);
		if (type == NDI_FRAME_VIDEO) {
			store_frame(&video);
			ndi.recv_free_video_v2(recv, &video);
		} else if (type == NDI_FRAME_AUDIO) {
			output_ndi_audio(&audio);
			ndi.recv_free_audio_v3(recv, &audio);
		}

		/* Conectado mas sem imagem faz tempo: a projeção pode ter mudado (outra saída, reiniciou). */
		now = os_gettime_ns();
		uint64_t last = last_video_ns;
		uint64_t since = last > recv_since ? last : recv_since;
		if (now - since > NDI_RECONNECT_NS) {
			blog(LOG_INFO, "[projeto-live] telão: NDI parado; procurando de novo");
			ndi.recv_destroy(recv);
			recv = NULL;
		}
	}

	if (recv) {
		ndi.recv_destroy(recv);
	}
	if (find) {
		ndi.find_destroy(find);
	}
	return NULL;
}

/* 0 = sem "Telão", 1 = NDI (ou procurando), 2 = modo antigo. Para a barra de baixo. */
int pl_mirror_mode(void)
{
	pthread_mutex_lock(&mirrors_mutex);
	int count = mirror_count;
	pthread_mutex_unlock(&mirrors_mutex);
	if (!count) {
		return 0;
	}
	return old_mode && !ndi_active ? 2 : 1;
}

static void start_all(void)
{
	last_video_ns = 0;
	ndi_active = false;
	if (!load_ndi()) {
		start_old_mode();
		return;
	}
	pl_projection_set_ndi(true);
	ndi_stop = false;
	ndi_thread_running = pthread_create(&ndi_thread, NULL, ndi_thread_main, NULL) == 0;
	if (!ndi_thread_running) {
		pl_projection_set_ndi(false);
		start_old_mode();
	}
}

static void stop_all(void)
{
	if (ndi_thread_running) {
		ndi_stop = true;
		pthread_join(ndi_thread, NULL);
		ndi_thread_running = false;
	}
	pl_projection_set_ndi(false);
	stop_old_mode();
	ndi_active = false;

	obs_enter_graphics();
	if (frame_texture) {
		gs_texture_destroy(frame_texture);
		frame_texture = NULL;
	}
	obs_leave_graphics();
	texture_seq = 0;

	pthread_mutex_lock(&frame_mutex);
	bfree(frame_data);
	frame_data = NULL;
	frame_capacity = 0;
	frame_width = frame_height = 0;
	frame_seq = 0;
	pthread_mutex_unlock(&frame_mutex);
}

/* ------------------------------------------------------------------------- */
/* A fonte "Telão (projeção)"                                                 */

static const char *mirror_name(void *unused)
{
	(void)unused;
	return obs_module_text("Mirror.Name");
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
		start_all();
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
		stop_all();
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

/* Põe o quadro novo do NDI na textura. Devolve false se ainda não há quadro. */
static bool update_texture(void)
{
	pthread_mutex_lock(&frame_mutex);
	if (frame_seq && frame_seq != texture_seq && frame_data) {
		if (!frame_texture || texture_width != frame_width || texture_height != frame_height ||
		    texture_bgrx != frame_bgrx) {
			if (frame_texture) {
				gs_texture_destroy(frame_texture);
			}
			frame_texture = gs_texture_create(frame_width, frame_height, frame_bgrx ? GS_BGRX : GS_BGRA, 1,
							  NULL, GS_DYNAMIC);
			texture_width = frame_width;
			texture_height = frame_height;
			texture_bgrx = frame_bgrx;
		}
		if (frame_texture) {
			gs_texture_set_image(frame_texture, frame_data, frame_width * 4, false);
		}
		texture_seq = frame_seq;
	}
	pthread_mutex_unlock(&frame_mutex);
	return frame_texture != NULL;
}

/* Encaixa uma imagem width x height na área de 1920x1080 sem distorcer. */
static void push_fit(uint32_t width, uint32_t height)
{
	float scale_x = (float)MIRROR_WIDTH / (float)width;
	float scale_y = (float)MIRROR_HEIGHT / (float)height;
	float scale = scale_x < scale_y ? scale_x : scale_y;
	float x = ((float)MIRROR_WIDTH - (float)width * scale) / 2.0f;
	float y = ((float)MIRROR_HEIGHT - (float)height * scale) / 2.0f;
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	gs_matrix_scale3f(scale, scale, 1.0f);
}

static void draw_ndi(void)
{
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), frame_texture);
	push_fit(texture_width, texture_height);
	while (gs_effect_loop(effect, "Draw")) {
		gs_draw_sprite(frame_texture, 0, texture_width, texture_height);
	}
	gs_matrix_pop();
	gs_enable_framebuffer_srgb(previous);
}

static void mirror_render(void *data, gs_effect_t *effect)
{
	(void)data;
	(void)effect;
	bool have_ndi = update_texture();
	if (have_ndi && ndi_active) {
		draw_ndi();
		return;
	}
	obs_source_t *old = get_receiver();
	uint32_t width = old ? obs_source_get_width(old) : 0;
	uint32_t height = old ? obs_source_get_height(old) : 0;
	if (width && height) {
		push_fit(width, height);
		obs_source_video_render(old);
		gs_matrix_pop();
	} else if (have_ndi) {
		/* NDI parado e modo antigo ainda sem imagem: fica o último quadro. */
		draw_ndi();
	}
	obs_source_release(old);
}

static void mirror_enum_sources(void *data, obs_source_enum_proc_t enum_callback, void *param)
{
	struct mirror *mirror = data;
	obs_source_t *old = get_receiver();
	if (old) {
		enum_callback(mirror->self, old, param);
		obs_source_release(old);
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
