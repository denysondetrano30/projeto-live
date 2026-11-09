/*
 * Projeto Live: imagem do iPhone pelo cabo USB (veja usb-relay.h).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "pl-socket.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <obs-module.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include "intercom-stream.h"
#include "pl-clock.h"
#include "usb-relay.h"

#define USBMUXD_WINDOWS_PORT 27015
#ifndef USBMUXD_UNIX_PATH
#define USBMUXD_UNIX_PATH "/var/run/usbmuxd"
#endif
#define MUX_HEADER_SIZE 16
#define MUX_MAX_REPLY (256 * 1024)
#define MAX_DEVICES 8
#define PUMP_BUFFER (64 * 1024)
#define WAIT_MS 100
#define SECOND_NS 1000000000ULL
#define CONTROL_RETRY_NS (5 * SECOND_NS)
#define PING_INTERVAL_NS (2 * SECOND_NS)
#define TIME_INTERVAL_NS (1 * SECOND_NS)

struct pl_usb_relay {
	pl_socket_t listen_sock;
	int local_port;
	char *name;
	const volatile bool *on_air;
	struct pl_voice_peer *voice;
	struct pl_clock *clock;
	pthread_t thread;
	bool thread_created;
	volatile bool stop;
	char last_status[128];

	/* Compartilhado com a thread da interface (painel do diretor). */
	pthread_mutex_t mutex;
	struct pl_link_status status;
	struct dstr outbox; /* linhas esperando para ir ao celular, cada uma com "\n" */
};

/* iPhones do cabo já em uso por alguma fonte: com dois iPhones ligados, cada fonte pega um. */
static pthread_mutex_t claimed_mutex = PTHREAD_MUTEX_INITIALIZER;
static long long claimed_ids[MAX_DEVICES];
static size_t claimed_count;

static bool claim_device(long long id)
{
	bool ok = true;
	pthread_mutex_lock(&claimed_mutex);
	for (size_t i = 0; i < claimed_count; i++) {
		ok = ok && claimed_ids[i] != id;
	}
	if (ok && claimed_count < MAX_DEVICES) {
		claimed_ids[claimed_count++] = id;
	} else {
		ok = false;
	}
	pthread_mutex_unlock(&claimed_mutex);
	return ok;
}

static void release_device(long long id)
{
	pthread_mutex_lock(&claimed_mutex);
	for (size_t i = 0; i < claimed_count; i++) {
		if (claimed_ids[i] == id) {
			claimed_ids[i] = claimed_ids[--claimed_count];
			break;
		}
	}
	pthread_mutex_unlock(&claimed_mutex);
}

/* ------------------------------------------------------------------------- */
/* Ajudantes                                                                 */

static void set_status(struct pl_usb_relay *relay, const char *status)
{
	if (strcmp(relay->last_status, status) == 0) {
		return;
	}
	snprintf(relay->last_status, sizeof(relay->last_status), "%s", status);
	blog(LOG_INFO, "[projeto-live] '%s' (cabo USB): %s", relay->name, status);
}

/* ------------------------------------------------------------------------- */
/* Conversa com o usbmuxd (mensagens "plist" em XML)                         */

static pl_socket_t connect_usbmuxd(void)
{
#ifdef _WIN32
	pl_socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == PL_INVALID_SOCKET) {
		return PL_INVALID_SOCKET;
	}
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(USBMUXD_WINDOWS_PORT);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		pl_close_socket(sock);
		return PL_INVALID_SOCKET;
	}
	return sock;
#else
	pl_socket_t sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock == PL_INVALID_SOCKET) {
		return PL_INVALID_SOCKET;
	}
	pl_no_sigpipe(sock);
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", USBMUXD_UNIX_PATH);
	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		pl_close_socket(sock);
		return PL_INVALID_SOCKET;
	}
	return sock;
#endif
}

static void put_le32(unsigned char *out, uint32_t value)
{
	out[0] = (unsigned char)(value & 0xFF);
	out[1] = (unsigned char)((value >> 8) & 0xFF);
	out[2] = (unsigned char)((value >> 16) & 0xFF);
	out[3] = (unsigned char)((value >> 24) & 0xFF);
}

static uint32_t get_le32(const unsigned char *in)
{
	return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static bool send_plist(pl_socket_t sock, const char *body_keys)
{
	struct dstr plist = {0};
	dstr_cat(&plist, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
			 "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
			 "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
			 "<plist version=\"1.0\">\n<dict>\n"
			 "\t<key>ClientVersionString</key>\n\t<string>projeto-live</string>\n"
			 "\t<key>ProgName</key>\n\t<string>projeto-live</string>\n"
			 "\t<key>kLibUSBMuxVersion</key>\n\t<integer>3</integer>\n");
	dstr_cat(&plist, body_keys);
	dstr_cat(&plist, "</dict>\n</plist>\n");

	unsigned char header[MUX_HEADER_SIZE];
	put_le32(header, (uint32_t)(MUX_HEADER_SIZE + plist.len));
	put_le32(header + 4, 1);  /* versão: plist */
	put_le32(header + 8, 8);  /* mensagem: plist */
	put_le32(header + 12, 1); /* etiqueta */

	bool ok = pl_send_all(sock, (const char *)header, sizeof(header)) && pl_send_all(sock, plist.array, plist.len);
	dstr_free(&plist);
	return ok;
}

/* Lê uma resposta. Devolve o XML (liberar com bfree) ou NULL. */
static char *read_plist(pl_socket_t sock, int timeout_ms)
{
	if (pl_wait_readable(sock, timeout_ms) != 1) {
		return NULL;
	}

	unsigned char header[MUX_HEADER_SIZE];
	if (!pl_recv_all(sock, (char *)header, sizeof(header))) {
		return NULL;
	}
	uint32_t length = get_le32(header);
	if (length < MUX_HEADER_SIZE || length > MUX_MAX_REPLY) {
		return NULL;
	}

	size_t body = length - MUX_HEADER_SIZE;
	char *xml = bmalloc(body + 1);
	if (!pl_recv_all(sock, xml, body)) {
		bfree(xml);
		return NULL;
	}
	xml[body] = 0;
	return xml;
}

/* Acha "<key>NOME</key>" seguido de "<integer>N</integer>" a partir de `from`. */
static const char *find_integer(const char *from, const char *key, long long *value)
{
	char needle[64];
	snprintf(needle, sizeof(needle), "<key>%s</key>", key);
	const char *found = strstr(from, needle);
	if (!found) {
		return NULL;
	}
	const char *tag = strstr(found, "<integer>");
	if (!tag) {
		return NULL;
	}
	*value = strtoll(tag + strlen("<integer>"), NULL, 10);
	return tag;
}

/* Lista os aparelhos ligados (IDs do usbmuxd). */
static size_t list_devices(long long *ids, size_t max)
{
	pl_socket_t sock = connect_usbmuxd();
	if (sock == PL_INVALID_SOCKET) {
		return (size_t)-1;
	}

	size_t count = 0;
	if (send_plist(sock, "\t<key>MessageType</key>\n\t<string>ListDevices</string>\n")) {
		char *xml = read_plist(sock, 3000);
		const char *cursor = xml;
		long long id;
		while (cursor && count < max && (cursor = find_integer(cursor, "DeviceID", &id)) != NULL) {
			bool known = false;
			for (size_t i = 0; i < count; i++) {
				known = known || ids[i] == id;
			}
			if (!known) {
				ids[count++] = id;
			}
			cursor++;
		}
		bfree(xml);
	}
	pl_close_socket(sock);
	return count;
}

/* Abre um túnel pelo cabo até a porta `port` do app no iPhone `id`. `result` recebe a resposta
 * do usbmuxd (0 = conectou, 3 = o app não está escutando nessa porta). */
static pl_socket_t connect_device(long long id, int port, long long *result)
{
	*result = -1;
	pl_socket_t sock = connect_usbmuxd();
	if (sock == PL_INVALID_SOCKET) {
		return PL_INVALID_SOCKET;
	}

	/* A porta vai em "ordem de rede" dentro do número. */
	int swapped = ((port & 0xFF) << 8) | ((port >> 8) & 0xFF);
	char keys[256];
	snprintf(keys, sizeof(keys),
		 "\t<key>MessageType</key>\n\t<string>Connect</string>\n"
		 "\t<key>DeviceID</key>\n\t<integer>%lld</integer>\n"
		 "\t<key>PortNumber</key>\n\t<integer>%d</integer>\n",
		 id, swapped);

	if (send_plist(sock, keys)) {
		char *xml = read_plist(sock, 3000);
		if (xml) {
			find_integer(xml, "Number", result);
		}
		bfree(xml);
	}

	if (*result != 0) {
		pl_close_socket(sock);
		return PL_INVALID_SOCKET;
	}
	pl_no_sigpipe(sock);
	pl_no_delay(sock);
	return sock;
}

/* Liga para o vídeo do app num iPhone do cabo que nenhuma outra fonte esteja usando.
 * Devolve o túnel (socket) e o iPhone escolhido, ou PL_INVALID_SOCKET. */
static pl_socket_t connect_phone(struct pl_usb_relay *relay, long long *device_id)
{
	long long ids[MAX_DEVICES];
	size_t count = list_devices(ids, MAX_DEVICES);
	if (count == (size_t)-1) {
		set_status(relay, "serviço da Apple (Apple Mobile Device) não encontrado; instale o iTunes ou o "
				  "app Dispositivos Apple");
		return PL_INVALID_SOCKET;
	}
	if (count == 0) {
		set_status(relay, "nenhum iPhone ligado no cabo");
		return PL_INVALID_SOCKET;
	}

	bool all_busy = true;
	for (size_t i = 0; i < count; i++) {
		if (!claim_device(ids[i])) {
			continue;
		}
		all_busy = false;

		long long result;
		pl_socket_t sock = connect_device(ids[i], PL_USB_PHONE_PORT, &result);
		if (sock != PL_INVALID_SOCKET) {
			set_status(relay, "recebendo a imagem do iPhone");
			*device_id = ids[i];
			return sock;
		}
		release_device(ids[i]);
		set_status(relay, result == 3 ? "iPhone no cabo, mas o app Projeto Live não está aberto"
					      : "o iPhone não aceitou a conexão pelo cabo");
	}
	if (all_busy) {
		set_status(relay, "todos os iPhones do cabo já estão em outras fontes \"Celular\"");
	}
	return PL_INVALID_SOCKET;
}

/* ------------------------------------------------------------------------- */
/* Relé                                                                      */

static void publish(struct pl_usb_relay *relay, bool connected, const char *camera)
{
	pthread_mutex_lock(&relay->mutex);
	relay->status.connected = connected;
	snprintf(relay->status.camera, sizeof(relay->status.camera), "%s", camera ? camera : "");
	/* Comando que ficou na fila sem celular não vale para o próximo que conectar. */
	dstr_free(&relay->outbox);
	pthread_mutex_unlock(&relay->mutex);
}

/* Linhas que o app manda pelo túnel de controle: "HELLO usb <nome>", "ACK <número>", "PING". */
static bool handle_control_line(void *param, const char *line)
{
	struct pl_usb_relay *relay = param;
	if (pl_clock_handle_line(relay->clock, line)) {
		return true;
	}
	if (strncmp(line, "HELLO usb ", 10) == 0) {
		/* Pode ser outro iPhone: o relógio é medido de novo. */
		pl_clock_reset(relay->clock);
		publish(relay, true, line[10] ? line + 10 : "iPhone");
		blog(LOG_INFO, "[projeto-live] '%s' (cabo USB): %s pronto para receber os avisos do diretor",
		     relay->name, line + 10);
	} else if (strncmp(line, "ACK ", 4) == 0) {
		uint32_t id = (uint32_t)strtoul(line + 4, NULL, 10);
		pthread_mutex_lock(&relay->mutex);
		if (id > relay->status.last_ack) {
			relay->status.last_ack = id;
		}
		pthread_mutex_unlock(&relay->mutex);
	}
	return true;
}

/* Manda ao app o que o painel do diretor deixou na fila. */
static bool flush_outbox(struct pl_usb_relay *relay, pl_socket_t control)
{
	struct dstr pending = {0};
	pthread_mutex_lock(&relay->mutex);
	pending = relay->outbox;
	memset(&relay->outbox, 0, sizeof(relay->outbox));
	pthread_mutex_unlock(&relay->mutex);

	bool ok = !pending.len || pl_send_all(control, pending.array, pending.len);
	dstr_free(&pending);
	return ok;
}

struct tunnel {
	pl_socket_t sock;
	int sent_tally; /* -1 = ainda não mandou, 0 = fora do ar, 1 = no ar */
};

/* Avisa o app se esta câmera está no ar ("TALLY on" / "TALLY off"), só quando muda. */
static bool send_tally(struct pl_usb_relay *relay, struct tunnel *tunnel)
{
	int tally = relay->on_air && os_atomic_load_bool(relay->on_air) ? 1 : 0;
	if (tunnel->sock == PL_INVALID_SOCKET || tally == tunnel->sent_tally) {
		return true;
	}
	tunnel->sent_tally = tally;
	return pl_send_line(tunnel->sock, tally ? "TALLY on" : "TALLY off");
}

static void close_control(struct pl_usb_relay *relay, struct tunnel *control)
{
	if (control->sock == PL_INVALID_SOCKET) {
		return;
	}
	pl_close_socket(control->sock);
	control->sock = PL_INVALID_SOCKET;
	control->sent_tally = -1;
	publish(relay, false, NULL);
}

/* Repassa o vídeo do iPhone para a Fonte de mídia até um dos lados fechar. Ao mesmo tempo,
 * mantém o túnel de controle (porta 9011): "no ar", avisos do diretor e as confirmações. */
static void pump(struct pl_usb_relay *relay, pl_socket_t phone, pl_socket_t media, long long device_id)
{
	char *buffer = bmalloc(PUMP_BUFFER);
	struct tunnel video = {phone, -1};
	struct tunnel control = {PL_INVALID_SOCKET, -1};
	struct pl_line_buffer lines = {{0}, 0};
	uint64_t next_control_ns = 0;
	uint64_t next_voice_ns = 0;
	uint64_t last_ping_ns = 0;
	uint64_t last_time_ns = 0;
	uint32_t sent_voice = 0;
	bool voice_pending = true;

	while (!relay->stop) {
		uint64_t now = os_gettime_ns();

		/* Apps antigos (até a versão 0.6) não têm a porta de controle: tenta de novo de vez em quando. */
		if (control.sock == PL_INVALID_SOCKET && now >= next_control_ns) {
			long long result;
			control.sock = connect_device(device_id, PL_USB_CONTROL_PORT, &result);
			lines.used = 0;
			next_control_ns = now + CONTROL_RETRY_NS;
			voice_pending = true;
		}

		/* Intercom: um terceiro túnel, só de voz (o app 0.8 em diante escuta na porta 9012). */
		if (relay->voice && now >= next_voice_ns && !pl_voice_peer_has_stream(relay->voice)) {
			long long result;
			pl_socket_t voice = connect_device(device_id, PL_USB_VOICE_PORT, &result);
			if (voice != PL_INVALID_SOCKET) {
				pl_voice_peer_attach_stream(relay->voice, voice);
			}
			next_voice_ns = now + CONTROL_RETRY_NS;
		}

		/* O "no ar" vai pelos dois túneis: o de vídeo serve para o app 0.6. */
		if (!send_tally(relay, &video)) {
			break;
		}
		if (!send_tally(relay, &control) ||
		    (control.sock != PL_INVALID_SOCKET && !flush_outbox(relay, control.sock))) {
			close_control(relay, &control);
		}
		if (control.sock != PL_INVALID_SOCKET && now - last_ping_ns >= PING_INTERVAL_NS) {
			last_ping_ns = now;
			if (!pl_send_line(control.sock, "PING")) {
				close_control(relay, &control);
			}
		}
		/* Sincronia das câmeras (veja pl-clock.h). */
		if (control.sock != PL_INVALID_SOCKET && relay->clock && now - last_time_ns >= TIME_INTERVAL_NS) {
			char line[48];
			last_time_ns = now;
			pl_clock_request(line, sizeof(line));
			if (!pl_send_line(control.sock, line)) {
				close_control(relay, &control);
			}
		}
		if (control.sock != PL_INVALID_SOCKET && relay->voice) {
			char text[PL_VOICE_TEXT_MAX], line[PL_VOICE_TEXT_MAX + 8];
			uint32_t serial = pl_voice_peer_voice_text(relay->voice, text, sizeof(text));
			if (voice_pending || serial != sent_voice) {
				snprintf(line, sizeof(line), "VOICE %s", text[0] ? text : "-");
				if (!pl_send_line(control.sock, line)) {
					close_control(relay, &control);
				}
				sent_voice = serial;
				voice_pending = false;
			}
		}

		fd_set readable;
		FD_ZERO(&readable);
		FD_SET(phone, &readable);
		int max_fd = (int)phone;
		if (control.sock != PL_INVALID_SOCKET) {
			FD_SET(control.sock, &readable);
			if ((int)control.sock > max_fd) {
				max_fd = (int)control.sock;
			}
		}
		struct timeval timeout = {0, WAIT_MS * 1000};
		int ready = select(max_fd + 1, &readable, NULL, NULL, &timeout);
		if (ready < 0) {
			break;
		}

		if (ready > 0 && control.sock != PL_INVALID_SOCKET && FD_ISSET(control.sock, &readable)) {
			if (!pl_read_lines(control.sock, &lines, handle_control_line, relay)) {
				close_control(relay, &control);
			}
		}

		if (ready > 0 && FD_ISSET(phone, &readable)) {
			int got = (int)recv(phone, buffer, PUMP_BUFFER, 0);
			if (got <= 0 || !pl_send_all(media, buffer, (size_t)got)) {
				break;
			}
		} else if (pl_wait_readable(media, 0) == 1) {
			/* Confere se a Fonte de mídia desistiu (ela não manda nada; recv 0 = fechou). */
			char probe;
			if (recv(media, &probe, 1, 0) <= 0) {
				break;
			}
		}
	}

	close_control(relay, &control);
	pl_voice_peer_detach_stream(relay->voice);
	bfree(buffer);
}

static void *relay_thread(void *data)
{
	struct pl_usb_relay *relay = data;
	os_set_thread_name("projeto-live: cabo USB");

	while (!relay->stop) {
		if (pl_wait_readable(relay->listen_sock, 200) != 1) {
			continue;
		}

		pl_socket_t media = accept(relay->listen_sock, NULL, NULL);
		if (media == PL_INVALID_SOCKET) {
			continue;
		}
		pl_no_sigpipe(media);

		long long device_id = 0;
		pl_socket_t phone = connect_phone(relay, &device_id);
		if (phone != PL_INVALID_SOCKET) {
			pump(relay, phone, media, device_id);
			pl_close_socket(phone);
			release_device(device_id);
			set_status(relay, "conexão pelo cabo encerrada");
		}
		/* Sem iPhone: fecha, e a Fonte de mídia tenta de novo em 1 segundo. */
		pl_close_socket(media);
	}
	return NULL;
}

struct pl_usb_relay *pl_usb_relay_create(const char *source_name, const volatile bool *on_air,
					 struct pl_voice_peer *voice, struct pl_clock *clock)
{
	pl_socket_startup();

	pl_socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == PL_INVALID_SOCKET) {
		pl_socket_cleanup();
		return NULL;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = 0;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t len = sizeof(addr);
	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(sock, 1) != 0 ||
	    getsockname(sock, (struct sockaddr *)&addr, &len) != 0) {
		pl_close_socket(sock);
		pl_socket_cleanup();
		return NULL;
	}

	struct pl_usb_relay *relay = bzalloc(sizeof(struct pl_usb_relay));
	relay->listen_sock = sock;
	relay->local_port = ntohs(addr.sin_port);
	relay->name = bstrdup(source_name);
	relay->on_air = on_air;
	relay->voice = voice;
	relay->clock = clock;
	pthread_mutex_init(&relay->mutex, NULL);

	if (pthread_create(&relay->thread, NULL, relay_thread, relay) == 0) {
		relay->thread_created = true;
	} else {
		pl_close_socket(sock);
		pthread_mutex_destroy(&relay->mutex);
		bfree(relay->name);
		bfree(relay);
		pl_socket_cleanup();
		return NULL;
	}
	return relay;
}

int pl_usb_relay_local_port(const struct pl_usb_relay *relay)
{
	return relay ? relay->local_port : 0;
}

bool pl_usb_relay_send(struct pl_usb_relay *relay, const char *line)
{
	if (!relay || !line || !*line) {
		return false;
	}

	pthread_mutex_lock(&relay->mutex);
	bool connected = relay->status.connected;
	if (connected) {
		dstr_cat(&relay->outbox, line);
		dstr_cat(&relay->outbox, "\n");
	}
	pthread_mutex_unlock(&relay->mutex);
	return connected;
}

void pl_usb_relay_get_status(struct pl_usb_relay *relay, struct pl_link_status *status)
{
	memset(status, 0, sizeof(*status));
	if (!relay) {
		return;
	}
	pthread_mutex_lock(&relay->mutex);
	*status = relay->status;
	pthread_mutex_unlock(&relay->mutex);
}

void pl_usb_relay_destroy(struct pl_usb_relay *relay)
{
	if (!relay) {
		return;
	}
	relay->stop = true;
	if (relay->thread_created) {
		pthread_join(relay->thread, NULL);
	}
	pl_close_socket(relay->listen_sock);
	dstr_free(&relay->outbox);
	pthread_mutex_destroy(&relay->mutex);
	bfree(relay->name);
	bfree(relay);
	pl_socket_cleanup();
}
