/*
 * Projeto Live: destinos extras da transmissão (multistream direto do PC; veja multistream.c).
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

#define PL_DEST_NAME_MAX 64
#define PL_DEST_TEXT_MAX 512
#define PL_DEST_ERROR_MAX 256

/*
 * OFF: desmarcado. WAITING: marcado, esperando "Iniciar transmissão". CONNECTING: ligando para a
 * plataforma. LIVE: no ar. RECONNECTING: caiu e está tentando de novo. ERROR: parou com erro.
 */
enum pl_dest_state {
	PL_DEST_OFF,
	PL_DEST_WAITING,
	PL_DEST_CONNECTING,
	PL_DEST_LIVE,
	PL_DEST_RECONNECTING,
	PL_DEST_ERROR,
};

/* O que a pessoa configura em cada destino. `platform`: youtube, facebook, instagram, tiktok ou custom. */
struct pl_dest_info {
	char name[PL_DEST_NAME_MAX];
	char platform[16];
	char server[PL_DEST_TEXT_MAX];
	char key[PL_DEST_TEXT_MAX];
	bool enabled;
	/* false: mesmo vídeo da transmissão principal (não pesa no PC). */
	bool own_quality;
	/* Só com own_quality. */
	int bitrate_kbps;
};

struct pl_dest_status {
	enum pl_dest_state state;
	char error[PL_DEST_ERROR_MAX];
	/* Enviados desde que ligou (para calcular a velocidade). */
	uint64_t total_bytes;
};

/* Lê destinos.json e passa a seguir o botão "Iniciar transmissão". */
void pl_multistream_load(void);
void pl_multistream_unload(void);

/* Funções para o painel (thread da interface). Toda mudança fica guardada em destinos.json. */
size_t pl_multistream_count(void);
bool pl_multistream_get(size_t index, struct pl_dest_info *info);
void pl_multistream_get_status(size_t index, struct pl_dest_status *status);
void pl_multistream_add(const struct pl_dest_info *info);
/* Se o destino estiver no ar, ele reinicia com as configurações novas. */
void pl_multistream_update(size_t index, const struct pl_dest_info *info);
void pl_multistream_remove(size_t index);
void pl_multistream_set_enabled(size_t index, bool enabled);

/* A transmissão principal (Configurações > Transmissão) está no ar? */
bool pl_multistream_main_live(void);
/* Velocidade de vídeo da principal, em kbps (0 se não souber). */
int pl_multistream_main_kbps(void);

/* Endereço padrão da plataforma ("" se a pessoa precisa colar o da plataforma). */
const char *pl_multistream_default_server(const char *platform);
/* O melhor codificador H.264 do PC (placa de vídeo primeiro, x264 por último). */
const char *pl_best_h264_encoder(void);

/* Instagram e TikTok são verticais. */
bool pl_multistream_platform_vertical(const char *platform);

#ifdef __cplusplus
}
#endif
