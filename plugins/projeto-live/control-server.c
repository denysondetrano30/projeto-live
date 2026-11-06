/*
 * Projeto Live: canal de controle pelo Wi-Fi (veja control-server.h).
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

#include "control-server.h"
#include "sha256.h"

#define NONCE_CHARS 32
#define KEY_MAX 64
#define WAIT_MS 100
#define SECOND_NS 1000000000ULL
#define HELLO_TIMEOUT_NS (5 * SECOND_NS)
#define SILENCE_TIMEOUT_NS (6 * SECOND_NS)
#define PING_INTERVAL_NS (2 * SECOND_NS)
#define BIND_RETRY_NS (3 * SECOND_NS)

struct peer {
	pl_socket_t sock;
	struct pl_line_buffer lines;
	uint64_t last_recv_ns;
	uint64_t last_send_ns;
	char nonce[NONCE_CHARS + 1];
	bool authed;
	int sent_tally; /* -1 = ainda não mandou, 0 = fora do ar, 1 = no ar */
	char camera[PL_CAMERA_NAME_MAX];
};

struct pl_control {
	char *name;
	int port;
	char key[KEY_MAX];
	const volatile bool *on_air;
	pthread_t thread;
	bool thread_created;
	volatile bool stop;
	uint64_t nonce_counter;

	/* Compartilhado com a thread da interface (painel do diretor). */
	pthread_mutex_t mutex;
	struct pl_link_status status;
	struct dstr outbox; /* linhas esperando para ir ao celular, cada uma com "\n" */
};

/* ------------------------------------------------------------------------- */
/* Conexões                                                                  */

static void peer_reset(struct peer *peer)
{
	memset(peer, 0, sizeof(*peer));
	peer->sock = PL_INVALID_SOCKET;
	peer->sent_tally = -1;
}

static void peer_close(struct peer *peer)
{
	if (peer->sock != PL_INVALID_SOCKET)
		pl_close_socket(peer->sock);
	peer_reset(peer);
}

static bool send_line(struct peer *peer, const char *text, uint64_t now)
{
	peer->last_send_ns = now;
	return pl_send_line(peer->sock, text);
}

/* Atualiza o que o painel do diretor enxerga sobre o celular conectado. */
static void publish_client(struct pl_control *control, const struct peer *client)
{
	pthread_mutex_lock(&control->mutex);
	bool connected = client && client->sock != PL_INVALID_SOCKET;
	control->status.connected = connected;
	snprintf(control->status.camera, sizeof(control->status.camera), "%s", connected ? client->camera : "");
	/* Comando que ficou na fila sem celular não vale para o próximo que conectar. */
	dstr_free(&control->outbox);
	pthread_mutex_unlock(&control->mutex);
}

static void drop_client(struct pl_control *control, struct peer *client, const char *why)
{
	if (client->sock == PL_INVALID_SOCKET)
		return;
	blog(LOG_INFO, "[projeto-live] '%s': %s %s", control->name, client->camera, why);
	peer_close(client);
	publish_client(control, NULL);
}

/* Desafio novo a cada conexão, para uma resposta gravada não servir de novo. */
static void make_nonce(struct pl_control *control, char *out)
{
	uint64_t time = os_gettime_ns();
	uint64_t counter = ++control->nonce_counter;
	uintptr_t where = (uintptr_t)&time;
	int random = rand();

	uint8_t digest[PL_SHA256_SIZE];
	struct pl_sha256 ctx;
	pl_sha256_init(&ctx);
	pl_sha256_update(&ctx, &time, sizeof(time));
	pl_sha256_update(&ctx, &counter, sizeof(counter));
	pl_sha256_update(&ctx, &where, sizeof(where));
	pl_sha256_update(&ctx, &random, sizeof(random));
	pl_sha256_update(&ctx, control->key, strlen(control->key));
	pl_sha256_final(&ctx, digest);
	pl_hex(digest, NONCE_CHARS / 2, out);
}

/* Compara sem parar no primeiro caractere diferente (não entrega pistas pelo tempo). */
static bool same_text(const char *a, const char *b, size_t size)
{
	unsigned char diff = 0;
	for (size_t i = 0; i < size; i++)
		diff |= (unsigned char)(a[i] ^ b[i]);
	return diff == 0;
}

struct line_context {
	struct pl_control *control;
	struct peer *peer;
};

static bool handle_line(void *param, const char *line)
{
	struct line_context *ctx = param;
	struct pl_control *control = ctx->control;
	struct peer *peer = ctx->peer;

	if (!peer->authed) {
		if (strncmp(line, "HELLO ", 6) != 0)
			return false;

		const char *proof = line + 6;
		const char *name = strchr(proof, ' ');
		size_t proof_len = name ? (size_t)(name - proof) : strlen(proof);

		uint8_t mac[PL_SHA256_SIZE];
		char expected[PL_SHA256_SIZE * 2 + 1];
		pl_hmac_sha256(control->key, strlen(control->key), peer->nonce, strlen(peer->nonce), mac);
		pl_hex(mac, sizeof(mac), expected);

		if (proof_len != PL_SHA256_SIZE * 2 || !same_text(expected, proof, proof_len)) {
			pl_send_line(peer->sock, "ERR key");
			blog(LOG_WARNING,
			     "[projeto-live] '%s': um celular tentou o canal de controle com a senha errada "
			     "(QR code antigo?)",
			     control->name);
			return false;
		}

		snprintf(peer->camera, sizeof(peer->camera), "%s", name && name[1] ? name + 1 : "celular");
		peer->authed = true;
		return true;
	}

	/* O operador tocou em "Entendi" no aviso do diretor. */
	if (strncmp(line, "ACK ", 4) == 0) {
		uint32_t id = (uint32_t)strtoul(line + 4, NULL, 10);
		pthread_mutex_lock(&control->mutex);
		if (id > control->status.last_ack)
			control->status.last_ack = id;
		pthread_mutex_unlock(&control->mutex);
	}

	/* "PING" e linhas de versões futuras do app: nada a fazer. */
	return true;
}

static bool read_peer(struct pl_control *control, struct peer *peer, uint64_t now)
{
	struct line_context ctx = {control, peer};
	peer->last_recv_ns = now;
	return pl_read_lines(peer->sock, &peer->lines, handle_line, &ctx);
}

static void accept_peer(struct pl_control *control, pl_socket_t listener, struct peer *pending, uint64_t now)
{
	pl_socket_t sock = accept(listener, NULL, NULL);
	if (sock == PL_INVALID_SOCKET)
		return;

	/* Só um celular esperando por vez: o mais novo vale. */
	peer_close(pending);
	pending->sock = sock;
	pending->last_recv_ns = now;
	pl_no_sigpipe(sock);
	pl_no_delay(sock);
	make_nonce(control, pending->nonce);

	char challenge[NONCE_CHARS + 8];
	snprintf(challenge, sizeof(challenge), "PL1 %s", pending->nonce);
	if (!send_line(pending, challenge, now))
		peer_close(pending);
}

static pl_socket_t open_listener(int port)
{
	pl_socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == PL_INVALID_SOCKET)
		return PL_INVALID_SOCKET;

#ifndef _WIN32
	/* Permite reabrir a porta logo depois de fechar (no Windows isso teria outro efeito). */
	int one = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(sock, 4) != 0) {
		pl_close_socket(sock);
		return PL_INVALID_SOCKET;
	}
	return sock;
}

/* Manda ao celular o que o painel deixou na fila. */
static bool flush_outbox(struct pl_control *control, struct peer *client, uint64_t now)
{
	struct dstr pending = {0};
	pthread_mutex_lock(&control->mutex);
	pending = control->outbox;
	memset(&control->outbox, 0, sizeof(control->outbox));
	pthread_mutex_unlock(&control->mutex);

	bool ok = true;
	if (pending.len) {
		client->last_send_ns = now;
		ok = pl_send_all(client->sock, pending.array, pending.len);
	}
	dstr_free(&pending);
	return ok;
}

/* ------------------------------------------------------------------------- */
/* Thread do canal                                                           */

static void watch(fd_set *set, pl_socket_t sock, int *max_fd, bool *any)
{
	if (sock == PL_INVALID_SOCKET)
		return;
	FD_SET(sock, set);
	if ((int)sock > *max_fd)
		*max_fd = (int)sock;
	*any = true;
}

static void *control_thread(void *data)
{
	struct pl_control *control = data;
	os_set_thread_name("projeto-live: controle");

	pl_socket_t listener = PL_INVALID_SOCKET;
	uint64_t next_bind_ns = 0;
	bool bind_failed = false;
	struct peer pending, client;
	peer_reset(&pending);
	peer_reset(&client);

	while (!control->stop) {
		uint64_t now = os_gettime_ns();
		if (listener == PL_INVALID_SOCKET && now >= next_bind_ns) {
			listener = open_listener(control->port);
			if (listener == PL_INVALID_SOCKET) {
				if (!bind_failed)
					blog(LOG_WARNING,
					     "[projeto-live] '%s': a porta %d (TCP) está ocupada; o aviso de "
					     "\"no ar\" pelo Wi-Fi fica esperando ela liberar",
					     control->name, control->port);
				bind_failed = true;
				next_bind_ns = now + BIND_RETRY_NS;
			} else if (bind_failed) {
				blog(LOG_INFO, "[projeto-live] '%s': porta %d liberada, canal de controle aberto",
				     control->name, control->port);
				bind_failed = false;
			}
		}

		fd_set readable;
		FD_ZERO(&readable);
		int max_fd = 0;
		bool any = false;
		watch(&readable, listener, &max_fd, &any);
		watch(&readable, pending.sock, &max_fd, &any);
		watch(&readable, client.sock, &max_fd, &any);
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

		if (ready > 0 && listener != PL_INVALID_SOCKET && FD_ISSET(listener, &readable))
			accept_peer(control, listener, &pending, now);

		if (ready > 0 && pending.sock != PL_INVALID_SOCKET && FD_ISSET(pending.sock, &readable)) {
			if (!read_peer(control, &pending, now))
				peer_close(&pending);
		}

		if (ready > 0 && client.sock != PL_INVALID_SOCKET && FD_ISSET(client.sock, &readable)) {
			if (!read_peer(control, &client, now))
				drop_client(control, &client, "desconectou do canal de controle");
		}

		/* Provou a senha: vira a câmera desta fonte (substitui a anterior, se houver). */
		if (pending.sock != PL_INVALID_SOCKET && pending.authed) {
			if (client.sock != PL_INVALID_SOCKET)
				drop_client(control, &client, "foi substituído por outro celular");
			client = pending;
			peer_reset(&pending);
			client.last_recv_ns = now;
			blog(LOG_INFO, "[projeto-live] '%s': %s conectou no canal de controle", control->name,
			     client.camera);
			if (send_line(&client, "OK", now))
				publish_client(control, &client);
			else
				peer_close(&client);
		}

		if (pending.sock != PL_INVALID_SOCKET && now - pending.last_recv_ns > HELLO_TIMEOUT_NS)
			peer_close(&pending);

		if (client.sock != PL_INVALID_SOCKET && now - client.last_recv_ns > SILENCE_TIMEOUT_NS)
			drop_client(control, &client, "parou de responder no canal de controle");

		if (client.sock != PL_INVALID_SOCKET) {
			int tally = control->on_air && os_atomic_load_bool(control->on_air) ? 1 : 0;
			bool ok = flush_outbox(control, &client, now);
			if (ok && tally != client.sent_tally) {
				ok = send_line(&client, tally ? "TALLY on" : "TALLY off", now);
				client.sent_tally = tally;
			} else if (ok && now - client.last_send_ns >= PING_INTERVAL_NS) {
				ok = send_line(&client, "PING", now);
			}
			if (!ok)
				drop_client(control, &client, "desconectou do canal de controle");
		}
	}

	peer_close(&pending);
	peer_close(&client);
	publish_client(control, NULL);
	if (listener != PL_INVALID_SOCKET)
		pl_close_socket(listener);
	return NULL;
}

/* ------------------------------------------------------------------------- */

struct pl_control *pl_control_create(const char *source_name, int port, const char *key, const volatile bool *on_air)
{
	if (port <= 0 || port > 65535 || !key || !*key || strlen(key) >= KEY_MAX)
		return NULL;

	pl_socket_startup();

	struct pl_control *control = bzalloc(sizeof(struct pl_control));
	control->name = bstrdup(source_name);
	control->port = port;
	snprintf(control->key, sizeof(control->key), "%s", key);
	control->on_air = on_air;
	pthread_mutex_init(&control->mutex, NULL);

	if (pthread_create(&control->thread, NULL, control_thread, control) != 0) {
		pthread_mutex_destroy(&control->mutex);
		bfree(control->name);
		bfree(control);
		pl_socket_cleanup();
		return NULL;
	}
	control->thread_created = true;
	return control;
}

bool pl_control_matches(const struct pl_control *control, int port, const char *key)
{
	return control && control->port == port && key && strcmp(control->key, key) == 0;
}

bool pl_control_send(struct pl_control *control, const char *line)
{
	if (!control || !line || !*line)
		return false;

	pthread_mutex_lock(&control->mutex);
	bool connected = control->status.connected;
	if (connected) {
		dstr_cat(&control->outbox, line);
		dstr_cat(&control->outbox, "\n");
	}
	pthread_mutex_unlock(&control->mutex);
	return connected;
}

void pl_control_get_status(struct pl_control *control, struct pl_link_status *status)
{
	memset(status, 0, sizeof(*status));
	if (!control)
		return;
	pthread_mutex_lock(&control->mutex);
	*status = control->status;
	pthread_mutex_unlock(&control->mutex);
}

void pl_control_destroy(struct pl_control *control)
{
	if (!control)
		return;
	control->stop = true;
	if (control->thread_created)
		pthread_join(control->thread, NULL);
	dstr_free(&control->outbox);
	pthread_mutex_destroy(&control->mutex);
	bfree(control->name);
	bfree(control);
	pl_socket_cleanup();
}
