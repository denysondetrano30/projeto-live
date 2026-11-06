/*
 * Projeto Live: endereços IPv4 deste computador, para o QR code de pareamento.
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

#define PL_ADDR_MAX 64
#define PL_MAX_ADDRESSES 4

/* Preenche `out` com até `max` endereços IPv4 deste computador, a rede principal primeiro
 * (no Windows, as placas que têm roteador vêm antes das placas virtuais).
 * Devolve quantos endereços foram encontrados. */
size_t pl_local_ipv4(char out[][PL_ADDR_MAX], size_t max);

#ifdef __cplusplus
}
#endif
