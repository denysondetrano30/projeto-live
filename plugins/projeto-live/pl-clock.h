/*
 * Projeto Live: relógio em comum entre o OBS e um celular (sincronia das câmeras).
 *
 * O canal de controle manda "TIME <hora do PC em ns>" a cada segundo; o app responde
 * "TIME <hora do PC> <hora do iPhone em ns>". Com a ida e volta, dá para saber a diferença entre
 * os dois relógios (como o NTP): fica valendo a medida da resposta mais rápida das últimas 16.
 *
 * Os quadros do celular chegam com o PTS no relógio do iPhone (veja o app, PhoneClock.swift); com
 * a diferença, o receptor (phone-video.c) sabe a hora do PC em que cada quadro foi filmado.
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

struct pl_clock;

struct pl_clock *pl_clock_create(void);
void pl_clock_destroy(struct pl_clock *clock);

/* Esquece as medidas (o celular desconectou; o próximo pode ser outro aparelho). */
void pl_clock_reset(struct pl_clock *clock);

/* Monta a pergunta "TIME <agora>" para mandar ao celular. */
void pl_clock_request(char *line, size_t size);

/* Trata uma linha que chegou do celular. Devolve true se era uma resposta "TIME". */
bool pl_clock_handle_line(struct pl_clock *clock, const char *line);

/* Diferença (relógio do celular - relógio do PC), em ns. false se ainda não houver medida. */
bool pl_clock_offset(struct pl_clock *clock, int64_t *offset_ns);

#ifdef __cplusplus
}
#endif
