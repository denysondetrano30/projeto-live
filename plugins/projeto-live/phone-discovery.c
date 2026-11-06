/*
 * Projeto Live: descoberta dos celulares na rede local (mDNS).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdns.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include "phone-discovery.h"

#define PL_SERVICE_TYPE "_projetolive._tcp.local."
#define PL_SERVICE_MARK "._projetolive._tcp"
#define PL_MAX_SOCKETS 16
#define PL_LISTEN_MS 1000
#define PL_INTERVAL_MS 2000
#define PL_EXPIRE_NS (20ULL * 1000000000ULL)
#define PL_BUFFER_SIZE 4096

struct discovery_state {
	pthread_t thread;
	bool thread_created;
	os_event_t *stop_event;
	pthread_mutex_t mutex;
	bool mutex_created;
	struct pl_phone phones[PL_MAX_PHONES];
	size_t count;
#ifdef _WIN32
	bool wsa_started;
#endif
};

static struct discovery_state disc;

/* Informações tiradas de uma única resposta (um pacote vem de um único celular). */
struct reply_info {
	char name[PL_NAME_MAX];
	char address[PL_ADDR_MAX];
	int srt_port;
};

/* ------------------------------------------------------------------------- */
/* Ajudantes de texto                                                        */

static bool same_text_nocase(const char *a, const char *b, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		char ca = a[i];
		char cb = b[i];
		if (ca >= 'A' && ca <= 'Z')
			ca = (char)(ca - 'A' + 'a');
		if (cb >= 'A' && cb <= 'Z')
			cb = (char)(cb - 'A' + 'a');
		if (ca != cb)
			return false;
	}
	return true;
}

/* Posição de `mark` dentro de str[0..len), ou -1 se não estiver lá. */
static long find_nocase(const char *str, size_t len, const char *mark)
{
	size_t mark_len = strlen(mark);
	if (mark_len == 0 || len < mark_len)
		return -1;
	for (size_t i = 0; i + mark_len <= len; i++) {
		if (same_text_nocase(str + i, mark, mark_len))
			return (long)i;
	}
	return -1;
}

static void copy_text(char *dst, size_t dst_size, const char *src, size_t len)
{
	if (!dst_size)
		return;
	if (len >= dst_size)
		len = dst_size - 1;
	memcpy(dst, src, len);
	dst[len] = 0;
}

static void format_ipv4(const struct sockaddr *addr, char *out, size_t size)
{
	const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
	const unsigned char *b = (const unsigned char *)&in->sin_addr;
	snprintf(out, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

/* ------------------------------------------------------------------------- */
/* Leitura das respostas mDNS                                                */

static int record_callback(int sock, const struct sockaddr *from, size_t addrlen, mdns_entry_type_t entry,
			   uint16_t query_id, uint16_t rtype, uint16_t rclass, uint32_t ttl, const void *data,
			   size_t size, size_t name_offset, size_t name_length, size_t record_offset,
			   size_t record_length, void *user_data)
{
	struct reply_info *info = user_data;

	UNUSED_PARAMETER(sock);
	UNUSED_PARAMETER(addrlen);
	UNUSED_PARAMETER(entry);
	UNUSED_PARAMETER(query_id);
	UNUSED_PARAMETER(rclass);
	UNUSED_PARAMETER(ttl);
	UNUSED_PARAMETER(name_length);

	if (from && from->sa_family == AF_INET && !info->address[0])
		format_ipv4(from, info->address, sizeof(info->address));

	if (rtype == MDNS_RECORDTYPE_PTR) {
		char owner_buf[256];
		char target_buf[256];
		size_t offset = name_offset;
		mdns_string_t owner = mdns_string_extract(data, size, &offset, owner_buf, sizeof(owner_buf));
		if (find_nocase(owner.str, owner.length, PL_SERVICE_MARK) < 0)
			return 0;

		mdns_string_t target =
			mdns_record_parse_ptr(data, size, record_offset, record_length, target_buf, sizeof(target_buf));
		long mark = find_nocase(target.str, target.length, PL_SERVICE_MARK);
		if (mark > 0)
			copy_text(info->name, sizeof(info->name), target.str, (size_t)mark);

	} else if (rtype == MDNS_RECORDTYPE_TXT) {
		mdns_record_txt_t txt[16];
		size_t count = mdns_record_parse_txt(data, size, record_offset, record_length, txt, 16);
		for (size_t i = 0; i < count; i++) {
			if (txt[i].key.length == 3 && same_text_nocase(txt[i].key.str, "srt", 3)) {
				char value[16];
				copy_text(value, sizeof(value), txt[i].value.str, txt[i].value.length);
				int port = atoi(value);
				if (port > 0 && port < 65536)
					info->srt_port = port;
			}
		}
	}
	return 0;
}

static void remember_phone(const struct reply_info *info)
{
	if (!info->name[0] || !info->address[0])
		return;

	pthread_mutex_lock(&disc.mutex);

	size_t i;
	for (i = 0; i < disc.count; i++) {
		if (strcmp(disc.phones[i].name, info->name) == 0)
			break;
	}

	bool is_new = i == disc.count;
	if (is_new) {
		if (disc.count == PL_MAX_PHONES) {
			pthread_mutex_unlock(&disc.mutex);
			return;
		}
		disc.count++;
	}

	struct pl_phone *phone = &disc.phones[i];
	bool moved = !is_new && strcmp(phone->address, info->address) != 0;
	snprintf(phone->name, sizeof(phone->name), "%s", info->name);
	snprintf(phone->address, sizeof(phone->address), "%s", info->address);
	phone->srt_port = info->srt_port;
	phone->last_seen_ns = os_gettime_ns();

	pthread_mutex_unlock(&disc.mutex);

	if (is_new || moved)
		blog(LOG_INFO, "[projeto-live] Celular na rede: '%s' em %s:%d", info->name, info->address,
		     info->srt_port);
}

static void forget_old_phones(void)
{
	uint64_t now = os_gettime_ns();

	pthread_mutex_lock(&disc.mutex);
	size_t kept = 0;
	for (size_t i = 0; i < disc.count; i++) {
		if (now - disc.phones[i].last_seen_ns < PL_EXPIRE_NS) {
			if (kept != i)
				disc.phones[kept] = disc.phones[i];
			kept++;
		} else {
			blog(LOG_INFO, "[projeto-live] Celular sumiu da rede: '%s'", disc.phones[i].name);
		}
	}
	disc.count = kept;
	pthread_mutex_unlock(&disc.mutex);
}

/* ------------------------------------------------------------------------- */
/* Um socket por placa de rede, para achar o celular mesmo com várias redes  */

static int open_socket_for(const struct sockaddr_in *local)
{
	struct sockaddr_in saddr = *local;
	saddr.sin_port = 0;
	return mdns_socket_open_ipv4(&saddr);
}

#ifdef _WIN32
static size_t open_sockets(int *socks, size_t max)
{
	ULONG size = 16 * 1024;
	ULONG flags = GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER;
	IP_ADAPTER_ADDRESSES *adapters = bmalloc(size);
	ULONG ret = GetAdaptersAddresses(AF_INET, flags, NULL, adapters, &size);
	if (ret == ERROR_BUFFER_OVERFLOW) {
		bfree(adapters);
		adapters = bmalloc(size);
		ret = GetAdaptersAddresses(AF_INET, flags, NULL, adapters, &size);
	}
	if (ret != NO_ERROR) {
		bfree(adapters);
		return 0;
	}

	size_t count = 0;
	for (IP_ADAPTER_ADDRESSES *a = adapters; a && count < max; a = a->Next) {
		if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
		    (a->Flags & IP_ADAPTER_NO_MULTICAST))
			continue;
		for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && count < max; u = u->Next) {
			if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET)
				continue;
			int sock = open_socket_for((const struct sockaddr_in *)u->Address.lpSockaddr);
			if (sock >= 0)
				socks[count++] = sock;
		}
	}
	bfree(adapters);
	return count;
}
#else
static size_t open_sockets(int *socks, size_t max)
{
	struct ifaddrs *list = NULL;
	if (getifaddrs(&list) != 0)
		return 0;

	size_t count = 0;
	for (struct ifaddrs *ifa = list; ifa && count < max; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
			continue;
		if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_MULTICAST))
			continue;
#ifndef PL_TEST_ALLOW_LOOPBACK
		if (ifa->ifa_flags & IFF_LOOPBACK)
			continue;
#endif
		int sock = open_socket_for((const struct sockaddr_in *)ifa->ifa_addr);
		if (sock >= 0)
			socks[count++] = sock;
	}
	freeifaddrs(list);
	return count;
}
#endif

static bool should_stop(void)
{
	return os_event_try(disc.stop_event) == 0;
}

/* Pergunta "quem é _projetolive._tcp?" em todas as redes e escuta as respostas por PL_LISTEN_MS. */
static void query_once(void)
{
	int socks[PL_MAX_SOCKETS];
	size_t sock_count = open_sockets(socks, PL_MAX_SOCKETS);
	if (!sock_count)
		return;

	void *buffer = bmalloc(PL_BUFFER_SIZE);

	for (size_t i = 0; i < sock_count; i++)
		mdns_query_send(socks[i], MDNS_RECORDTYPE_PTR, MDNS_STRING_CONST(PL_SERVICE_TYPE), buffer,
				PL_BUFFER_SIZE, 0);

	uint64_t end = os_gettime_ns() + (uint64_t)PL_LISTEN_MS * 1000000ULL;
	while (os_gettime_ns() < end && !should_stop()) {
		fd_set readable;
		FD_ZERO(&readable);
		int max_fd = 0;
		for (size_t i = 0; i < sock_count; i++) {
#ifdef _WIN32
			FD_SET((SOCKET)socks[i], &readable);
#else
			FD_SET(socks[i], &readable);
#endif
			if (socks[i] >= max_fd)
				max_fd = socks[i] + 1;
		}

		struct timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = 100000;
		if (select(max_fd, &readable, NULL, NULL, &timeout) <= 0)
			continue;

		for (size_t i = 0; i < sock_count; i++) {
#ifdef _WIN32
			if (!FD_ISSET((SOCKET)socks[i], &readable))
				continue;
#else
			if (!FD_ISSET(socks[i], &readable))
				continue;
#endif
			struct reply_info info;
			memset(&info, 0, sizeof(info));
			info.srt_port = PL_DEFAULT_SRT_PORT;
			mdns_query_recv(socks[i], buffer, PL_BUFFER_SIZE, record_callback, &info, 0);
			remember_phone(&info);
		}
	}

	for (size_t i = 0; i < sock_count; i++)
		mdns_socket_close(socks[i]);
	bfree(buffer);
}

static void *discovery_thread(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("projeto-live: descoberta de celulares");

	do {
		query_once();
		forget_old_phones();
	} while (os_event_timedwait(disc.stop_event, PL_INTERVAL_MS) == ETIMEDOUT);

	return NULL;
}

/* ------------------------------------------------------------------------- */
/* Interface pública                                                         */

void pl_discovery_start(void)
{
	if (disc.thread_created)
		return;

#ifdef _WIN32
	WSADATA wsa;
	disc.wsa_started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#endif

	if (pthread_mutex_init(&disc.mutex, NULL) != 0) {
		blog(LOG_WARNING, "[projeto-live] Não consegui iniciar a descoberta de celulares (mutex)");
		return;
	}
	disc.mutex_created = true;

	if (os_event_init(&disc.stop_event, OS_EVENT_TYPE_MANUAL) != 0) {
		blog(LOG_WARNING, "[projeto-live] Não consegui iniciar a descoberta de celulares (evento)");
		return;
	}

	if (pthread_create(&disc.thread, NULL, discovery_thread, NULL) == 0) {
		disc.thread_created = true;
		blog(LOG_INFO, "[projeto-live] Procurando celulares na rede");
	} else {
		blog(LOG_WARNING, "[projeto-live] Não consegui iniciar a descoberta de celulares (thread)");
	}
}

void pl_discovery_stop(void)
{
	if (disc.thread_created) {
		os_event_signal(disc.stop_event);
		pthread_join(disc.thread, NULL);
		disc.thread_created = false;
	}
	if (disc.stop_event) {
		os_event_destroy(disc.stop_event);
		disc.stop_event = NULL;
	}
	if (disc.mutex_created) {
		pthread_mutex_destroy(&disc.mutex);
		disc.mutex_created = false;
	}
	disc.count = 0;

#ifdef _WIN32
	if (disc.wsa_started) {
		WSACleanup();
		disc.wsa_started = false;
	}
#endif
}

size_t pl_discovery_get(struct pl_phone *out, size_t max)
{
	if (!disc.mutex_created)
		return 0;

	pthread_mutex_lock(&disc.mutex);
	size_t count = disc.count < max ? disc.count : max;
	for (size_t i = 0; i < count; i++)
		out[i] = disc.phones[i];
	pthread_mutex_unlock(&disc.mutex);
	return count;
}

bool pl_discovery_find(const char *name, struct pl_phone *out)
{
	if (!disc.mutex_created || !name || !*name)
		return false;

	bool found = false;
	pthread_mutex_lock(&disc.mutex);
	for (size_t i = 0; i < disc.count; i++) {
		if (strcmp(disc.phones[i].name, name) == 0) {
			*out = disc.phones[i];
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&disc.mutex);
	return found;
}
