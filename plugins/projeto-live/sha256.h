/*
 * Projeto Live: SHA-256 e HMAC-SHA256 (FIPS 180-4 e RFC 2104).
 *
 * Usado no canal de controle: o OBS manda um desafio e o celular prova que tem a senha do
 * QR code sem mandar a senha pela rede.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PL_SHA256_SIZE 32

struct pl_sha256 {
	uint32_t state[8];
	uint64_t length;
	uint8_t block[64];
	size_t used;
};

void pl_sha256_init(struct pl_sha256 *ctx);
void pl_sha256_update(struct pl_sha256 *ctx, const void *data, size_t size);
void pl_sha256_final(struct pl_sha256 *ctx, uint8_t out[PL_SHA256_SIZE]);

void pl_hmac_sha256(const void *key, size_t key_size, const void *message, size_t message_size,
		    uint8_t out[PL_SHA256_SIZE]);

/* Escreve `size` bytes como texto hexadecimal minúsculo (out precisa de 2 * size + 1). */
void pl_hex(const uint8_t *data, size_t size, char *out);

#ifdef __cplusplus
}
#endif
