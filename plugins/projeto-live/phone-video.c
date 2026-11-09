/*
 * Projeto Live: receptor da imagem do celular (veja phone-video.h).
 *
 * Duas threads por câmera:
 * - leitura: abre o endereço com o FFmpeg, lê os pacotes, decodifica e põe cada quadro na fila
 *   com a hora em que deve aparecer;
 * - saída: espera a hora de cada quadro e entrega à fonte interna (assíncrona, sem buffer).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "phone-video.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

#include <stdio.h>
#include <string.h>

#include <media-io/video-io.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include "pl-clock.h"
#include "pl-sync.h"

#define VIDEO_SOURCE_ID "projeto_live_phone_video"
#define URL_MAX 512
#define QUEUE_MAX 90
#define MS_NS 1000000LL
#define SECOND_NS 1000000000LL
/* Sem relógio: atraso pequeno a partir do primeiro quadro, só para absorver a variação da rede. */
#define FALLBACK_BUFFER_NS (120LL * MS_NS)
/* Quadro com hora absurda (relógio pulou): mostra na hora em vez de esperar. */
#define MAX_WAIT_NS (3LL * SECOND_NS)
#define RETRY_MS 1000
#define PTS_WRAP (1ULL << 33)

struct queued_frame {
	struct obs_source_frame *frame;
	int64_t show_ns;
};

struct pl_phone_video {
	char *name;
	obs_source_t *output;
	struct pl_clock *clock;

	pthread_t read_thread;
	pthread_t show_thread;
	bool threads_created;
	volatile bool stop;
	volatile bool restart;

	pthread_mutex_t url_mutex;
	char url[URL_MAX];

	pthread_mutex_t queue_mutex;
	pthread_cond_t queue_cond;
	struct queued_frame queue[QUEUE_MAX];
	int queue_count;
	bool clear_pending;

	volatile long latency_ms;
	volatile bool synced;
};

static volatile long synced_count;

/* Todos os receptores, para achar a câmera mais lenta (atraso automático). */
#define MAX_RECEIVERS 32
static pthread_mutex_t receivers_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct pl_phone_video *receivers[MAX_RECEIVERS];
static int receiver_count;

/* ------------------------------------------------------------------------- */
/* Fonte interna (só recebe os quadros)                                      */

static const char *video_name(void *unused)
{
	(void)unused;
	return "Projeto Live: imagem do celular";
}

static void *video_create(obs_data_t *settings, obs_source_t *source)
{
	(void)settings;
	return source;
}

static void video_destroy(void *data)
{
	(void)data;
}

static struct obs_source_info phone_video_info = {
	.id = VIDEO_SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_CAP_DISABLED,
	.get_name = video_name,
	.create = video_create,
	.destroy = video_destroy,
};

void pl_phone_video_register(void)
{
	obs_register_source(&phone_video_info);
}

int pl_phone_video_synced_count(void)
{
	return (int)os_atomic_load_long(&synced_count);
}

static void set_synced(struct pl_phone_video *video, bool synced)
{
	if (video->synced == synced) {
		return;
	}
	video->synced = synced;
	if (synced) {
		os_atomic_inc_long(&synced_count);
	} else {
		os_atomic_dec_long(&synced_count);
	}
}

/* ------------------------------------------------------------------------- */
/* Fila de quadros e thread de saída                                         */

static void free_queue_locked(struct pl_phone_video *video)
{
	for (int i = 0; i < video->queue_count; i++) {
		obs_source_frame_destroy(video->queue[i].frame);
	}
	video->queue_count = 0;
}

/* Põe na fila em ordem de hora. Fila cheia: sai o mais antigo. */
static void push_frame(struct pl_phone_video *video, struct obs_source_frame *frame, int64_t show_ns)
{
	pthread_mutex_lock(&video->queue_mutex);
	if (video->queue_count == QUEUE_MAX) {
		obs_source_frame_destroy(video->queue[0].frame);
		memmove(&video->queue[0], &video->queue[1], sizeof(video->queue[0]) * (QUEUE_MAX - 1));
		video->queue_count--;
	}
	int at = video->queue_count;
	while (at > 0 && video->queue[at - 1].show_ns > show_ns) {
		at--;
	}
	memmove(&video->queue[at + 1], &video->queue[at], sizeof(video->queue[0]) * (size_t)(video->queue_count - at));
	video->queue[at].frame = frame;
	video->queue[at].show_ns = show_ns;
	video->queue_count++;
	pthread_cond_signal(&video->queue_cond);
	pthread_mutex_unlock(&video->queue_mutex);
}

/* A imagem sumiu (celular desconectou): limpa a fila e a tela. */
static void clear_output(struct pl_phone_video *video)
{
	pthread_mutex_lock(&video->queue_mutex);
	free_queue_locked(video);
	video->clear_pending = true;
	pthread_cond_signal(&video->queue_cond);
	pthread_mutex_unlock(&video->queue_mutex);
}

static void *show_thread(void *data)
{
	struct pl_phone_video *video = data;
	os_set_thread_name("projeto-live: imagem do celular (saída)");

	pthread_mutex_lock(&video->queue_mutex);
	while (!video->stop) {
		if (video->clear_pending) {
			video->clear_pending = false;
			pthread_mutex_unlock(&video->queue_mutex);
			obs_source_output_video(video->output, NULL);
			pthread_mutex_lock(&video->queue_mutex);
			continue;
		}
		if (!video->queue_count) {
			pthread_cond_wait(&video->queue_cond, &video->queue_mutex);
			continue;
		}
		int64_t now = (int64_t)os_gettime_ns();
		int64_t wait = video->queue[0].show_ns - now;
		if (wait > 1 * MS_NS && wait < MAX_WAIT_NS) {
			/* Espera a hora do quadro, olhando a fila de novo a cada 5 ms. */
			int64_t until = wait > 5 * MS_NS ? now + 5 * MS_NS : video->queue[0].show_ns;
			pthread_mutex_unlock(&video->queue_mutex);
			os_sleepto_ns((uint64_t)until);
			pthread_mutex_lock(&video->queue_mutex);
			continue;
		}
		struct obs_source_frame *frame = video->queue[0].frame;
		memmove(&video->queue[0], &video->queue[1], sizeof(video->queue[0]) * (size_t)(video->queue_count - 1));
		video->queue_count--;

		/* Atrasado: se já tem outro quadro também na hora, pula este (alcança o tempo certo). */
		bool skip = video->queue_count && video->queue[0].show_ns <= now;
		pthread_mutex_unlock(&video->queue_mutex);
		if (!skip) {
			frame->timestamp = os_gettime_ns();
			obs_source_output_video(video->output, frame);
		}
		obs_source_frame_destroy(frame);
		pthread_mutex_lock(&video->queue_mutex);
	}
	free_queue_locked(video);
	pthread_mutex_unlock(&video->queue_mutex);
	return NULL;
}

/* ------------------------------------------------------------------------- */
/* Hora de cada quadro                                                       */

struct timing {
	bool anchored;
	int64_t anchor_pts_ns;
	int64_t anchor_show_ns;
};

static int64_t pts_to_ns(uint64_t pts90)
{
	return (int64_t)(pts90 / 90) * 1000000LL + (int64_t)(pts90 % 90) * 1000000LL / 90;
}

/* Hora (no relógio do PC) em que o quadro deve aparecer. */
static int64_t show_time(struct pl_phone_video *video, struct timing *timing, int64_t pts90_raw)
{
	int64_t now = (int64_t)os_gettime_ns();
	int64_t delay = (int64_t)pl_sync_current_ms() * MS_NS;
	uint64_t pts90 = (uint64_t)pts90_raw & (PTS_WRAP - 1);

	int64_t offset;
	if (pl_clock_offset(video->clock, &offset)) {
		/* PTS = relógio do iPhone em 90 kHz, cortado em 33 bits (dá a volta a cada ~26 horas). */
		int64_t period = pts_to_ns(PTS_WRAP);
		int64_t phone_now = now + offset;
		int64_t pts_ns = pts_to_ns(pts90);
		int64_t turns = (phone_now - pts_ns + period / 2) / period;
		int64_t captured_phone = pts_ns + turns * period;
		int64_t captured = captured_phone - offset;
		int64_t latency = now - captured;

		/* Só confia se a conta fizer sentido (app antigo manda PTS que não é o relógio dele). */
		if (latency > -200 * MS_NS && latency < 10 * SECOND_NS) {
			long ms = (long)(latency / MS_NS);
			long old = os_atomic_load_long(&video->latency_ms);
			os_atomic_set_long(&video->latency_ms, old < 0 ? ms : (old * 7 + ms) / 8);
			set_synced(video, true);
			timing->anchored = false;
			return captured + delay;
		}
	}

	/* Sem relógio: no ritmo do PTS, a partir do primeiro quadro. */
	set_synced(video, false);
	os_atomic_set_long(&video->latency_ms, -1);
	int64_t pts_ns = pts_to_ns(pts90);
	int64_t show = timing->anchor_show_ns + (pts_ns - timing->anchor_pts_ns);
	if (!timing->anchored || show < now - 300 * MS_NS || show > now + SECOND_NS) {
		timing->anchored = true;
		timing->anchor_pts_ns = pts_ns;
		timing->anchor_show_ns = now + FALLBACK_BUFFER_NS;
		show = timing->anchor_show_ns;
	}
	return show;
}

/* ------------------------------------------------------------------------- */
/* Quadro do FFmpeg -> quadro do OBS                                         */

static struct obs_source_frame *convert_frame(const AVFrame *in)
{
	enum video_format format;
	switch (in->format) {
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		format = VIDEO_FORMAT_I420;
		break;
	case AV_PIX_FMT_NV12:
		format = VIDEO_FORMAT_NV12;
		break;
	default:
		return NULL;
	}
	uint32_t width = (uint32_t)in->width;
	uint32_t height = (uint32_t)in->height;
	struct obs_source_frame *out = obs_source_frame_create(format, width, height);
	if (!out) {
		return NULL;
	}

	int planes = format == VIDEO_FORMAT_I420 ? 3 : 2;
	for (int plane = 0; plane < planes; plane++) {
		uint32_t rows = plane == 0 ? height : (height + 1) / 2;
		uint32_t bytes = out->linesize[plane] < (uint32_t)in->linesize[plane] ? out->linesize[plane]
										      : (uint32_t)in->linesize[plane];
		for (uint32_t row = 0; row < rows; row++) {
			memcpy(out->data[plane] + (size_t)row * out->linesize[plane],
			       in->data[plane] + (size_t)row * (size_t)in->linesize[plane], bytes);
		}
	}

	bool full_range = in->color_range == AVCOL_RANGE_JPEG || in->format == AV_PIX_FMT_YUVJ420P;
	out->full_range = full_range;
	enum video_colorspace space = in->colorspace == AVCOL_SPC_BT470BG || in->colorspace == AVCOL_SPC_SMPTE170M
					      ? VIDEO_CS_601
					      : VIDEO_CS_709;
	video_format_get_parameters_for_format(space, full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL, format,
					       out->color_matrix, out->color_range_min, out->color_range_max);
	return out;
}

/* ------------------------------------------------------------------------- */
/* Thread de leitura                                                         */

static int interrupt_cb(void *data)
{
	struct pl_phone_video *video = data;
	return video->stop || video->restart;
}

static void current_url(struct pl_phone_video *video, char *url)
{
	pthread_mutex_lock(&video->url_mutex);
	snprintf(url, URL_MAX, "%s", video->url);
	pthread_mutex_unlock(&video->url_mutex);
}

/* Lê e mostra até a conexão cair (ou o endereço mudar). Devolve true se chegou a receber imagem. */
static bool receive(struct pl_phone_video *video, const char *url)
{
	AVFormatContext *format = avformat_alloc_context();
	if (!format) {
		return false;
	}
	format->interrupt_callback.callback = interrupt_cb;
	format->interrupt_callback.opaque = video;
	format->flags |= AVFMT_FLAG_NOBUFFER;
	format->probesize = 500000;
	format->max_analyze_duration = 1500000;

	AVDictionary *options = NULL;
	av_dict_set(&options, "fflags", "nobuffer", 0);
	const AVInputFormat *mpegts = av_find_input_format("mpegts");
	int result = avformat_open_input(&format, url, mpegts, &options);
	av_dict_free(&options);
	if (result < 0) {
		return false;
	}

	bool received = false;
	AVCodecContext *decoder = NULL;
	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	struct timing timing = {0};
	int stream_index = -1;

	if (avformat_find_stream_info(format, NULL) < 0) {
		goto done;
	}
	stream_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (stream_index < 0) {
		goto done;
	}

	const AVCodecParameters *params = format->streams[stream_index]->codecpar;
	const AVCodec *codec = avcodec_find_decoder(params->codec_id);
	decoder = codec ? avcodec_alloc_context3(codec) : NULL;
	if (!decoder || avcodec_parameters_to_context(decoder, params) < 0) {
		goto done;
	}
	decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
	decoder->thread_count = 0;
	decoder->thread_type = FF_THREAD_SLICE;
	if (avcodec_open2(decoder, codec, NULL) < 0) {
		goto done;
	}

	blog(LOG_INFO, "[projeto-live] '%s': recebendo a imagem do celular", video->name);
	AVRational time_base = format->streams[stream_index]->time_base;

	while (!video->stop && !video->restart && av_read_frame(format, packet) >= 0) {
		if (packet->stream_index == stream_index && avcodec_send_packet(decoder, packet) >= 0) {
			while (avcodec_receive_frame(decoder, frame) >= 0) {
				int64_t pts = frame->best_effort_timestamp;
				if (pts != AV_NOPTS_VALUE) {
					int64_t pts90 = av_rescale_q(pts, time_base, (AVRational){1, 90000});
					struct obs_source_frame *out = convert_frame(frame);
					if (out) {
						push_frame(video, out, show_time(video, &timing, pts90));
						received = true;
					}
				}
				av_frame_unref(frame);
			}
		}
		av_packet_unref(packet);
	}

done:
	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&decoder);
	avformat_close_input(&format);
	return received;
}

static void *read_thread(void *data)
{
	struct pl_phone_video *video = data;
	os_set_thread_name("projeto-live: imagem do celular (leitura)");

	while (!video->stop) {
		video->restart = false;
		char url[URL_MAX];
		current_url(video, url);
		if (!url[0]) {
			os_sleep_ms(200);
			continue;
		}
		bool received = receive(video, url);
		set_synced(video, false);
		os_atomic_set_long(&video->latency_ms, -1);
		if (received) {
			blog(LOG_INFO, "[projeto-live] '%s': a imagem do celular parou", video->name);
			clear_output(video);
		}
		/* Espera um pouco antes de tentar de novo (sem atrasar uma troca de endereço). */
		for (int waited = 0; waited < RETRY_MS && !video->stop && !video->restart; waited += 50) {
			os_sleep_ms(50);
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------------- */

struct pl_phone_video *pl_phone_video_create(const char *name, struct pl_clock *clock)
{
	struct pl_phone_video *video = bzalloc(sizeof(*video));
	video->name = bstrdup(name);
	video->clock = clock;
	video->latency_ms = -1;
	video->output = obs_source_create_private(VIDEO_SOURCE_ID, "Projeto Live: imagem do celular", NULL);
	if (video->output) {
		obs_source_set_async_unbuffered(video->output, true);
	}
	pthread_mutex_init(&video->url_mutex, NULL);
	pthread_mutex_init(&video->queue_mutex, NULL);
	pthread_cond_init(&video->queue_cond, NULL);

	if (pthread_create(&video->show_thread, NULL, show_thread, video) == 0) {
		if (pthread_create(&video->read_thread, NULL, read_thread, video) == 0) {
			video->threads_created = true;
		} else {
			video->stop = true;
			pthread_mutex_lock(&video->queue_mutex);
			pthread_cond_signal(&video->queue_cond);
			pthread_mutex_unlock(&video->queue_mutex);
			pthread_join(video->show_thread, NULL);
		}
	}
	if (!video->threads_created) {
		blog(LOG_WARNING, "[projeto-live] '%s': não deu para abrir o receptor da imagem", name);
	}

	pthread_mutex_lock(&receivers_mutex);
	if (receiver_count < MAX_RECEIVERS) {
		receivers[receiver_count++] = video;
	}
	pthread_mutex_unlock(&receivers_mutex);
	return video;
}

void pl_phone_video_destroy(struct pl_phone_video *video)
{
	if (!video) {
		return;
	}
	pthread_mutex_lock(&receivers_mutex);
	for (int i = 0; i < receiver_count; i++) {
		if (receivers[i] == video) {
			receivers[i] = receivers[--receiver_count];
			break;
		}
	}
	pthread_mutex_unlock(&receivers_mutex);
	video->stop = true;
	if (video->threads_created) {
		pthread_join(video->read_thread, NULL);
		pthread_mutex_lock(&video->queue_mutex);
		pthread_cond_signal(&video->queue_cond);
		pthread_mutex_unlock(&video->queue_mutex);
		pthread_join(video->show_thread, NULL);
	}
	set_synced(video, false);
	obs_source_release(video->output);
	pthread_cond_destroy(&video->queue_cond);
	pthread_mutex_destroy(&video->queue_mutex);
	pthread_mutex_destroy(&video->url_mutex);
	bfree(video->name);
	bfree(video);
}

obs_source_t *pl_phone_video_source(struct pl_phone_video *video)
{
	return video ? video->output : NULL;
}

void pl_phone_video_set_url(struct pl_phone_video *video, const char *url)
{
	if (!video) {
		return;
	}
	pthread_mutex_lock(&video->url_mutex);
	bool changed = strcmp(video->url, url ? url : "") != 0;
	snprintf(video->url, sizeof(video->url), "%s", url ? url : "");
	pthread_mutex_unlock(&video->url_mutex);
	if (changed) {
		video->restart = true;
	}
}

int pl_phone_video_latency_ms(struct pl_phone_video *video)
{
	return video ? (int)os_atomic_load_long(&video->latency_ms) : -1;
}

int pl_phone_video_max_latency_ms(void)
{
	int worst = -1;
	pthread_mutex_lock(&receivers_mutex);
	for (int i = 0; i < receiver_count; i++) {
		int latency = (int)os_atomic_load_long(&receivers[i]->latency_ms);
		if (latency > worst) {
			worst = latency;
		}
	}
	pthread_mutex_unlock(&receivers_mutex);
	return worst;
}
