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

#include "usb-relay.h"

#define USBMUXD_WINDOWS_PORT 27015
#ifndef USBMUXD_UNIX_PATH
#define USBMUXD_UNIX_PATH "/var/run/usbmuxd"
#endif
#define MUX_HEADER_SIZE 16
#define MUX_MAX_REPLY (256 * 1024)
#define MAX_DEVICES 8
#define PUMP_BUFFER (64 * 1024)

struct pl_usb_relay {
	pl_socket_t listen_sock;
	int local_port;
	char *name;
	const volatile bool *on_air;
	pthread_t thread;
	bool thread_created;
	volatile bool stop;
	char last_status[128];
};

/* ------------------------------------------------------------------------- */
/* Ajudantes                                                                 */

static void set_status(struct pl_usb_relay *relay, const char *status)
{
	if (strcmp(relay->last_status, status) == 0)
		return;
	snprintf(relay->last_status, sizeof(relay->last_status), "%s", status);
	blog(LOG_INFO, "[projeto-live] '%s' (cabo USB): %s", relay->name, status);
}

/* ------------------------------------------------------------------------- */
/* Conversa com o usbmuxd (mensagens "plist" em XML)                         */

static pl_socket_t connect_usbmuxd(void)
{
#ifdef _WIN32
	pl_socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == PL_INVALID_SOCKET)
		return PL_INVALID_SOCKET;
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
	if (sock == PL_INVALID_SOCKET)
		return PL_INVALID_SOCKET;
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
	if (pl_wait_readable(sock, timeout_ms) != 1)
		return NULL;

	unsigned char header[MUX_HEADER_SIZE];
	if (!pl_recv_all(sock, (char *)header, sizeof(header)))
		return NULL;
	uint32_t length = get_le32(header);
	if (length < MUX_HEADER_SIZE || length > MUX_MAX_REPLY)
		return NULL;

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
	if (!found)
		return NULL;
	const char *tag = strstr(found, "<integer>");
	if (!tag)
		return NULL;
	*value = strtoll(tag + strlen("<integer>"), NULL, 10);
	return tag;
}

/* Lista os aparelhos ligados (IDs do usbmuxd). */
static size_t list_devices(long long *ids, size_t max)
{
	pl_socket_t sock = connect_usbmuxd();
	if (sock == PL_INVALID_SOCKET)
		return (size_t)-1;

	size_t count = 0;
	if (send_plist(sock, "\t<key>MessageType</key>\n\t<string>ListDevices</string>\n")) {
		char *xml = read_plist(sock, 3000);
		const char *cursor = xml;
		long long id;
		while (cursor && count < max && (cursor = find_integer(cursor, "DeviceID", &id)) != NULL) {
			bool known = false;
			for (size_t i = 0; i < count; i++)
				known = known || ids[i] == id;
			if (!known)
				ids[count++] = id;
			cursor++;
		}
		bfree(xml);
	}
	pl_close_socket(sock);
	return count;
}

/* Liga para a porta do app no iPhone pelo cabo. Devolve o túnel (socket) ou PL_INVALID_SOCKET. */
static pl_socket_t connect_phone(struct pl_usb_relay *relay)
{
	long long ids[MAX_DEVICES];
	size_t count = list_devices(ids, MAX_DEVICES);
	if (count == (size_t)-1) {
		set_status(
			relay,
			"serviço da Apple (Apple Mobile Device) não encontrado; instale o iTunes ou o app Dispositivos Apple");
		return PL_INVALID_SOCKET;
	}
	if (count == 0) {
		set_status(relay, "nenhum iPhone ligado no cabo");
		return PL_INVALID_SOCKET;
	}

	/* A porta vai em "ordem de rede" dentro do número. */
	int port = ((PL_USB_PHONE_PORT & 0xFF) << 8) | ((PL_USB_PHONE_PORT >> 8) & 0xFF);

	for (size_t i = 0; i < count; i++) {
		pl_socket_t sock = connect_usbmuxd();
		if (sock == PL_INVALID_SOCKET)
			return PL_INVALID_SOCKET;

		char keys[256];
		snprintf(keys, sizeof(keys),
			 "\t<key>MessageType</key>\n\t<string>Connect</string>\n"
			 "\t<key>DeviceID</key>\n\t<integer>%lld</integer>\n"
			 "\t<key>PortNumber</key>\n\t<integer>%d</integer>\n",
			 ids[i], port);

		long long result = -1;
		if (send_plist(sock, keys)) {
			char *xml = read_plist(sock, 3000);
			if (xml)
				find_integer(xml, "Number", &result);
			bfree(xml);
		}

		if (result == 0) {
			set_status(relay, "recebendo a imagem do iPhone");
			return sock;
		}
		pl_close_socket(sock);
		set_status(relay, result == 3 ? "iPhone no cabo, mas o app Projeto Live não está aberto"
					      : "o iPhone não aceitou a conexão pelo cabo");
	}
	return PL_INVALID_SOCKET;
}

/* ------------------------------------------------------------------------- */
/* Relé                                                                      */

/* Avisa o app, pelo mesmo túnel do cabo, se esta câmera está no ar ("TALLY on" / "TALLY off"). */
static bool send_tally(struct pl_usb_relay *relay, pl_socket_t phone, int *sent)
{
	int tally = relay->on_air && os_atomic_load_bool(relay->on_air) ? 1 : 0;
	if (tally == *sent)
		return true;
	*sent = tally;
	const char *line = tally ? "TALLY on\n" : "TALLY off\n";
	return pl_send_all(phone, line, strlen(line));
}

/* Repassa o vídeo do iPhone para a Fonte de mídia até um dos lados fechar. */
static void pump(struct pl_usb_relay *relay, pl_socket_t phone, pl_socket_t media)
{
	char *buffer = bmalloc(PUMP_BUFFER);
	int sent_tally = -1;
	while (!relay->stop) {
		if (!send_tally(relay, phone, &sent_tally))
			break;

		int ready = pl_wait_readable(phone, 100);
		if (ready < 0)
			break;
		if (ready == 0) {
			/* Confere se a Fonte de mídia desistiu (ela não manda nada; recv 0 = fechou). */
			if (pl_wait_readable(media, 0) == 1) {
				char probe;
				if (recv(media, &probe, 1, 0) <= 0)
					break;
			}
			continue;
		}
		int got = (int)recv(phone, buffer, PUMP_BUFFER, 0);
		if (got <= 0 || !pl_send_all(media, buffer, (size_t)got))
			break;
	}
	bfree(buffer);
}

static void *relay_thread(void *data)
{
	struct pl_usb_relay *relay = data;
	os_set_thread_name("projeto-live: cabo USB");

	while (!relay->stop) {
		if (pl_wait_readable(relay->listen_sock, 200) != 1)
			continue;

		pl_socket_t media = accept(relay->listen_sock, NULL, NULL);
		if (media == PL_INVALID_SOCKET)
			continue;
		pl_no_sigpipe(media);

		pl_socket_t phone = connect_phone(relay);
		if (phone != PL_INVALID_SOCKET) {
			pl_no_sigpipe(phone);
			pl_no_delay(phone);
			pump(relay, phone, media);
			pl_close_socket(phone);
			set_status(relay, "conexão pelo cabo encerrada");
		}
		/* Sem iPhone: fecha, e a Fonte de mídia tenta de novo em 1 segundo. */
		pl_close_socket(media);
	}
	return NULL;
}

struct pl_usb_relay *pl_usb_relay_create(const char *source_name, const volatile bool *on_air)
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

	if (pthread_create(&relay->thread, NULL, relay_thread, relay) == 0) {
		relay->thread_created = true;
	} else {
		pl_close_socket(sock);
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

void pl_usb_relay_destroy(struct pl_usb_relay *relay)
{
	if (!relay)
		return;
	relay->stop = true;
	if (relay->thread_created)
		pthread_join(relay->thread, NULL);
	pl_close_socket(relay->listen_sock);
	bfree(relay->name);
	bfree(relay);
	pl_socket_cleanup();
}
