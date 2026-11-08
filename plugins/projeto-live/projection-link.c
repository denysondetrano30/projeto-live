/*
 * Projeto Live: ligação com o app de projeção (veja projection-link.h).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#ifdef _WIN32
/* rand_s: números aleatórios do sistema para as chaves. */
#define _CRT_RAND_S
#endif

#include <stdlib.h>
#include <stdio.h>

#include "pl-socket.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include "mdns-announce.h"
#include "projection-link.h"
#include "sha256.h"

#define FIRST_PORT 9020
#define LAST_PORT 9029
#define MAX_PEERS 4
#define MAX_CLIENTS 32
#define ID_MAX 64
#define KEY_CHARS 64
#define NONCE_CHARS 32
#define WAIT_MS 100
#define SECOND_NS 1000000000ULL
#define HELLO_TIMEOUT_NS (5 * SECOND_NS)
#define APPROVAL_TIMEOUT_NS (60 * SECOND_NS)
#define SILENCE_TIMEOUT_NS (6 * SECOND_NS)
#define PING_INTERVAL_NS (2 * SECOND_NS)
#define BIND_RETRY_NS (5 * SECOND_NS)
#define TRUST_FILE "projecao.json"

enum peer_state {
	PEER_EMPTY,
	PEER_HELLO,   /* esperando o "HELLO" */
	PEER_WAITING, /* esperando a pessoa responder "Permitir?" */
	PEER_READY,   /* conectado */
};

struct peer {
	enum peer_state state;
	pl_socket_t sock;
	struct pl_line_buffer lines;
	uint64_t since_ns;
	uint64_t last_recv_ns;
	uint64_t last_send_ns;
	char nonce[NONCE_CHARS + 1];
	char id[ID_MAX];
	char name[PL_PROJECTION_NAME_MAX];
	unsigned int ticket;
};

struct client {
	char id[ID_MAX];
	char name[PL_PROJECTION_NAME_MAX];
	char key[KEY_CHARS + 1];
};

static pthread_t thread;
static bool thread_created;
static volatile bool stop_thread;
static pthread_mutex_t mutex;

/* Protegidos por `mutex`. */
static char obs_id[ID_MAX];
static struct client clients[MAX_CLIENTS];
static int client_count;
static char connected_names[512];
static int connected_count;
static unsigned int answer_ticket;
static int answer_value; /* 1 = sim, -1 = não, 0 = sem resposta */
static bool forget_requested;

static unsigned int next_ticket;
static uint64_t nonce_counter;

/* ------------------------------------------------------------------------- */
/* Números aleatórios e chaves                                               */

static void random_bytes(uint8_t *out, size_t size)
{
	size_t filled = 0;
#ifdef _WIN32
	while (filled < size) {
		unsigned int value = 0;
		if (rand_s(&value) != 0) {
			break;
		}
		for (size_t i = 0; i < sizeof(value) && filled < size; i++) {
			out[filled++] = (uint8_t)(value >> (8 * i));
		}
	}
#else
	FILE *source = fopen("/dev/urandom", "rb");
	if (source) {
		filled = fread(out, 1, size, source);
		fclose(source);
	}
#endif
	/* Sem fonte do sistema (não deveria acontecer): mistura relógio e contador. */
	while (filled < size) {
		uint64_t mix = os_gettime_ns() ^ ((uint64_t)filled * 0x9E3779B97F4A7C15ULL);
		uint8_t digest[PL_SHA256_SIZE];
		struct pl_sha256 ctx;
		pl_sha256_init(&ctx);
		pl_sha256_update(&ctx, &mix, sizeof(mix));
		pl_sha256_update(&ctx, out, size);
		pl_sha256_final(&ctx, digest);
		out[filled] = digest[0];
		filled++;
	}
}

static void random_hex(char *out, size_t chars)
{
	uint8_t bytes[64];
	size_t count = chars / 2;
	if (count > sizeof(bytes)) {
		count = sizeof(bytes);
	}
	random_bytes(bytes, count);
	pl_hex(bytes, count, out);
	out[count * 2] = 0;
}

static void make_nonce(char *out)
{
	uint8_t seed[16];
	random_bytes(seed, sizeof(seed));
	uint64_t counter = ++nonce_counter;
	uint8_t digest[PL_SHA256_SIZE];
	struct pl_sha256 ctx;
	pl_sha256_init(&ctx);
	pl_sha256_update(&ctx, seed, sizeof(seed));
	pl_sha256_update(&ctx, &counter, sizeof(counter));
	pl_sha256_final(&ctx, digest);
	pl_hex(digest, NONCE_CHARS / 2, out);
	out[NONCE_CHARS] = 0;
}

/* Compara sem parar no primeiro caractere diferente (não entrega pistas pelo tempo). */
static bool same_text(const char *a, const char *b)
{
	size_t len = strlen(a);
	if (len != strlen(b)) {
		return false;
	}
	unsigned char diff = 0;
	for (size_t i = 0; i < len; i++) {
		diff |= (unsigned char)(a[i] ^ b[i]);
	}
	return diff == 0;
}

/* Texto que vem da rede: só o que cabe numa linha e sem caracteres de controle. */
static void copy_clean(char *out, size_t size, const char *text)
{
	size_t used = 0;
	for (; text && *text && used + 1 < size; text++) {
		unsigned char c = (unsigned char)*text;
		out[used++] = c < 32 ? ' ' : (char)c;
	}
	out[used] = 0;
}

/* ------------------------------------------------------------------------- */
/* Projeções pareadas (projecao.json, na pasta de configuração do plugin)    */

static void save_trust_locked(void)
{
	obs_data_t *root = obs_data_create();
	obs_data_set_string(root, "obs_id", obs_id);
	obs_data_array_t *list = obs_data_array_create();
	for (int i = 0; i < client_count; i++) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "id", clients[i].id);
		obs_data_set_string(item, "name", clients[i].name);
		obs_data_set_string(item, "key", clients[i].key);
		obs_data_array_push_back(list, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "clients", list);
	obs_data_array_release(list);

	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path(TRUST_FILE);
	if (path) {
		obs_data_save_json_safe(root, path, "tmp", "bak");
		bfree(path);
	}
	obs_data_release(root);
}

static void load_trust(void)
{
	pthread_mutex_lock(&mutex);
	client_count = 0;
	obs_id[0] = 0;
	char *path = obs_module_config_path(TRUST_FILE);
	obs_data_t *root = path ? obs_data_create_from_json_file_safe(path, "bak") : NULL;
	bfree(path);
	if (root) {
		copy_clean(obs_id, sizeof(obs_id), obs_data_get_string(root, "obs_id"));
		obs_data_array_t *list = obs_data_get_array(root, "clients");
		size_t count = obs_data_array_count(list);
		for (size_t i = 0; i < count && client_count < MAX_CLIENTS; i++) {
			obs_data_t *item = obs_data_array_item(list, i);
			struct client *entry = &clients[client_count];
			copy_clean(entry->id, sizeof(entry->id), obs_data_get_string(item, "id"));
			copy_clean(entry->name, sizeof(entry->name), obs_data_get_string(item, "name"));
			copy_clean(entry->key, sizeof(entry->key), obs_data_get_string(item, "key"));
			if (entry->id[0] && strlen(entry->key) == KEY_CHARS) {
				client_count++;
			}
			obs_data_release(item);
		}
		obs_data_array_release(list);
		obs_data_release(root);
	}
	if (!obs_id[0]) {
		random_hex(obs_id, 16);
		save_trust_locked();
	}
	pthread_mutex_unlock(&mutex);
}

/* Chave da projeção `id` (false se ela não foi pareada). */
static bool find_key(const char *id, char *key)
{
	bool found = false;
	pthread_mutex_lock(&mutex);
	for (int i = 0; i < client_count; i++) {
		if (strcmp(clients[i].id, id) == 0) {
			snprintf(key, KEY_CHARS + 1, "%s", clients[i].key);
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&mutex);
	return found;
}

static void add_client(const char *id, const char *name, const char *key)
{
	pthread_mutex_lock(&mutex);
	int slot = -1;
	for (int i = 0; i < client_count; i++) {
		if (strcmp(clients[i].id, id) == 0) {
			slot = i;
		}
	}
	if (slot < 0) {
		if (client_count == MAX_CLIENTS) {
			/* Lista cheia: sai a mais antiga. */
			memmove(&clients[0], &clients[1], sizeof(clients[0]) * (MAX_CLIENTS - 1));
			client_count--;
		}
		slot = client_count++;
	}
	snprintf(clients[slot].id, sizeof(clients[slot].id), "%s", id);
	snprintf(clients[slot].name, sizeof(clients[slot].name), "%s", name);
	snprintf(clients[slot].key, sizeof(clients[slot].key), "%s", key);
	save_trust_locked();
	pthread_mutex_unlock(&mutex);
}

/* ------------------------------------------------------------------------- */
/* obs-websocket: porta e senha, para a projeção controlar o OBS             */

static char *websocket_config_path(void)
{
	char *mine = obs_module_config_path("");
	if (!mine) {
		return NULL;
	}
	struct dstr path = {0};
	dstr_copy(&path, mine);
	bfree(mine);
	while (path.len && (path.array[path.len - 1] == '/' || path.array[path.len - 1] == '\\')) {
		dstr_resize(&path, path.len - 1);
	}
	const char *slash = strrchr(path.array, '/');
	const char *backslash = strrchr(path.array, '\\');
	if (backslash && (!slash || backslash > slash)) {
		slash = backslash;
	}
	if (!slash) {
		dstr_free(&path);
		return NULL;
	}
	dstr_resize(&path, (size_t)(slash - path.array) + 1);
	dstr_cat(&path, "obs-websocket/config.json");
	return path.array;
}

/*
 * Lê porta e senha do obs-websocket. Se o servidor dele estiver desligado, liga na configuração
 * (com senha) e devolve false: só vale depois de reiniciar o OBS.
 */
static bool websocket_info(int *port, char *password, size_t size)
{
	char *path = websocket_config_path();
	if (!path) {
		return false;
	}
	obs_data_t *config = obs_data_create_from_json_file(path);
	if (!config) {
		config = obs_data_create();
	}
	bool enabled = obs_data_get_bool(config, "server_enabled");
	*port = obs_data_has_user_value(config, "server_port") ? (int)obs_data_get_int(config, "server_port") : 4455;
	bool auth = !obs_data_has_user_value(config, "auth_required") || obs_data_get_bool(config, "auth_required");
	snprintf(password, size, "%s", auth ? obs_data_get_string(config, "server_password") : "");

	if (!enabled) {
		obs_data_set_bool(config, "server_enabled", true);
		obs_data_set_bool(config, "auth_required", true);
		if (!password[0]) {
			char fresh[33];
			random_hex(fresh, 32);
			obs_data_set_string(config, "server_password", fresh);
		}
		obs_data_set_bool(config, "first_load", false);
		obs_data_save_json_safe(config, path, "tmp", "bak");
		blog(LOG_INFO, "[projeto-live] projeção: servidor do obs-websocket ligado; vale ao reiniciar o OBS");
	}
	obs_data_release(config);
	bfree(path);
	return enabled;
}

/* Senha cifrada para a projeção: XOR com SHA-256(chave + desafio + "ws" + número do bloco). */
static void encrypt_password(const char *key, const char *nonce, const char *password, char *out)
{
	size_t len = strlen(password);
	uint8_t cipher[256];
	if (len > sizeof(cipher)) {
		len = sizeof(cipher);
	}
	for (size_t block = 0; block * PL_SHA256_SIZE < len; block++) {
		uint8_t stream[PL_SHA256_SIZE];
		uint8_t counter = (uint8_t)block;
		struct pl_sha256 ctx;
		pl_sha256_init(&ctx);
		pl_sha256_update(&ctx, key, strlen(key));
		pl_sha256_update(&ctx, nonce, strlen(nonce));
		pl_sha256_update(&ctx, "ws", 2);
		pl_sha256_update(&ctx, &counter, 1);
		pl_sha256_final(&ctx, stream);
		for (size_t i = 0; i < PL_SHA256_SIZE && block * PL_SHA256_SIZE + i < len; i++) {
			size_t at = block * PL_SHA256_SIZE + i;
			cipher[at] = (uint8_t)password[at] ^ stream[i];
		}
	}
	pl_hex(cipher, len, out);
	out[len * 2] = 0;
}

/* ------------------------------------------------------------------------- */
/* Pergunta "Permitir?" na interface                                         */

struct ask {
	unsigned int ticket;
	char name[PL_PROJECTION_NAME_MAX];
};

static void ask_task(void *data)
{
	struct ask *ask = data;
	pl_projection_ui_ask(ask->ticket, ask->name);
	bfree(ask);
}

static void restart_task(void *data)
{
	(void)data;
	pl_projection_ui_restart_notice();
}

void pl_projection_answer(unsigned int ticket, bool allow)
{
	pthread_mutex_lock(&mutex);
	answer_ticket = ticket;
	answer_value = allow ? 1 : -1;
	pthread_mutex_unlock(&mutex);
}

static int take_answer(unsigned int ticket)
{
	int value = 0;
	pthread_mutex_lock(&mutex);
	if (answer_ticket == ticket && answer_value) {
		value = answer_value;
		answer_value = 0;
	}
	pthread_mutex_unlock(&mutex);
	return value;
}

/* ------------------------------------------------------------------------- */
/* Conexões                                                                  */

static void peer_reset(struct peer *peer)
{
	memset(peer, 0, sizeof(*peer));
	peer->sock = PL_INVALID_SOCKET;
}

static void peer_close(struct peer *peer)
{
	if (peer->sock != PL_INVALID_SOCKET) {
		pl_close_socket(peer->sock);
	}
	peer_reset(peer);
}

static bool send_line(struct peer *peer, const char *text)
{
	peer->last_send_ns = os_gettime_ns();
	return pl_send_line(peer->sock, text);
}

static bool approval_pending(const struct peer *peers)
{
	for (int i = 0; i < MAX_PEERS; i++) {
		if (peers[i].state == PEER_WAITING) {
			return true;
		}
	}
	return false;
}

/* Conectado: manda o nome do OBS e os dados do obs-websocket. */
static bool welcome(struct peer *peer, const char *key)
{
	char name[160];
	char computer[PL_PROJECTION_NAME_MAX];
	pl_mdns_computer_name(computer, sizeof(computer));
	snprintf(name, sizeof(name), "OK %s", computer);
	if (!send_line(peer, name)) {
		return false;
	}
	peer->state = PEER_READY;
	blog(LOG_INFO, "[projeto-live] projeção conectada: %s", peer->name);

	int port = 0;
	char password[256];
	if (!websocket_info(&port, password, sizeof(password))) {
		obs_queue_task(OBS_TASK_UI, restart_task, NULL, false);
		return send_line(peer, "WS off");
	}
	char cipher[520];
	encrypt_password(key, peer->nonce, password, cipher);
	char line[600];
	snprintf(line, sizeof(line), "WS %d %s", port, cipher[0] ? cipher : "-");
	return send_line(peer, line);
}

struct line_context {
	struct peer *peer;
	struct peer *peers;
};

static bool handle_line(void *param, const char *line)
{
	struct line_context *context = param;
	struct peer *peer = context->peer;
	if (strcmp(line, "PING") == 0) {
		return true;
	}
	if (peer->state != PEER_HELLO || strncmp(line, "HELLO ", 6) != 0) {
		/* Outras linhas ficam para as próximas etapas (letra, comandos). */
		return peer->state == PEER_READY;
	}

	/* HELLO <id> <prova> <nome> */
	char id[ID_MAX] = "";
	char proof[2 * PL_SHA256_SIZE + 1] = "";
	const char *rest = line + 6;
	const char *space = strchr(rest, ' ');
	if (!space || (size_t)(space - rest) >= sizeof(id)) {
		return false;
	}
	memcpy(id, rest, (size_t)(space - rest));
	id[space - rest] = 0;
	rest = space + 1;
	space = strchr(rest, ' ');
	size_t proof_len = space ? (size_t)(space - rest) : strlen(rest);
	if (proof_len >= sizeof(proof)) {
		return false;
	}
	memcpy(proof, rest, proof_len);
	proof[proof_len] = 0;
	copy_clean(peer->id, sizeof(peer->id), id);
	copy_clean(peer->name, sizeof(peer->name), space ? space + 1 : id);

	if (strcmp(proof, "-") != 0) {
		char key[KEY_CHARS + 1];
		if (!find_key(peer->id, key)) {
			send_line(peer, "UNKNOWN");
			return false;
		}
		uint8_t digest[PL_SHA256_SIZE];
		char expected[2 * PL_SHA256_SIZE + 1];
		pl_hmac_sha256(key, strlen(key), peer->nonce, strlen(peer->nonce), digest);
		pl_hex(digest, PL_SHA256_SIZE, expected);
		expected[2 * PL_SHA256_SIZE] = 0;
		if (!same_text(expected, proof)) {
			send_line(peer, "UNKNOWN");
			return false;
		}
		return welcome(peer, key);
	}

	/* Primeira vez: pergunta na tela (uma pergunta de cada vez). */
	if (approval_pending(context->peers)) {
		send_line(peer, "BUSY");
		return false;
	}
	peer->state = PEER_WAITING;
	peer->since_ns = os_gettime_ns();
	peer->ticket = ++next_ticket;
	struct ask *ask = bzalloc(sizeof(*ask));
	ask->ticket = peer->ticket;
	snprintf(ask->name, sizeof(ask->name), "%s", peer->name);
	obs_queue_task(OBS_TASK_UI, ask_task, ask, false);
	return true;
}

static bool read_peer(struct peer *peer, struct peer *peers)
{
	peer->last_recv_ns = os_gettime_ns();
	struct line_context context = {peer, peers};
	return pl_read_lines(peer->sock, &peer->lines, handle_line, &context);
}

/* Resposta do "Permitir?" chegou (ou o tempo acabou). */
static bool check_approval(struct peer *peer, uint64_t now)
{
	int value = take_answer(peer->ticket);
	if (!value && now - peer->since_ns > APPROVAL_TIMEOUT_NS) {
		value = -1;
	}
	if (!value) {
		/* Enquanto a pessoa pensa, a conexão não cai. */
		if (now - peer->last_send_ns > PING_INTERVAL_NS) {
			return send_line(peer, "PING");
		}
		return true;
	}
	if (value < 0) {
		blog(LOG_INFO, "[projeto-live] projeção recusada: %s", peer->name);
		send_line(peer, "DENIED");
		return false;
	}
	char key[KEY_CHARS + 1];
	random_hex(key, KEY_CHARS);
	add_client(peer->id, peer->name, key);
	char line[KEY_CHARS + 16];
	snprintf(line, sizeof(line), "PAIRED %s", key);
	return send_line(peer, line) && welcome(peer, key);
}

static void accept_peer(pl_socket_t listener, struct peer *peers)
{
	pl_socket_t sock = accept(listener, NULL, NULL);
	if (sock == PL_INVALID_SOCKET) {
		return;
	}
	struct peer *slot = NULL;
	for (int i = 0; i < MAX_PEERS; i++) {
		if (peers[i].state == PEER_EMPTY) {
			slot = &peers[i];
			break;
		}
	}
	if (!slot) {
		pl_close_socket(sock);
		return;
	}
	pl_no_sigpipe(sock);
	pl_no_delay(sock);
	peer_reset(slot);
	slot->sock = sock;
	slot->state = PEER_HELLO;
	slot->since_ns = slot->last_recv_ns = os_gettime_ns();
	make_nonce(slot->nonce);
	char hello[NONCE_CHARS + ID_MAX + 16];
	pthread_mutex_lock(&mutex);
	snprintf(hello, sizeof(hello), "PLP1 %s %s", slot->nonce, obs_id);
	pthread_mutex_unlock(&mutex);
	if (!send_line(slot, hello)) {
		peer_close(slot);
	}
}

static pl_socket_t open_listener(int *port)
{
	for (int candidate = FIRST_PORT; candidate <= LAST_PORT; candidate++) {
		pl_socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (sock == PL_INVALID_SOCKET) {
			return PL_INVALID_SOCKET;
		}
#ifndef _WIN32
		int one = 1;
		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
		struct sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons((unsigned short)candidate);
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0 && listen(sock, 4) == 0) {
			*port = candidate;
			return sock;
		}
		pl_close_socket(sock);
	}
	return PL_INVALID_SOCKET;
}

static void publish_status(const struct peer *peers)
{
	char names[512] = "";
	int count = 0;
	for (int i = 0; i < MAX_PEERS; i++) {
		if (peers[i].state != PEER_READY) {
			continue;
		}
		size_t used = strlen(names);
		snprintf(names + used, sizeof(names) - used, "%s%s", count ? ", " : "", peers[i].name);
		count++;
	}
	pthread_mutex_lock(&mutex);
	snprintf(connected_names, sizeof(connected_names), "%s", names);
	connected_count = count;
	pthread_mutex_unlock(&mutex);
}

static void *link_thread(void *data)
{
	(void)data;
	os_set_thread_name("projeto-live: projeção");

	struct peer peers[MAX_PEERS];
	for (int i = 0; i < MAX_PEERS; i++) {
		peer_reset(&peers[i]);
	}
	pl_socket_t listener = PL_INVALID_SOCKET;
	int port = 0;
	uint64_t next_bind_ns = 0;
	bool warned = false;

	while (!stop_thread) {
		uint64_t now = os_gettime_ns();
		if (listener == PL_INVALID_SOCKET && now >= next_bind_ns) {
			listener = open_listener(&port);
			if (listener == PL_INVALID_SOCKET) {
				if (!warned) {
					blog(LOG_WARNING, "[projeto-live] projeção: portas %d a %d ocupadas",
					     FIRST_PORT, LAST_PORT);
				}
				warned = true;
				next_bind_ns = now + BIND_RETRY_NS;
			} else {
				char id[ID_MAX];
				pthread_mutex_lock(&mutex);
				snprintf(id, sizeof(id), "%s", obs_id);
				pthread_mutex_unlock(&mutex);
				blog(LOG_INFO, "[projeto-live] projeção: esperando na porta %d", port);
				pl_mdns_announce(port, id);
			}
		}

		pthread_mutex_lock(&mutex);
		bool forget = forget_requested;
		forget_requested = false;
		pthread_mutex_unlock(&mutex);
		if (forget) {
			for (int i = 0; i < MAX_PEERS; i++) {
				peer_close(&peers[i]);
			}
		}

		fd_set readable;
		FD_ZERO(&readable);
		int max_fd = 0;
		bool any = false;
		if (listener != PL_INVALID_SOCKET) {
			FD_SET(listener, &readable);
			max_fd = (int)listener;
			any = true;
		}
		for (int i = 0; i < MAX_PEERS; i++) {
			if (peers[i].sock != PL_INVALID_SOCKET) {
				FD_SET(peers[i].sock, &readable);
				if ((int)peers[i].sock > max_fd) {
					max_fd = (int)peers[i].sock;
				}
				any = true;
			}
		}
		if (!any) {
			os_sleep_ms(WAIT_MS);
			continue;
		}
		struct timeval timeout = {0, WAIT_MS * 1000};
		int ready = select(max_fd + 1, &readable, NULL, NULL, &timeout);
		if (ready < 0) {
			os_sleep_ms(WAIT_MS);
			continue;
		}
		now = os_gettime_ns();

		if (ready > 0 && listener != PL_INVALID_SOCKET && FD_ISSET(listener, &readable)) {
			accept_peer(listener, peers);
		}
		for (int i = 0; i < MAX_PEERS; i++) {
			struct peer *peer = &peers[i];
			if (peer->sock == PL_INVALID_SOCKET) {
				continue;
			}
			bool ok = true;
			if (ready > 0 && FD_ISSET(peer->sock, &readable)) {
				ok = read_peer(peer, peers);
			}
			if (ok && peer->state == PEER_WAITING) {
				ok = check_approval(peer, now);
			}
			if (ok && peer->state == PEER_HELLO && now - peer->since_ns > HELLO_TIMEOUT_NS) {
				ok = false;
			}
			if (ok && peer->state == PEER_READY) {
				if (now - peer->last_recv_ns > SILENCE_TIMEOUT_NS) {
					ok = false;
				} else if (now - peer->last_send_ns > PING_INTERVAL_NS) {
					ok = send_line(peer, "PING");
				}
			}
			if (!ok) {
				if (peer->state == PEER_READY) {
					blog(LOG_INFO, "[projeto-live] projeção desconectada: %s", peer->name);
				}
				peer_close(peer);
			}
		}
		publish_status(peers);
	}

	for (int i = 0; i < MAX_PEERS; i++) {
		peer_close(&peers[i]);
	}
	if (listener != PL_INVALID_SOCKET) {
		pl_close_socket(listener);
	}
	pl_mdns_withdraw();
	return NULL;
}

/* ------------------------------------------------------------------------- */
/* Começo e fim                                                              */

void pl_projection_start(void)
{
	if (thread_created) {
		return;
	}
	pl_socket_startup();
	pthread_mutex_init(&mutex, NULL);
	load_trust();
	stop_thread = false;
	if (pthread_create(&thread, NULL, link_thread, NULL) == 0) {
		thread_created = true;
	} else {
		blog(LOG_WARNING, "[projeto-live] projeção: não deu para abrir a thread");
	}
}

void pl_projection_stop(void)
{
	if (!thread_created) {
		return;
	}
	stop_thread = true;
	pthread_join(thread, NULL);
	thread_created = false;
	pthread_mutex_destroy(&mutex);
	pl_socket_cleanup();
}

int pl_projection_status(char *names, size_t size)
{
	if (!thread_created) {
		if (size) {
			names[0] = 0;
		}
		return 0;
	}
	pthread_mutex_lock(&mutex);
	int count = connected_count;
	snprintf(names, size, "%s", connected_names);
	pthread_mutex_unlock(&mutex);
	return count;
}

void pl_projection_forget_all(void)
{
	if (!thread_created) {
		return;
	}
	pthread_mutex_lock(&mutex);
	client_count = 0;
	save_trust_locked();
	forget_requested = true;
	pthread_mutex_unlock(&mutex);
	blog(LOG_INFO, "[projeto-live] projeção: pareamentos apagados");
}
