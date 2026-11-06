/*
 * Projeto Live: descoberta dos celulares na rede local.
 *
 * O app do celular se anuncia na rede como "_projetolive._tcp" (Bonjour/mDNS).
 * Uma thread em segundo plano pergunta pela rede a cada poucos segundos e guarda
 * a lista dos celulares encontrados, com o endereço IP e a porta do vídeo (SRT).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PL_DEFAULT_SRT_PORT 9000
#define PL_NAME_MAX 128
#define PL_ADDR_MAX 64
#define PL_MAX_PHONES 32

struct pl_phone {
	char name[PL_NAME_MAX];
	char address[PL_ADDR_MAX];
	int srt_port;
	uint64_t last_seen_ns;
};

void pl_discovery_start(void);
void pl_discovery_stop(void);

/* Copia até `max` celulares vistos recentemente para `out` e devolve quantos foram copiados. */
size_t pl_discovery_get(struct pl_phone *out, size_t max);

/* Procura um celular pelo nome. Devolve true e preenche `out` se ele estiver na lista. */
bool pl_discovery_find(const char *name, struct pl_phone *out);

#ifdef __cplusplus
}
#endif
