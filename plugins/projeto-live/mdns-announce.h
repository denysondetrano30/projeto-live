/*
 * Projeto Live: anúncio do OBS na rede (mDNS / Bonjour, serviço "_projetolive._tcp"), para o app
 * de projeção achar o OBS sozinho.
 *
 * Windows 10 1809 ou mais novo: API DnsServiceRegister do próprio Windows (carregada na hora, para
 * o plugin abrir mesmo num Windows antigo). macOS: dns_sd do sistema. Linux: ainda não anuncia
 * (a projeção no mesmo computador acha o OBS mesmo assim).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Anuncia (ou troca o anúncio para) a porta `port`, com o número deste OBS no texto do anúncio. */
void pl_mdns_announce(int port, const char *obs_id);

/* Tira o anúncio da rede. */
void pl_mdns_withdraw(void);

/* Nome deste computador, como aparece na rede. */
void pl_mdns_computer_name(char *out, size_t size);

#ifdef __cplusplus
}
#endif
