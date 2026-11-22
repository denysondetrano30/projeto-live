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

/*
 * O que a pessoa configura em cada destino. `platform`: youtube, facebook, instagram, tiktok ou
 * custom. `vertical`: recebe a tela vertical (senão, a imagem horizontal do Programa).
 */
struct pl_dest_info {
	/* Identificador fixo (para saber, ao salvar, quais destinos mudaram). */
	char id[40];
	char name[PL_DEST_NAME_MAX];
	char platform[16];
	char server[PL_DEST_TEXT_MAX];
	char key[PL_DEST_TEXT_MAX];
	bool enabled;
	bool vertical;
	/* Dia (AAAA-MM-DD) em que a chave foi colada: "chave de hoje" ou "chave velha". */
	char key_date[16];
	/* Antigos (antes da parte G, 22/11): ficam no arquivo, mas não valem mais. A qualidade agora
	 * é uma só para todos (pl_multistream_quality_kbps). */
	bool own_quality;
	int bitrate_kbps;
	char encoder[64];
	int width;
	int height;
};

struct pl_dest_status {
	enum pl_dest_state state;
	char error[PL_DEST_ERROR_MAX];
	/* Enviados desde que ligou (para calcular a velocidade). */
	uint64_t total_bytes;
};

/* Lê destinos.json. */
void pl_multistream_load(void);
void pl_multistream_unload(void);

/* Funções para o painel (thread da interface). Toda mudança fica guardada em destinos.json. */
size_t pl_multistream_count(void);
bool pl_multistream_get(size_t index, struct pl_dest_info *info);
void pl_multistream_get_status(size_t index, struct pl_dest_status *status);
/*
 * Troca a lista inteira (a aba Multistream das Configurações salva assim). Destinos iguais aos de
 * antes continuam no ar sem piscar; os que mudaram reiniciam; os que saíram param.
 */
void pl_multistream_set_all(const struct pl_dest_info *infos, size_t count);

/*
 * A transmissão do somun live (o botão "Transmitir"). start liga todos os destinos marcados e
 * devolve quantos são (0 = nenhum marcado, nada acontece). stop para todos.
 */
int pl_multistream_start(void);
void pl_multistream_stop(void);
bool pl_multistream_active(void);
/* Segundos desde o "Transmitir" (0 parada). */
uint64_t pl_multistream_seconds(void);

/* A qualidade de todos os destinos (horizontal e vertical), em kbps. Muda na hora, mesmo no ar. */
int pl_multistream_quality_kbps(void);
void pl_multistream_set_quality_kbps(int kbps);

/* Endereço padrão da plataforma ("" se a pessoa precisa colar o da plataforma). */
const char *pl_multistream_default_server(const char *platform);
/* O melhor codificador H.264 do PC (placa de vídeo primeiro, x264 por último). */
const char *pl_best_h264_encoder(void);

/* Instagram e TikTok são verticais (vale como padrão de destinos antigos e novos). */
bool pl_multistream_platform_vertical(const char *platform);
/* Codificadores H.264 que a pessoa pode escolher (sem os antigos e internos). */
typedef void (*pl_encoder_cb)(void *param, const char *id, const char *name);
void pl_multistream_enum_encoders(pl_encoder_cb callback, void *param);

#ifdef __cplusplus
}
#endif
