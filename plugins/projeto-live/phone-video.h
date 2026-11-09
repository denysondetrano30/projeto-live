/*
 * Projeto Live: receptor da imagem do celular, com atraso fixo (sincronia das câmeras).
 *
 * Lê o vídeo MPEG-TS do celular (SRT pelo Wi-Fi, ou TCP do relé do cabo USB) com o FFmpeg,
 * decodifica o H.264 e entrega cada quadro numa fonte interna do OBS (assíncrona, sem buffer),
 * na hora certa:
 *
 *   hora em que o quadro foi filmado (no relógio do PC) + atraso fixo das câmeras
 *
 * O PTS de cada quadro vem no relógio do iPhone (app 0.9 em diante, PhoneClock.swift) e a
 * diferença entre os relógios vem do canal de controle (pl-clock.h). Assim todas as câmeras
 * aparecem com o mesmo atraso, e o som da mesa pode ser atrasado na mesma medida (pl-sync.h).
 *
 * Sem relógio (plano B, app antigo, ou antes da primeira medida), os quadros saem no ritmo do
 * PTS, com um atraso pequeno a partir do primeiro que chegou.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pl_clock;
struct pl_phone_video;

/* Registra a fonte interna (chamar uma vez, no carregamento do plugin). */
void pl_phone_video_register(void);

/* `clock` precisa durar mais que o receptor. */
struct pl_phone_video *pl_phone_video_create(const char *name, struct pl_clock *clock);
void pl_phone_video_destroy(struct pl_phone_video *video);

/* Fonte interna com a imagem (para desenhar e para contar como filha ativa). */
obs_source_t *pl_phone_video_source(struct pl_phone_video *video);

/* Troca o endereço (srt://... ou tcp://...). Vazio = parar. */
void pl_phone_video_set_url(struct pl_phone_video *video, const char *url);

/* Atraso medido desta câmera (do quadro filmado até sair do decodificador), em ms; -1 = sem medida. */
int pl_phone_video_latency_ms(struct pl_phone_video *video);

/* Atraso medido da câmera mais lenta agora, em ms; -1 = nenhuma medida. */
int pl_phone_video_max_latency_ms(void);

/* Quantas câmeras estão agora mostrando a imagem com o relógio em comum (atraso fixo). */
int pl_phone_video_synced_count(void);

#ifdef __cplusplus
}
#endif
