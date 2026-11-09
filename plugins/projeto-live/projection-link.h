/*
 * Projeto Live: ligação com o app de projeção (base FreeShow).
 *
 * O OBS se anuncia na rede (mDNS, "_projetolive._tcp") e espera o app de projeção na porta TCP
 * 9020 (se estiver ocupada, a próxima livre até 9029; a porta certa vai no anúncio). Conversa em
 * linhas de texto, como o canal dos celulares:
 *
 *   OBS     -> projeção: "PLP1 <desafio> <número deste OBS>"
 *   projeção -> OBS:     "HELLO <número da projeção> <prova> <nome do computador da projeção>"
 *                        prova = HMAC-SHA256(chave, desafio) em hex, ou "-" na primeira vez
 *   primeira vez:        o OBS pergunta na tela "Permitir?". Sim -> "PAIRED <chave>" (guardada nos
 *                        dois lados; daí em diante conecta sozinho). Não -> "DENIED".
 *   prova errada:        "UNKNOWN" (a projeção esquece a chave e pode pedir de novo)
 *   conectado:           "OK <nome deste OBS>", depois "WS <porta> <senha cifrada>" com os dados do
 *                        obs-websocket (é por ele que a projeção troca cena e liga a live), ou
 *                        "WS off" se o servidor do obs-websocket estiver desligado.
 *   OBS -> projeção:     "MIRROR <porta>" / "MIRROR off": mande (ou pare de mandar) o telão em FLV
 *                        para tcp://<este OBS>:<porta> (fonte "Telão (projeção)")
 *   projeção -> OBS:     "TEXT <texto em base64>" (texto do slide no telão, para a fonte "Legenda
 *                        (projeção)"; "TEXT -" = telão sem texto)
 *   os dois lados:       "PING" a cada 2 segundos; 6 segundos sem nada = conexão caiu
 *
 * A senha do obs-websocket vai cifrada com SHA-256(chave + desafio + "ws" + bloco), então nunca
 * passa aberta pela rede depois do pareamento.
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

#define PL_PROJECTION_NAME_MAX 96

void pl_projection_start(void);
void pl_projection_stop(void);

/* Quantas projeções estão conectadas agora; `names` recebe os nomes separados por ", ". */
int pl_projection_status(char *names, size_t size);

/* Texto do telão agora (para a legenda). Devolve um número que muda a cada texto novo. */
uint64_t pl_projection_caption(char *text, size_t size);

/* Porta onde a fonte "Telão (projeção)" escuta o telão (0 = nenhuma fonte, não mandar). */
void pl_projection_set_mirror(int port);

/* Resposta da pergunta "Permitir?" (chamada pela interface). */
void pl_projection_answer(unsigned int ticket, bool allow);

/* Esquece todas as projeções pareadas e derruba as conexões. */
void pl_projection_forget_all(void);

/* Implementadas em projection-ui.cpp (rodam na thread da interface). */
void pl_projection_ui_load(void);
void pl_projection_ui_ask(unsigned int ticket, const char *client_name);
void pl_projection_ui_restart_notice(void);

#ifdef __cplusplus
}
#endif
