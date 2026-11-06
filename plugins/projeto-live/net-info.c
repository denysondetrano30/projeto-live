/*
 * Projeto Live: endereços IPv4 deste computador, para o QR code de pareamento.
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
#include <sys/socket.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <util/bmem.h>

#include "net-info.h"

/* Endereços que não servem para o celular chegar até o computador. */
static bool is_usable(const struct sockaddr_in *in)
{
	const unsigned char *b = (const unsigned char *)&in->sin_addr;
	if (b[0] == 0 || b[0] == 127)
		return false;
	if (b[0] == 169 && b[1] == 254)
		return false;
	return true;
}

static void add_address(char out[][PL_ADDR_MAX], size_t max, size_t *count, const struct sockaddr_in *in)
{
	if (*count >= max || !is_usable(in))
		return;

	char text[PL_ADDR_MAX];
	const unsigned char *b = (const unsigned char *)&in->sin_addr;
	snprintf(text, sizeof(text), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);

	for (size_t i = 0; i < *count; i++) {
		if (strcmp(out[i], text) == 0)
			return;
	}
	snprintf(out[*count], PL_ADDR_MAX, "%s", text);
	(*count)++;
}

#ifdef _WIN32
size_t pl_local_ipv4(char out[][PL_ADDR_MAX], size_t max)
{
	ULONG size = 16 * 1024;
	ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST |
		      GAA_FLAG_SKIP_DNS_SERVER;
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
	/* Primeira passada: placas com roteador (a rede de verdade). Segunda: as outras. */
	for (int pass = 0; pass < 2; pass++) {
		for (IP_ADAPTER_ADDRESSES *a = adapters; a; a = a->Next) {
			if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
				continue;
			bool has_gateway = a->FirstGatewayAddress != NULL;
			if ((pass == 0) != has_gateway)
				continue;
			for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
				if (u->Address.lpSockaddr && u->Address.lpSockaddr->sa_family == AF_INET)
					add_address(out, max, &count,
						    (const struct sockaddr_in *)u->Address.lpSockaddr);
			}
		}
	}

	bfree(adapters);
	return count;
}
#else
size_t pl_local_ipv4(char out[][PL_ADDR_MAX], size_t max)
{
	struct ifaddrs *list = NULL;
	if (getifaddrs(&list) != 0)
		return 0;

	size_t count = 0;
	for (struct ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
			continue;
		if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & (IFF_LOOPBACK | IFF_POINTOPOINT)))
			continue;
		add_address(out, max, &count, (const struct sockaddr_in *)ifa->ifa_addr);
	}

	freeifaddrs(list);
	return count;
}
#endif
