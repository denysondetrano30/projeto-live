/*
 * Projeto Live: SHA-256 e HMAC-SHA256 (veja sha256.h).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include <string.h>

#include "sha256.h"

static const uint32_t round_constants[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr(uint32_t value, int bits)
{
	return (value >> bits) | (value << (32 - bits));
}

static void compress(struct pl_sha256 *ctx, const uint8_t *block)
{
	uint32_t w[64];
	for (int i = 0; i < 16; i++)
		w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
		       ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
	uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

	for (int i = 0; i < 64; i++) {
		uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
		uint32_t choose = (e & f) ^ (~e & g);
		uint32_t t1 = h + s1 + choose + round_constants[i] + w[i];
		uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
		uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = s0 + majority;
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
	ctx->state[5] += f;
	ctx->state[6] += g;
	ctx->state[7] += h;
}

void pl_sha256_init(struct pl_sha256 *ctx)
{
	static const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
					    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	memcpy(ctx->state, initial, sizeof(initial));
	ctx->length = 0;
	ctx->used = 0;
}

void pl_sha256_update(struct pl_sha256 *ctx, const void *data, size_t size)
{
	const uint8_t *bytes = data;
	ctx->length += (uint64_t)size;
	while (size) {
		size_t take = 64 - ctx->used;
		if (take > size)
			take = size;
		memcpy(ctx->block + ctx->used, bytes, take);
		ctx->used += take;
		bytes += take;
		size -= take;
		if (ctx->used == 64) {
			compress(ctx, ctx->block);
			ctx->used = 0;
		}
	}
}

void pl_sha256_final(struct pl_sha256 *ctx, uint8_t out[PL_SHA256_SIZE])
{
	uint64_t bits = ctx->length * 8;

	ctx->block[ctx->used++] = 0x80;
	if (ctx->used > 56) {
		memset(ctx->block + ctx->used, 0, 64 - ctx->used);
		compress(ctx, ctx->block);
		ctx->used = 0;
	}
	memset(ctx->block + ctx->used, 0, 56 - ctx->used);
	for (int i = 0; i < 8; i++)
		ctx->block[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
	compress(ctx, ctx->block);

	for (int i = 0; i < 8; i++) {
		out[i * 4] = (uint8_t)(ctx->state[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
		out[i * 4 + 3] = (uint8_t)ctx->state[i];
	}
}

void pl_hmac_sha256(const void *key, size_t key_size, const void *message, size_t message_size,
		    uint8_t out[PL_SHA256_SIZE])
{
	uint8_t block_key[64] = {0};
	if (key_size > sizeof(block_key)) {
		struct pl_sha256 ctx;
		pl_sha256_init(&ctx);
		pl_sha256_update(&ctx, key, key_size);
		pl_sha256_final(&ctx, block_key);
	} else {
		memcpy(block_key, key, key_size);
	}

	uint8_t inner_pad[64], outer_pad[64];
	for (int i = 0; i < 64; i++) {
		inner_pad[i] = block_key[i] ^ 0x36;
		outer_pad[i] = block_key[i] ^ 0x5c;
	}

	uint8_t inner[PL_SHA256_SIZE];
	struct pl_sha256 ctx;
	pl_sha256_init(&ctx);
	pl_sha256_update(&ctx, inner_pad, sizeof(inner_pad));
	pl_sha256_update(&ctx, message, message_size);
	pl_sha256_final(&ctx, inner);

	pl_sha256_init(&ctx);
	pl_sha256_update(&ctx, outer_pad, sizeof(outer_pad));
	pl_sha256_update(&ctx, inner, sizeof(inner));
	pl_sha256_final(&ctx, out);
}

void pl_hex(const uint8_t *data, size_t size, char *out)
{
	static const char digits[] = "0123456789abcdef";
	for (size_t i = 0; i < size; i++) {
		out[i * 2] = digits[data[i] >> 4];
		out[i * 2 + 1] = digits[data[i] & 15];
	}
	out[size * 2] = 0;
}
