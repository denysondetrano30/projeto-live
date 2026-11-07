/*
 * Projeto Live: central do intercom (veja intercom.h).
 *
 * Duas threads: a da rede (recebe os pacotes de voz pelo Wi-Fi e pelos túneis do cabo) e a da
 * mistura (a cada 20 ms monta e manda o que cada um deve ouvir). Tudo o que as duas dividem fica
 * protegido por `hub.mutex`. Só a thread da rede fecha os túneis do cabo, para nenhuma outra
 * thread usar um socket já fechado.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "intercom-stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include "intercom-audio.h"
#include "sha256.h"

#ifdef _WIN32
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#endif

#define MS_NS 1000000ULL
#define FRAME_NS (20 * MS_NS)
#define TALK_HOLD_NS (150 * MS_NS)    /* sem voz por 150 ms = parou de falar */
#define UDP_ALIVE_NS (5000 * MS_NS)   /* sem pacote por 5 s = o celular saiu */
#define DEVICE_IDLE_NS (10000 * MS_NS) /* sem celulares por 10 s: fecha fone e microfone */
#define DEVICE_RETRY_NS (10000 * MS_NS)
#define JITTER_FRAMES 16
#define JITTER_START 2 /* junta 40 ms antes de tocar uma fala */
#define JITTER_MAX 8   /* mais de 160 ms na fila: atrasou; joga fora o mais velho... */
#define JITTER_KEEP 3  /* ...até sobrar 60 ms */
#define STREAM_OUT_MAX 16384
#define CLOSING_MAX 32
#define MAX_PEERS 32
#define PEER_NAME_MAX 128
#define TOKEN_SIZE 8

static const char magic[4] = {'P', 'L', 'V', '1'};

struct pl_voice_peer {
	struct pl_voice_peer *next;
	char name[PEER_NAME_MAX];
	uint8_t token[TOKEN_SIZE];
	char token_hex[TOKEN_SIZE * 2 + 1];

	/* Wi-Fi */
	bool has_addr;
	struct sockaddr_in addr;
	uint64_t last_udp_ns;

	/* Cabo */
	pl_socket_t stream;
	uint8_t stream_in[2 + PL_VOICE_PACKET_MAX];
	size_t stream_in_used;
	uint8_t stream_out[STREAM_OUT_MAX];
	size_t stream_out_used;

	/* Voz que chegou do celular, esperando a vez na mistura */
	int16_t jitter[JITTER_FRAMES][PL_VOICE_SAMPLES];
	size_t jitter_head;
	size_t jitter_count;
	bool playing;
	uint64_t last_voice_ns;

	bool private_talk;
	bool talking;
	bool was_connected;
	char voice_text[PL_VOICE_TEXT_MAX];
	uint32_t voice_serial;
	uint16_t seq;
};

static struct {
	pthread_mutex_t mutex;
	bool started;
	volatile bool stop;
	struct pl_voice_peer *peers;

	pl_socket_t udp;
	int udp_port;
	pthread_t net_thread;
	pthread_t mix_thread;
	pl_socket_t closing[CLOSING_MAX];
	size_t closing_count;

	bool talk_all;
	bool latched;
	char speaking[PL_VOICE_TEXT_MAX];

	char capture_name[256];
	char playback_name[256];
	float volume;
	bool devices_active;
	bool devices_dirty;
	char device_error[160];
	uint64_t last_wanted_ns;
	uint64_t device_retry_ns;

	uint64_t token_counter;
} hub = {
	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.udp = PL_INVALID_SOCKET,
	.volume = 1.0f,
};

/* ------------------------------------------------------------------------- */
/* Configuração do fone e do microfone (guardada em intercom.json)           */

static void load_settings(void)
{
	char *path = obs_module_config_path("intercom.json");
	obs_data_t *data = path ? obs_data_create_from_json_file_safe(path, "bak") : NULL;
	if (data) {
		obs_data_set_default_double(data, "volume", 1.0);
		snprintf(hub.capture_name, sizeof(hub.capture_name), "%s", obs_data_get_string(data, "capture"));
		snprintf(hub.playback_name, sizeof(hub.playback_name), "%s", obs_data_get_string(data, "playback"));
		hub.volume = (float)obs_data_get_double(data, "volume");
		obs_data_release(data);
	}
	bfree(path);
}

static void save_settings(const char *capture, const char *playback, float volume)
{
	char *dir = obs_module_config_path("");
	char *path = obs_module_config_path("intercom.json");
	if (dir && path) {
		os_mkdirs(dir);
		obs_data_t *data = obs_data_create();
		obs_data_set_string(data, "capture", capture);
		obs_data_set_string(data, "playback", playback);
		obs_data_set_double(data, "volume", volume);
		obs_data_save_json_safe(data, path, "tmp", "bak");
		obs_data_release(data);
	}
	bfree(dir);
	bfree(path);
}

/* ------------------------------------------------------------------------- */
/* Participantes (com hub.mutex)                                             */

static bool peer_connected(const struct pl_voice_peer *peer, uint64_t now)
{
	return peer->stream != PL_INVALID_SOCKET || (peer->has_addr && now - peer->last_udp_ns < UDP_ALIVE_NS);
}

static void detach_locked(struct pl_voice_peer *peer)
{
	if (peer->stream == PL_INVALID_SOCKET)
		return;
	if (hub.closing_count < CLOSING_MAX)
		hub.closing[hub.closing_count++] = peer->stream;
	else
		pl_close_socket(peer->stream); /* não deve acontecer: a rede esvazia a lista a cada 20 ms */
	peer->stream = PL_INVALID_SOCKET;
	peer->stream_in_used = 0;
	peer->stream_out_used = 0;
}

static struct pl_voice_peer *find_by_token(const uint8_t *token)
{
	for (struct pl_voice_peer *peer = hub.peers; peer; peer = peer->next) {
		if (memcmp(peer->token, token, TOKEN_SIZE) == 0)
			return peer;
	}
	return NULL;
}

static void jitter_push(struct pl_voice_peer *peer, const uint8_t *pcm)
{
	if (peer->jitter_count == JITTER_FRAMES) {
		peer->jitter_head = (peer->jitter_head + 1) % JITTER_FRAMES;
		peer->jitter_count--;
	}
	int16_t *frame = peer->jitter[(peer->jitter_head + peer->jitter_count) % JITTER_FRAMES];
	for (size_t i = 0; i < PL_VOICE_SAMPLES; i++)
		frame[i] = (int16_t)(uint16_t)(pcm[i * 2] | (pcm[i * 2 + 1] << 8));
	peer->jitter_count++;
}

static bool jitter_pop(struct pl_voice_peer *peer, int16_t *out)
{
	if (!peer->playing) {
		if (peer->jitter_count < JITTER_START)
			return false;
		peer->playing = true;
	}
	if (peer->jitter_count == 0) {
		peer->playing = false;
		return false;
	}
	while (peer->jitter_count > JITTER_MAX) {
		peer->jitter_head = (peer->jitter_head + 1) % JITTER_FRAMES;
		peer->jitter_count--;
	}
	memcpy(out, peer->jitter[peer->jitter_head], sizeof(int16_t) * PL_VOICE_SAMPLES);
	peer->jitter_head = (peer->jitter_head + 1) % JITTER_FRAMES;
	peer->jitter_count--;
	return true;
}

/* Um pacote que chegou deste celular (do Wi-Fi ou do cabo). */
static void handle_packet(struct pl_voice_peer *peer, const uint8_t *data, size_t size, uint64_t now)
{
	if (size < PL_VOICE_HEADER || memcmp(data, magic, 4) != 0)
		return;
	uint8_t flags = data[14];
	if ((flags & PL_VOICE_FLAG_AUDIO) && size >= PL_VOICE_PACKET_MAX) {
		jitter_push(peer, data + PL_VOICE_HEADER);
		peer->last_voice_ns = now;
	}
}

/* ------------------------------------------------------------------------- */
/* Thread da rede                                                            */

static void read_udp(uint64_t now)
{
	uint8_t packet[PL_VOICE_PACKET_MAX + 64];
	for (int i = 0; i < 256; i++) {
		struct sockaddr_in from;
		socklen_t from_len = sizeof(from);
		int got = (int)recvfrom(hub.udp, (char *)packet, (int)sizeof(packet), 0, (struct sockaddr *)&from,
					&from_len);
		if (got < 0)
			return; /* nada mais por agora (ou erro passageiro: tenta na próxima volta) */
		if ((size_t)got < PL_VOICE_HEADER || memcmp(packet, magic, 4) != 0)
			continue;

		pthread_mutex_lock(&hub.mutex);
		struct pl_voice_peer *peer = find_by_token(packet + 4);
		if (peer) {
			peer->addr = from;
			peer->has_addr = true;
			peer->last_udp_ns = now;
			handle_packet(peer, packet, (size_t)got, now);
		}
		pthread_mutex_unlock(&hub.mutex);
	}
}

/* Lê um túnel do cabo (com hub.mutex). Devolve false se ele fechou ou mandou algo inválido. */
static bool read_stream(struct pl_voice_peer *peer, uint64_t now)
{
	size_t room = sizeof(peer->stream_in) - peer->stream_in_used;
	int got = (int)recv(peer->stream, (char *)peer->stream_in + peer->stream_in_used, (int)room, 0);
	if (got == 0)
		return false;
	if (got < 0)
		return pl_would_block();
	peer->stream_in_used += (size_t)got;

	size_t offset = 0;
	while (peer->stream_in_used - offset >= 2) {
		size_t length = peer->stream_in[offset] | ((size_t)peer->stream_in[offset + 1] << 8);
		if (length < PL_VOICE_HEADER || length > PL_VOICE_PACKET_MAX)
			return false;
		if (peer->stream_in_used - offset < 2 + length)
			break;
		handle_packet(peer, peer->stream_in + offset + 2, length, now);
		offset += 2 + length;
	}
	memmove(peer->stream_in, peer->stream_in + offset, peer->stream_in_used - offset);
	peer->stream_in_used -= offset;
	return true;
}

static void *net_thread(void *data)
{
	(void)data;
	os_set_thread_name("projeto-live: intercom rede");

	while (!hub.stop) {
		pl_socket_t streams[MAX_PEERS];
		size_t stream_count = 0;

		pthread_mutex_lock(&hub.mutex);
		for (size_t i = 0; i < hub.closing_count; i++)
			pl_close_socket(hub.closing[i]);
		hub.closing_count = 0;
		for (struct pl_voice_peer *peer = hub.peers; peer && stream_count < MAX_PEERS; peer = peer->next) {
			if (peer->stream != PL_INVALID_SOCKET)
				streams[stream_count++] = peer->stream;
		}
		pthread_mutex_unlock(&hub.mutex);

		fd_set readable;
		FD_ZERO(&readable);
		int max_fd = (int)hub.udp;
		FD_SET(hub.udp, &readable);
		for (size_t i = 0; i < stream_count; i++) {
			FD_SET(streams[i], &readable);
			if ((int)streams[i] > max_fd)
				max_fd = (int)streams[i];
		}

		struct timeval timeout = {0, 20000};
		int ready = select(max_fd + 1, &readable, NULL, NULL, &timeout);
		if (ready <= 0) {
			if (ready < 0)
				os_sleep_ms(20);
			continue;
		}
		uint64_t now = os_gettime_ns();

		if (FD_ISSET(hub.udp, &readable))
			read_udp(now);

		for (size_t i = 0; i < stream_count; i++) {
			if (!FD_ISSET(streams[i], &readable))
				continue;
			pthread_mutex_lock(&hub.mutex);
			for (struct pl_voice_peer *peer = hub.peers; peer; peer = peer->next) {
				if (peer->stream == streams[i]) {
					if (!read_stream(peer, now))
						detach_locked(peer);
					break;
				}
			}
			pthread_mutex_unlock(&hub.mutex);
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------------- */
/* Thread da mistura                                                         */

static void put_samples(uint8_t *out, const int32_t *mix)
{
	for (size_t i = 0; i < PL_VOICE_SAMPLES; i++) {
		int32_t value = mix[i];
		if (value > 32767)
			value = 32767;
		else if (value < -32768)
			value = -32768;
		uint16_t sample = (uint16_t)(int16_t)value;
		out[i * 2] = (uint8_t)(sample & 0xFF);
		out[i * 2 + 1] = (uint8_t)(sample >> 8);
	}
}

static void flush_stream(struct pl_voice_peer *peer)
{
	if (peer->stream == PL_INVALID_SOCKET || !peer->stream_out_used)
		return;
	int sent = (int)send(peer->stream, (const char *)peer->stream_out, (int)peer->stream_out_used, PL_SEND_FLAGS);
	if (sent < 0) {
		if (!pl_would_block())
			detach_locked(peer);
		return;
	}
	memmove(peer->stream_out, peer->stream_out + sent, peer->stream_out_used - (size_t)sent);
	peer->stream_out_used -= (size_t)sent;
}

static void send_to_peer(struct pl_voice_peer *peer, const int32_t *mix, uint64_t now)
{
	uint8_t packet[PL_VOICE_PACKET_MAX];
	memcpy(packet, magic, 4);
	memcpy(packet + 4, peer->token, TOKEN_SIZE);
	packet[12] = (uint8_t)(peer->seq & 0xFF);
	packet[13] = (uint8_t)(peer->seq >> 8);
	packet[14] = PL_VOICE_FLAG_AUDIO;
	packet[15] = 0;
	put_samples(packet + PL_VOICE_HEADER, mix);
	peer->seq++;

	if (peer->stream != PL_INVALID_SOCKET) {
		/* Pelo cabo: se a fila estiver cheia, o quadro é descartado inteiro (nunca pela metade). */
		if (peer->stream_out_used + 2 + sizeof(packet) <= sizeof(peer->stream_out)) {
			uint8_t *out = peer->stream_out + peer->stream_out_used;
			out[0] = (uint8_t)(sizeof(packet) & 0xFF);
			out[1] = (uint8_t)(sizeof(packet) >> 8);
			memcpy(out + 2, packet, sizeof(packet));
			peer->stream_out_used += 2 + sizeof(packet);
		}
		flush_stream(peer);
	} else if (peer->has_addr && now - peer->last_udp_ns < UDP_ALIVE_NS) {
		sendto(hub.udp, (const char *)packet, (int)sizeof(packet), 0, (const struct sockaddr *)&peer->addr,
		       sizeof(peer->addr));
	}
}

static void append_name(char *text, size_t size, const char *name)
{
	size_t len = strlen(text);
	if (len && len + 2 < size) {
		strcat(text, ", ");
		len += 2;
	}
	if (len < size - 1)
		snprintf(text + len, size - len, "%s", name);
}

/* Monta o que cada um ouve nestes 20 ms (com hub.mutex). */
static void mix_step(uint64_t now, const int16_t *director)
{
	struct pl_voice_peer *peers[MAX_PEERS];
	static int16_t voice[MAX_PEERS][PL_VOICE_SAMPLES];
	bool has_voice[MAX_PEERS];
	size_t count = 0;

	for (struct pl_voice_peer *peer = hub.peers; peer && count < MAX_PEERS; peer = peer->next) {
		peers[count] = peer;
		has_voice[count] = jitter_pop(peer, voice[count]);
		peer->talking = peer->last_voice_ns && now - peer->last_voice_ns < TALK_HOLD_NS;

		bool connected = peer_connected(peer, now);
		if (connected != peer->was_connected) {
			blog(LOG_INFO, "[projeto-live] '%s' %s intercom", peer->name,
			     connected ? "entrou no" : "saiu do");
			peer->was_connected = connected;
		}
		if (!connected)
			peer->talking = false;
		count++;
	}

	bool director_all = hub.talk_all || hub.latched;

	/* O fone do diretor ouve todas as câmeras que estão falando. */
	if (hub.devices_active) {
		int32_t mix[PL_VOICE_SAMPLES] = {0};
		bool any = false;
		for (size_t i = 0; i < count; i++) {
			if (!has_voice[i])
				continue;
			any = true;
			for (size_t s = 0; s < PL_VOICE_SAMPLES; s++)
				mix[s] += voice[i][s];
		}
		if (any) {
			int16_t out[PL_VOICE_SAMPLES];
			for (size_t s = 0; s < PL_VOICE_SAMPLES; s++) {
				float value = (float)mix[s] * hub.volume;
				if (value > 32767.0f)
					value = 32767.0f;
				else if (value < -32768.0f)
					value = -32768.0f;
				out[s] = (int16_t)value;
			}
			pl_audio_playback_write(out, PL_VOICE_SAMPLES);
		}
	}

	/* Cada celular ouve as outras câmeras e, se for o caso, o diretor. */
	for (size_t i = 0; i < count; i++) {
		struct pl_voice_peer *peer = peers[i];
		if (!peer_connected(peer, now))
			continue;

		int32_t mix[PL_VOICE_SAMPLES] = {0};
		bool any = false;
		for (size_t j = 0; j < count; j++) {
			if (j == i || !has_voice[j])
				continue;
			any = true;
			for (size_t s = 0; s < PL_VOICE_SAMPLES; s++)
				mix[s] += voice[j][s];
		}
		if (director && (director_all || peer->private_talk)) {
			any = true;
			for (size_t s = 0; s < PL_VOICE_SAMPLES; s++)
				mix[s] += director[s];
		}
		if (any)
			send_to_peer(peer, mix, now);
		else
			flush_stream(peer);
	}

	/* Quem está falando: para o painel e para cada celular. */
	char speaking[PL_VOICE_TEXT_MAX] = "";
	for (size_t i = 0; i < count; i++) {
		if (peers[i]->talking)
			append_name(speaking, sizeof(speaking), peers[i]->name);
	}
	snprintf(hub.speaking, sizeof(hub.speaking), "%s", speaking);

	for (size_t i = 0; i < count; i++) {
		struct pl_voice_peer *peer = peers[i];
		char text[PL_VOICE_TEXT_MAX] = "";
		if (director_all)
			append_name(text, sizeof(text), obs_module_text("Intercom.Director"));
		else if (peer->private_talk)
			append_name(text, sizeof(text), obs_module_text("Intercom.DirectorPrivate"));
		for (size_t j = 0; j < count; j++) {
			if (j != i && peers[j]->talking)
				append_name(text, sizeof(text), peers[j]->name);
		}
		if (strcmp(text, peer->voice_text) != 0) {
			snprintf(peer->voice_text, sizeof(peer->voice_text), "%s", text);
			peer->voice_serial++;
		}
	}
}

/* Abre o fone e o microfone do diretor quando há celulares no intercom; fecha quando não há. */
static void manage_devices(uint64_t now)
{
	char capture[256], playback[256];
	pthread_mutex_lock(&hub.mutex);
	bool any = false;
	for (struct pl_voice_peer *peer = hub.peers; peer; peer = peer->next)
		any = any || peer_connected(peer, now);
	if (any)
		hub.last_wanted_ns = now;
	bool wanted = any || (hub.devices_active && now - hub.last_wanted_ns < DEVICE_IDLE_NS);
	bool retry = hub.device_error[0] && now >= hub.device_retry_ns;
	bool reopen = wanted && (!hub.devices_active || hub.devices_dirty || retry);
	bool close = !wanted && hub.devices_active;
	snprintf(capture, sizeof(capture), "%s", hub.capture_name);
	snprintf(playback, sizeof(playback), "%s", hub.playback_name);
	hub.devices_dirty = false;
	pthread_mutex_unlock(&hub.mutex);

	if (reopen) {
		char error[160];
		bool ok = pl_audio_open(capture, playback, error, sizeof(error));
		if (!ok || error[0])
			blog(LOG_WARNING, "[projeto-live] intercom: %s", error);
		pthread_mutex_lock(&hub.mutex);
		hub.devices_active = true;
		snprintf(hub.device_error, sizeof(hub.device_error), "%s", ok ? "" : error);
		hub.device_retry_ns = now + DEVICE_RETRY_NS;
		pthread_mutex_unlock(&hub.mutex);
	} else if (close) {
		pl_audio_close();
		pthread_mutex_lock(&hub.mutex);
		hub.devices_active = false;
		hub.device_error[0] = 0;
		pthread_mutex_unlock(&hub.mutex);
	}
}

static void *mix_thread(void *data)
{
	(void)data;
	os_set_thread_name("projeto-live: intercom mistura");

	uint64_t next = os_gettime_ns();
	uint64_t next_devices = 0;
	int16_t director[PL_VOICE_SAMPLES];

	while (!hub.stop) {
		next += FRAME_NS;
		uint64_t now = os_gettime_ns();
		if (now > next + 5 * FRAME_NS)
			next = now; /* ficou muito para trás (computador ocupado): recomeça do agora */
		os_sleepto_ns(next);
		now = os_gettime_ns();

		if (now >= next_devices) {
			manage_devices(now);
			next_devices = now + 500 * MS_NS;
		}

		/* O microfone é lido sempre, para não acumular; só vai para alguém se o diretor falar. */
		bool have_director = pl_audio_capture_read(director, PL_VOICE_SAMPLES);

		pthread_mutex_lock(&hub.mutex);
		bool director_talking = hub.talk_all || hub.latched;
		for (struct pl_voice_peer *peer = hub.peers; peer && !director_talking; peer = peer->next)
			director_talking = peer->private_talk;
		mix_step(now, have_director && director_talking ? director : NULL);
		pthread_mutex_unlock(&hub.mutex);
	}
	return NULL;
}

/* ------------------------------------------------------------------------- */
/* Ligar e desligar                                                          */

void pl_intercom_start(void)
{
	if (hub.started)
		return;
	load_settings();
	pl_socket_startup();

	pl_socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == PL_INVALID_SOCKET) {
		blog(LOG_WARNING, "[projeto-live] intercom: não foi possível abrir a porta de voz");
		pl_socket_cleanup();
		return;
	}
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = 0;
	socklen_t len = sizeof(addr);
	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    getsockname(sock, (struct sockaddr *)&addr, &len) != 0) {
		blog(LOG_WARNING, "[projeto-live] intercom: não foi possível abrir a porta de voz");
		pl_close_socket(sock);
		pl_socket_cleanup();
		return;
	}
	pl_set_nonblocking(sock);
#ifdef _WIN32
	/* Sem isto, o Windows "quebra" o socket quando um celular some (aviso ICMP de porta fechada). */
	BOOL report = FALSE;
	DWORD bytes = 0;
	WSAIoctl(sock, SIO_UDP_CONNRESET, &report, sizeof(report), NULL, 0, &bytes, NULL, NULL);
#endif

	hub.udp = sock;
	hub.udp_port = ntohs(addr.sin_port);
	hub.stop = false;
	if (pthread_create(&hub.net_thread, NULL, net_thread, NULL) != 0) {
		pl_close_socket(sock);
		hub.udp = PL_INVALID_SOCKET;
		pl_socket_cleanup();
		return;
	}
	if (pthread_create(&hub.mix_thread, NULL, mix_thread, NULL) != 0) {
		hub.stop = true;
		pthread_join(hub.net_thread, NULL);
		pl_close_socket(sock);
		hub.udp = PL_INVALID_SOCKET;
		pl_socket_cleanup();
		return;
	}
	hub.started = true;
	blog(LOG_INFO, "[projeto-live] intercom pronto (voz pelo Wi-Fi na porta UDP %d)", hub.udp_port);
}

void pl_intercom_stop(void)
{
	if (!hub.started)
		return;
	hub.stop = true;
	pthread_join(hub.net_thread, NULL);
	pthread_join(hub.mix_thread, NULL);
	hub.started = false;

	pthread_mutex_lock(&hub.mutex);
	for (struct pl_voice_peer *peer = hub.peers; peer; peer = peer->next)
		detach_locked(peer);
	for (size_t i = 0; i < hub.closing_count; i++)
		pl_close_socket(hub.closing[i]);
	hub.closing_count = 0;
	pthread_mutex_unlock(&hub.mutex);

	pl_close_socket(hub.udp);
	hub.udp = PL_INVALID_SOCKET;
	pl_audio_shutdown();
	pl_socket_cleanup();
}

/* ------------------------------------------------------------------------- */
/* Participantes                                                             */

static void make_token(struct pl_voice_peer *peer)
{
	uint64_t time = os_gettime_ns();
	uint64_t counter = ++hub.token_counter;
	uintptr_t where = (uintptr_t)peer;
	int random = rand();

	uint8_t digest[PL_SHA256_SIZE];
	struct pl_sha256 ctx;
	pl_sha256_init(&ctx);
	pl_sha256_update(&ctx, &time, sizeof(time));
	pl_sha256_update(&ctx, &counter, sizeof(counter));
	pl_sha256_update(&ctx, &where, sizeof(where));
	pl_sha256_update(&ctx, &random, sizeof(random));
	pl_sha256_final(&ctx, digest);
	memcpy(peer->token, digest, TOKEN_SIZE);
	pl_hex(peer->token, TOKEN_SIZE, peer->token_hex);
}

struct pl_voice_peer *pl_voice_peer_create(const char *name)
{
	struct pl_voice_peer *peer = bzalloc(sizeof(struct pl_voice_peer));
	peer->stream = PL_INVALID_SOCKET;
	snprintf(peer->name, sizeof(peer->name), "%s", name ? name : "");

	pthread_mutex_lock(&hub.mutex);
	make_token(peer);
	peer->next = hub.peers;
	hub.peers = peer;
	pthread_mutex_unlock(&hub.mutex);
	return peer;
}

void pl_voice_peer_destroy(struct pl_voice_peer *peer)
{
	if (!peer)
		return;
	pthread_mutex_lock(&hub.mutex);
	for (struct pl_voice_peer **link = &hub.peers; *link; link = &(*link)->next) {
		if (*link == peer) {
			*link = peer->next;
			break;
		}
	}
	detach_locked(peer);
	if (!hub.started) {
		for (size_t i = 0; i < hub.closing_count; i++)
			pl_close_socket(hub.closing[i]);
		hub.closing_count = 0;
	}
	pthread_mutex_unlock(&hub.mutex);
	bfree(peer);
}

void pl_voice_peer_set_name(struct pl_voice_peer *peer, const char *name)
{
	if (!peer || !name)
		return;
	pthread_mutex_lock(&hub.mutex);
	snprintf(peer->name, sizeof(peer->name), "%s", name);
	pthread_mutex_unlock(&hub.mutex);
}

bool pl_voice_peer_audio_line(struct pl_voice_peer *peer, char *out, size_t size)
{
	if (!peer || !hub.started)
		return false;
	pthread_mutex_lock(&hub.mutex);
	snprintf(out, size, "AUDIO %d %s", hub.udp_port, peer->token_hex);
	pthread_mutex_unlock(&hub.mutex);
	return true;
}

void pl_voice_peer_attach_stream(struct pl_voice_peer *peer, pl_socket_t sock)
{
	if (!peer || sock == PL_INVALID_SOCKET)
		return;
	pl_set_nonblocking(sock);
	pl_no_delay(sock);
	pthread_mutex_lock(&hub.mutex);
	detach_locked(peer);
	if (hub.started) {
		peer->stream = sock;
	} else {
		pl_close_socket(sock);
	}
	pthread_mutex_unlock(&hub.mutex);
}

void pl_voice_peer_detach_stream(struct pl_voice_peer *peer)
{
	if (!peer)
		return;
	pthread_mutex_lock(&hub.mutex);
	detach_locked(peer);
	pthread_mutex_unlock(&hub.mutex);
}

bool pl_voice_peer_has_stream(struct pl_voice_peer *peer)
{
	if (!peer)
		return false;
	pthread_mutex_lock(&hub.mutex);
	bool has = peer->stream != PL_INVALID_SOCKET;
	pthread_mutex_unlock(&hub.mutex);
	return has;
}

uint32_t pl_voice_peer_voice_text(struct pl_voice_peer *peer, char *out, size_t size)
{
	if (!peer) {
		snprintf(out, size, "%s", "");
		return 0;
	}
	pthread_mutex_lock(&hub.mutex);
	snprintf(out, size, "%s", peer->voice_text);
	uint32_t serial = peer->voice_serial;
	pthread_mutex_unlock(&hub.mutex);
	return serial;
}

bool pl_voice_peer_is_connected(struct pl_voice_peer *peer)
{
	if (!peer)
		return false;
	pthread_mutex_lock(&hub.mutex);
	bool connected = peer_connected(peer, os_gettime_ns());
	pthread_mutex_unlock(&hub.mutex);
	return connected;
}

bool pl_voice_peer_is_talking(struct pl_voice_peer *peer)
{
	if (!peer)
		return false;
	pthread_mutex_lock(&hub.mutex);
	bool talking = peer->talking;
	pthread_mutex_unlock(&hub.mutex);
	return talking;
}

/* ------------------------------------------------------------------------- */
/* Diretor                                                                   */

void pl_intercom_set_talk_all(bool on)
{
	pthread_mutex_lock(&hub.mutex);
	hub.talk_all = on;
	pthread_mutex_unlock(&hub.mutex);
}

void pl_intercom_set_latched(bool on)
{
	pthread_mutex_lock(&hub.mutex);
	hub.latched = on;
	pthread_mutex_unlock(&hub.mutex);
}

bool pl_intercom_get_latched(void)
{
	pthread_mutex_lock(&hub.mutex);
	bool latched = hub.latched;
	pthread_mutex_unlock(&hub.mutex);
	return latched;
}

void pl_voice_peer_set_private(struct pl_voice_peer *peer, bool on)
{
	if (!peer)
		return;
	pthread_mutex_lock(&hub.mutex);
	peer->private_talk = on;
	pthread_mutex_unlock(&hub.mutex);
}

void pl_intercom_get_state(struct pl_intercom_state *state)
{
	memset(state, 0, sizeof(*state));
	uint64_t now = os_gettime_ns();
	pthread_mutex_lock(&hub.mutex);
	state->latched = hub.latched;
	state->talk_all = hub.talk_all || hub.latched;
	state->talking = state->talk_all;
	for (struct pl_voice_peer *peer = hub.peers; peer; peer = peer->next) {
		state->talking = state->talking || peer->private_talk;
		state->any_phone = state->any_phone || peer_connected(peer, now);
	}
	state->devices_open = hub.devices_active && !hub.device_error[0];
	snprintf(state->error, sizeof(state->error), "%s", hub.device_error);
	snprintf(state->speaking, sizeof(state->speaking), "%s", hub.speaking);
	pthread_mutex_unlock(&hub.mutex);
}

void pl_intercom_get_devices(char *capture, size_t capture_size, char *playback, size_t playback_size, float *volume)
{
	pthread_mutex_lock(&hub.mutex);
	snprintf(capture, capture_size, "%s", hub.capture_name);
	snprintf(playback, playback_size, "%s", hub.playback_name);
	*volume = hub.volume;
	pthread_mutex_unlock(&hub.mutex);
}

void pl_intercom_set_devices(const char *capture, const char *playback, float volume)
{
	if (volume < 0.0f)
		volume = 0.0f;
	if (volume > 4.0f)
		volume = 4.0f;
	pthread_mutex_lock(&hub.mutex);
	bool changed = strcmp(hub.capture_name, capture) != 0 || strcmp(hub.playback_name, playback) != 0;
	snprintf(hub.capture_name, sizeof(hub.capture_name), "%s", capture);
	snprintf(hub.playback_name, sizeof(hub.playback_name), "%s", playback);
	hub.volume = volume;
	if (changed)
		hub.devices_dirty = true;
	pthread_mutex_unlock(&hub.mutex);
	save_settings(capture, playback, volume);
}
