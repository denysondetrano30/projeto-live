/*
 * Projeto Live: anúncio do OBS na rede (veja mdns-announce.h).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "mdns-announce.h"

#include <stdio.h>
#include <string.h>

#include <obs-module.h>

#define SERVICE_TYPE "_projetolive._tcp"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/*
 * Tipos da API de DNS-SD do Windows (windns.h), escritos aqui para não depender da versão mínima
 * do Windows nos cabeçalhos: as funções são procuradas na dnsapi.dll na hora.
 */
typedef void *pl_dns_instance;
typedef void(WINAPI *pl_dns_register_complete)(DWORD status, void *context, pl_dns_instance instance);

struct pl_dns_register_request {
	ULONG version;
	ULONG interface_index;
	pl_dns_instance instance;
	pl_dns_register_complete callback;
	void *context;
	HANDLE credentials;
	BOOL unicast_enabled;
};

typedef pl_dns_instance(WINAPI *construct_func)(const wchar_t *service, const wchar_t *host, void *ip4, void *ip6,
						WORD port, WORD priority, WORD weight, DWORD count,
						const wchar_t **keys, const wchar_t **values);
typedef DWORD(WINAPI *register_func)(struct pl_dns_register_request *request, void *cancel);
typedef void(WINAPI *free_func)(pl_dns_instance instance);

#define PL_DNS_REQUEST_PENDING 9506L
#define PL_DNS_QUERY_REQUEST_VERSION1 1

static HMODULE dnsapi;
static construct_func construct_instance;
static register_func service_register;
static register_func service_deregister;
static free_func free_instance;
static struct pl_dns_register_request request;
static bool registered;

static void WINAPI register_done(DWORD status, void *context, pl_dns_instance instance)
{
	(void)context;
	if (status != ERROR_SUCCESS) {
		blog(LOG_WARNING, "[projeto-live] anúncio na rede: o Windows respondeu %lu", (unsigned long)status);
	}
	if (instance && free_instance) {
		free_instance(instance);
	}
}

static bool load_api(void)
{
	if (dnsapi) {
		return construct_instance && service_register && service_deregister && free_instance;
	}
	dnsapi = LoadLibraryW(L"dnsapi.dll");
	if (!dnsapi) {
		return false;
	}
	construct_instance = (construct_func)(void *)GetProcAddress(dnsapi, "DnsServiceConstructInstance");
	service_register = (register_func)(void *)GetProcAddress(dnsapi, "DnsServiceRegister");
	service_deregister = (register_func)(void *)GetProcAddress(dnsapi, "DnsServiceDeRegister");
	free_instance = (free_func)(void *)GetProcAddress(dnsapi, "DnsServiceFreeInstance");
	return construct_instance && service_register && service_deregister && free_instance;
}

void pl_mdns_computer_name(char *out, size_t size)
{
	wchar_t name[256];
	DWORD length = (DWORD)(sizeof(name) / sizeof(name[0]));
	if (!size) {
		return;
	}
	out[0] = 0;
	if (GetComputerNameExW(ComputerNameDnsHostname, name, &length) && length) {
		WideCharToMultiByte(CP_UTF8, 0, name, -1, out, (int)size, NULL, NULL);
	}
	if (!out[0]) {
		snprintf(out, size, "OBS");
	}
}

void pl_mdns_withdraw(void)
{
	if (!registered) {
		return;
	}
	/* O Windows termina de tirar o anúncio depois e ainda usa o pedido: não dá para liberar aqui. */
	service_deregister(&request, NULL);
	registered = false;
}

void pl_mdns_announce(int port, const char *obs_id)
{
	if (registered) {
		return;
	}
	if (!load_api()) {
		blog(LOG_INFO, "[projeto-live] anúncio na rede: precisa do Windows 10 (1809) ou mais novo");
		return;
	}

	char computer[128];
	pl_mdns_computer_name(computer, sizeof(computer));
	char service_utf8[256];
	char host_utf8[160];
	char id_utf8[80];
	snprintf(service_utf8, sizeof(service_utf8), "%s." SERVICE_TYPE ".local", computer);
	snprintf(host_utf8, sizeof(host_utf8), "%s.local", computer);
	snprintf(id_utf8, sizeof(id_utf8), "%s", obs_id);

	wchar_t service[256];
	wchar_t host[160];
	wchar_t id[80];
	MultiByteToWideChar(CP_UTF8, 0, service_utf8, -1, service, 256);
	MultiByteToWideChar(CP_UTF8, 0, host_utf8, -1, host, 160);
	MultiByteToWideChar(CP_UTF8, 0, id_utf8, -1, id, 80);

	const wchar_t *keys[2] = {L"id", L"v"};
	const wchar_t *values[2] = {id, L"1"};
	memset(&request, 0, sizeof(request));
	request.version = PL_DNS_QUERY_REQUEST_VERSION1;
	request.instance = construct_instance(service, host, NULL, NULL, (WORD)port, 0, 0, 2, keys, values);
	request.callback = register_done;
	if (!request.instance) {
		return;
	}
	DWORD status = service_register(&request, NULL);
	if (status == PL_DNS_REQUEST_PENDING || status == ERROR_SUCCESS) {
		registered = true;
		blog(LOG_INFO, "[projeto-live] anúncio na rede: '%s' na porta %d", computer, port);
	} else {
		blog(LOG_WARNING, "[projeto-live] anúncio na rede falhou (%lu)", (unsigned long)status);
		free_instance(request.instance);
		request.instance = NULL;
	}
}

#elif defined(__APPLE__)

#include <arpa/inet.h>
#include <dns_sd.h>
#include <unistd.h>

static DNSServiceRef service_ref;

void pl_mdns_computer_name(char *out, size_t size)
{
	if (!size) {
		return;
	}
	out[0] = 0;
	if (gethostname(out, size) == 0) {
		out[size - 1] = 0;
		char *dot = strstr(out, ".local");
		if (dot) {
			*dot = 0;
		}
	}
	if (!out[0]) {
		snprintf(out, size, "OBS");
	}
}

void pl_mdns_withdraw(void)
{
	if (service_ref) {
		DNSServiceRefDeallocate(service_ref);
		service_ref = NULL;
	}
}

void pl_mdns_announce(int port, const char *obs_id)
{
	pl_mdns_withdraw();

	char computer[128];
	pl_mdns_computer_name(computer, sizeof(computer));

	TXTRecordRef txt;
	char buffer[256];
	TXTRecordCreate(&txt, (uint16_t)sizeof(buffer), buffer);
	TXTRecordSetValue(&txt, "id", (uint8_t)strlen(obs_id), obs_id);
	TXTRecordSetValue(&txt, "v", 1, "1");

	DNSServiceErrorType error = DNSServiceRegister(&service_ref, 0, 0, computer, SERVICE_TYPE, NULL, NULL,
						       htons((uint16_t)port), TXTRecordGetLength(&txt),
						       TXTRecordGetBytesPtr(&txt), NULL, NULL);
	TXTRecordDeallocate(&txt);
	if (error != kDNSServiceErr_NoError) {
		service_ref = NULL;
		blog(LOG_WARNING, "[projeto-live] anúncio na rede falhou (%d)", (int)error);
		return;
	}
	blog(LOG_INFO, "[projeto-live] anúncio na rede: '%s' na porta %d", computer, port);
}

#else

#include <unistd.h>

void pl_mdns_computer_name(char *out, size_t size)
{
	if (!size) {
		return;
	}
	out[0] = 0;
	if (gethostname(out, size) == 0) {
		out[size - 1] = 0;
	}
	if (!out[0]) {
		snprintf(out, size, "OBS");
	}
}

void pl_mdns_announce(int port, const char *obs_id)
{
	(void)obs_id;
	blog(LOG_INFO, "[projeto-live] anúncio na rede ainda não existe no Linux (porta %d)", port);
}

void pl_mdns_withdraw(void) {}

#endif
