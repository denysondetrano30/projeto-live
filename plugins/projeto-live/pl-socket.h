/*
 * Projeto Live: ajudantes de socket (TCP) usados pelo relé do cabo USB e pelo canal de controle.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET pl_socket_t;
#define PL_INVALID_SOCKET INVALID_SOCKET
#define pl_close_socket closesocket
#else
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int pl_socket_t;
#define PL_INVALID_SOCKET (-1)
#define pl_close_socket close
#endif

#include <stdbool.h>
#include <stddef.h>

/* Não derrubar o OBS (SIGPIPE) quando o outro lado fecha a conexão no meio do envio. */
#ifdef MSG_NOSIGNAL
#define PL_SEND_FLAGS MSG_NOSIGNAL
#else
#define PL_SEND_FLAGS 0
#endif

static inline void pl_socket_startup(void)
{
#ifdef _WIN32
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

static inline void pl_socket_cleanup(void)
{
#ifdef _WIN32
	WSACleanup();
#endif
}

static inline bool pl_send_all(pl_socket_t sock, const char *data, size_t size)
{
	while (size) {
		int chunk = size > 0x7FFFFFFF ? 0x7FFFFFFF : (int)size;
		int sent = (int)send(sock, data, chunk, PL_SEND_FLAGS);
		if (sent <= 0) {
			return false;
		}
		data += sent;
		size -= (size_t)sent;
	}
	return true;
}

static inline bool pl_recv_all(pl_socket_t sock, char *data, size_t size)
{
	while (size) {
		int chunk = size > 0x7FFFFFFF ? 0x7FFFFFFF : (int)size;
		int got = (int)recv(sock, data, chunk, 0);
		if (got <= 0) {
			return false;
		}
		data += got;
		size -= (size_t)got;
	}
	return true;
}

/* Espera até `ms` milissegundos para o socket ter dados. 1 = pronto, 0 = tempo esgotado, -1 = erro. */
static inline int pl_wait_readable(pl_socket_t sock, int ms)
{
	fd_set readable;
	FD_ZERO(&readable);
	FD_SET(sock, &readable);
	struct timeval timeout;
	timeout.tv_sec = ms / 1000;
	timeout.tv_usec = (ms % 1000) * 1000;
	int ret = select((int)sock + 1, &readable, NULL, NULL, &timeout);
	return ret < 0 ? -1 : (ret > 0 ? 1 : 0);
}

static inline void pl_no_sigpipe(pl_socket_t sock)
{
#ifdef SO_NOSIGPIPE
	int one = 1;
	setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
	(void)sock;
#endif
}

/* Mensagens curtas (como "TALLY on") saem na hora, sem esperar juntar mais dados. */
static inline void pl_no_delay(pl_socket_t sock)
{
	int one = 1;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
}
