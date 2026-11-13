/*
 * Projeto Live: imagem do celular (iPhone ou Android) pelo cabo USB (veja usb-relay.h).
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
#include <util/pipe.h>
#include <util/platform.h>
#include <util/threading.h>

#include "intercom-stream.h"
#include "pl-clock.h"
#include "usb-relay.h"

#define USBMUXD_WINDOWS_PORT 27015
#define ADB_PORT 5037
#define ADB_START_RETRY_NS (15 * SECOND_NS)
#define ADB_SERIAL_MAX 96
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

/* Um celular no cabo: iPhone (pelo usbmuxd da Apple) ou Android (pelo adb do Google). */
enum phone_kind { PHONE_IPHONE, PHONE_ANDROID };

struct phone_dev {
	enum phone_kind kind;
	long long id; /* iPhone: o número do usbmuxd; Android: um número tirado do serial */
	char serial[ADB_SERIAL_MAX];
};

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

/* ------------------------------------------------------------------------- */
/* Android: o servidor do adb (127.0.0.1:5037)                               */
/*
 * Conversa do adb: cada pedido vai como 4 dígitos hexadecimais com o tamanho + o texto; a
 * resposta começa com "OKAY" ou "FAIL". "host:devices" lista os aparelhos; "host:transport:<serial>"
 * escolhe um; depois "tcp:<porta>" vira um túnel direto até essa porta dentro do celular.
 * Se o servidor não estiver rodando, o plugin liga o adb que vem com o Projeto Live.
 */

static uint64_t adb_next_start_ns;

static pl_socket_t adb_connect_once(void)
{
	pl_socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == PL_INVALID_SOCKET) {
		return PL_INVALID_SOCKET;
	}
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(ADB_PORT);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		pl_close_socket(sock);
		return PL_INVALID_SOCKET;
	}
	pl_no_sigpipe(sock);
	return sock;
}

/* Liga o servidor do adb ("adb start-server"), no máximo a cada 15 segundos. */
static void adb_start_server(void)
{
	uint64_t now = os_gettime_ns();
	if (now < adb_next_start_ns) {
		return;
	}
	adb_next_start_ns = now + ADB_START_RETRY_NS;

	struct dstr cmd = {0};
#ifdef _WIN32
	char *path = obs_module_file("adb/adb.exe");
	if (!path) {
		return;
	}
	dstr_printf(&cmd, "\"%s\" start-server", path);
	bfree(path);
#else
	dstr_copy(&cmd, "adb start-server");
#endif
	os_process_pipe_t *pipe = os_process_pipe_create(cmd.array, "r");
	dstr_free(&cmd);
	if (!pipe) {
		return;
	}
	/* Não lê a saída: o servidor do adb que fica rodando pode herdar a ponta do cano e a leitura
	 * nunca terminaria. Só espera o "adb start-server" sair (ele sai quando o servidor está pronto). */
	os_process_pipe_destroy(pipe);
}

static pl_socket_t adb_connect(void)
{
	pl_socket_t sock = adb_connect_once();
	if (sock == PL_INVALID_SOCKET) {
		adb_start_server();
		sock = adb_connect_once();
	}
	return sock;
}

static bool adb_request(pl_socket_t sock, const char *text)
{
	char header[5];
	size_t len = strlen(text);
	if (len > 0xFFFF) {
		return false;
	}
	snprintf(header, sizeof(header), "%04x", (unsigned)len);
	if (!pl_send_all(sock, header, 4) || !pl_send_all(sock, text, len)) {
		return false;
	}
	if (pl_wait_readable(sock, 3000) != 1) {
		return false;
	}
	char status[4];
	return pl_recv_all(sock, status, 4) && memcmp(status, "OKAY", 4) == 0;
}

static long long adb_id(const char *serial)
{
	/* Um número estável para o serial (para não pegar o mesmo Android em duas fontes). */
	unsigned long long hash = 1469598103934665603ULL;
	for (const char *c = serial; *c; c++) {
		hash = (hash ^ (unsigned char)*c) * 1099511628211ULL;
	}
	return (long long)((hash & 0x3FFFFFFFFFFFFFFFULL) | 0x4000000000000000ULL);
}

/* Lista os Androids. Devolve quantos estão prontos; `unauthorized` conta os que ainda não
 * aceitaram este computador. (size_t)-1 = sem adb. */
static size_t adb_list(struct phone_dev *devs, size_t max, size_t *unauthorized)
{
	*unauthorized = 0;
	pl_socket_t sock = adb_connect();
	if (sock == PL_INVALID_SOCKET) {
		return (size_t)-1;
	}
	size_t count = 0;
	char size_text[5] = {0};
	if (adb_request(sock, "host:devices") && pl_wait_readable(sock, 3000) == 1 && pl_recv_all(sock, size_text, 4)) {
		size_t size = (size_t)strtoul(size_text, NULL, 16);
		char *list = bzalloc(size + 1);
		if (size == 0 || pl_recv_all(sock, list, size)) {
			char *line = list;
			while (line && *line) {
				char *next = strchr(line, '\n');
				if (next) {
					*next++ = 0;
				}
				char *tab = strchr(line, '\t');
				if (tab) {
					*tab = 0;
					const char *state = tab + 1;
					if (strncmp(state, "device", 6) == 0 && count < max &&
					    strlen(line) < ADB_SERIAL_MAX) {
						devs[count].kind = PHONE_ANDROID;
						snprintf(devs[count].serial, ADB_SERIAL_MAX, "%s", line);
						devs[count].id = adb_id(line);
						count++;
					} else if (strncmp(state, "unauthorized", 12) == 0) {
						(*unauthorized)++;
					}
				}
				line = next;
			}
		}
		bfree(list);
	}
	pl_close_socket(sock);
	return count;
}

/* Túnel pelo cabo até a porta `port` do app no Android `serial`. */
static pl_socket_t adb_connect_port(const char *serial, int port)
{
	pl_socket_t sock = adb_connect();
	if (sock == PL_INVALID_SOCKET) {
		return PL_INVALID_SOCKET;
	}
	char text[ADB_SERIAL_MAX + 32];
	snprintf(text, sizeof(text), "host:transport:%s", serial);
	if (!adb_request(sock, text)) {
		pl_close_socket(sock);
		return PL_INVALID_SOCKET;
	}
	snprintf(text, sizeof(text), "tcp:%d", port);
	if (!adb_request(sock, text)) {
		pl_close_socket(sock);
		return PL_INVALID_SOCKET;
	}
	pl_no_delay(sock);
	return sock;
}

/* Abre o túnel até a porta `port` do app no celular `dev` (iPhone ou Android). */
static pl_socket_t connect_dev(const struct phone_dev *dev, int port)
{
	if (dev->kind == PHONE_ANDROID) {
		return adb_connect_port(dev->serial, port);
	}
	long long result;
	return connect_device(dev->id, port, &result);
}

/* Liga para o vídeo do app num celular do cabo (iPhone ou Android) que nenhuma outra fonte esteja
 * usando. Devolve o túnel (socket) e o celular escolhido, ou PL_INVALID_SOCKET. */
static pl_socket_t connect_phone(struct pl_usb_relay *relay, struct phone_dev *chosen)
{
	struct phone_dev devs[MAX_DEVICES * 2];
	size_t count = 0;

	long long ids[MAX_DEVICES];
	size_t iphones = list_devices(ids, MAX_DEVICES);
	bool apple_service = iphones != (size_t)-1;
	for (size_t i = 0; apple_service && i < iphones; i++) {
		devs[count].kind = PHONE_IPHONE;
		devs[count].id = ids[i];
		devs[count].serial[0] = 0;
		count++;
	}

	size_t unauthorized = 0;
	size_t androids = adb_list(devs + count, MAX_DEVICES, &unauthorized);
	bool adb_service = androids != (size_t)-1;
	if (adb_service) {
		count += androids;
	}

	if (count == 0) {
		if (unauthorized > 0) {
			set_status(relay,
				   "Android no cabo: no celular, toque em \"Permitir\" na pergunta da depuração USB");
		} else if (!apple_service && !adb_service) {
			set_status(relay,
				   "nenhum celular no cabo (iPhone: precisa do iTunes ou do app Dispositivos Apple; "
				   "Android: ligue a Depuração USB)");
		} else {
			set_status(
				relay,
				"nenhum celular no cabo (Android: ligue a Depuração USB nas Opções do desenvolvedor)");
		}
		return PL_INVALID_SOCKET;
	}

	bool all_busy = true;
	for (size_t i = 0; i < count; i++) {
		if (!claim_device(devs[i].id)) {
			continue;
		}
		all_busy = false;

		pl_socket_t sock = PL_INVALID_SOCKET;
		long long result = -1;
		if (devs[i].kind == PHONE_IPHONE) {
			sock = connect_device(devs[i].id, PL_USB_PHONE_PORT, &result);
		} else {
			sock = adb_connect_port(devs[i].serial, PL_USB_PHONE_PORT);
		}
		if (sock != PL_INVALID_SOCKET) {
			set_status(relay, devs[i].kind == PHONE_IPHONE ? "recebendo a imagem do iPhone"
								       : "recebendo a imagem do Android");
			*chosen = devs[i];
			return sock;
		}
		release_device(devs[i].id);
		if (devs[i].kind == PHONE_IPHONE) {
			set_status(relay, result == 3 ? "iPhone no cabo, mas o app Projeto Live não está aberto"
						      : "o iPhone não aceitou a conexão pelo cabo");
		} else {
			set_status(relay, "Android no cabo, mas o app Projeto Live não está aberto");
		}
	}
	if (all_busy) {
		set_status(relay, "todos os celulares do cabo já estão em outras fontes \"Celular\"");
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
static void pump(struct pl_usb_relay *relay, pl_socket_t phone, pl_socket_t media, const struct phone_dev *dev)
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
			control.sock = connect_dev(dev, PL_USB_CONTROL_PORT);
			lines.used = 0;
			next_control_ns = now + CONTROL_RETRY_NS;
			voice_pending = true;
		}

		/* Intercom: um terceiro túnel, só de voz (o app 0.8 em diante escuta na porta 9012). */
		if (relay->voice && now >= next_voice_ns && !pl_voice_peer_has_stream(relay->voice)) {
			pl_socket_t voice = connect_dev(dev, PL_USB_VOICE_PORT);
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

		struct phone_dev dev;
		memset(&dev, 0, sizeof(dev));
		pl_socket_t phone = connect_phone(relay, &dev);
		if (phone != PL_INVALID_SOCKET) {
			pump(relay, phone, media, &dev);
			pl_close_socket(phone);
			release_device(dev.id);
			set_status(relay, "conexão pelo cabo encerrada");
		}
		/* Sem celular: fecha, e a Fonte de mídia tenta de novo em 1 segundo. */
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
