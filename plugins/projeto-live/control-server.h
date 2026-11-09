/*
 * Projeto Live: canal de controle entre a fonte "Celular" e o app, pelo Wi-Fi.
 *
 * O vídeo vai por SRT (UDP); o canal de controle usa TCP no mesmo número de porta da fonte.
 * Conversa em linhas de texto:
 *
 *   OBS     -> celular: "PL1 <desafio>"
 *   celular -> OBS:     "HELLO <HMAC-SHA256(senha do QR code, desafio) em hex> <nome da câmera>"
 *   OBS     -> celular: "OK", depois "TALLY on" (no ar) ou "TALLY off" sempre que mudar
 *   OBS     -> celular: "CMD <número> <texto>" (aviso do diretor, como "CMD 3 Abre")
 *   celular -> OBS:     "ACK <número>" quando o operador toca em "Entendi"
 *   OBS     -> celular: "AUDIO <porta UDP> <ficha>" e "VOICE <quem fala>" (intercom, veja intercom.h)
 *   OBS     -> celular: "TIME <hora do PC>"; celular -> OBS: "TIME <hora do PC> <hora do celular>"
 *                       (sincronia das câmeras, a cada segundo; veja pl-clock.h)
 *   os dois lados:      "PING" a cada 2 segundos; 6 segundos sem nada = conexão caiu
 *
 * Assim a senha nunca passa pela rede. Uma câmera por fonte: um celular novo que prove a senha
 * substitui o anterior. Pelo cabo USB, as mesmas linhas vão por um túnel do cabo
 * (veja usb-relay.c), sem desafio.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <stdbool.h>

#include "pl-link.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pl_control;
struct pl_voice_peer;
struct pl_clock;

/* Abre o canal na porta TCP `port`. `on_air`, `voice` (o participante desta fonte no intercom) e
 * `clock` (sincronia, pode ser NULL) são usados pela thread do canal e precisam durar mais que ele. Se a porta estiver ocupada,
 * continua tentando a cada 3 segundos. */
struct pl_control *pl_control_create(const char *source_name, int port, const char *key, const volatile bool *on_air,
				     struct pl_voice_peer *voice, struct pl_clock *clock);

/* true se o canal já está aberto com esta porta e esta senha. */
bool pl_control_matches(const struct pl_control *control, int port, const char *key);

/* Manda uma linha ao celular (sem o "\n"). Devolve false se não houver celular conectado.
 * Pode ser chamada de qualquer thread. */
bool pl_control_send(struct pl_control *control, const char *line);

/* O que o painel do diretor mostra: celular conectado, nome e último "Entendi". */
void pl_control_get_status(struct pl_control *control, struct pl_link_status *status);

void pl_control_destroy(struct pl_control *control);

#ifdef __cplusplus
}
#endif
