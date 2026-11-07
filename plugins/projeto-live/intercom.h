/*
 * Projeto Live: intercom (conversa por voz entre o diretor e os operadores de câmera).
 *
 * O PC é a central: recebe a voz de cada celular e do fone do diretor, mistura e devolve a cada
 * um o que ele deve ouvir.
 *
 * - Canal geral: quem aperta "Segure para falar" no celular é ouvido pelo diretor e por todos
 *   os outros celulares. "Falar com todos" (segurar ou travar) leva a voz do diretor a todos.
 * - "Falar só com esta câmera" leva a voz do diretor só àquele celular.
 * - Ninguém ouve a própria voz.
 *
 * Voz em 16 kHz, mono, 16 bits, em quadros de 20 ms (320 amostras), sem compressão (cerca de
 * 256 kbit/s, folgado numa rede local). Formato do pacote:
 *
 *   "PLV1" | ficha (8 bytes) | sequência (2 bytes, little-endian) | marcas (1 byte) | 0 |
 *   voz: 640 bytes (PCM 16 bits little-endian), só quando a marca PL_VOICE_FLAG_AUDIO está ligada
 *
 * Pelo Wi-Fi, os pacotes vão por UDP (um pacote perdido vira só um estalo, sem atrasar o resto).
 * O canal de controle diz ao celular a porta e a ficha ("AUDIO <porta> <ficha em hex>"), e o
 * celular manda um pacote sem voz a cada segundo para o PC saber o endereço dele. Pelo cabo USB,
 * os mesmos pacotes vão por um túnel TCP (porta 9012 do app), cada um precedido do tamanho em
 * 2 bytes (little-endian). O canal de controle também avisa quem está falando:
 * "VOICE <nomes>" ou "VOICE -".
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

#define PL_VOICE_RATE 16000
#define PL_VOICE_SAMPLES 320
#define PL_VOICE_HEADER 16
#define PL_VOICE_PACKET_MAX (PL_VOICE_HEADER + PL_VOICE_SAMPLES * 2)
#define PL_VOICE_FLAG_AUDIO 1
#define PL_VOICE_TEXT_MAX 256

struct pl_voice_peer;

/* A central: liga no carregamento do módulo e desliga no descarregamento. */
void pl_intercom_start(void);
void pl_intercom_stop(void);

/* ---- Participantes: um por fonte "Celular" (qualquer thread) ---- */

struct pl_voice_peer *pl_voice_peer_create(const char *name);
void pl_voice_peer_destroy(struct pl_voice_peer *peer);
void pl_voice_peer_set_name(struct pl_voice_peer *peer, const char *name);

/* Wi-Fi: a linha "AUDIO <porta> <ficha>" para o canal de controle mandar. false se não houver. */
bool pl_voice_peer_audio_line(struct pl_voice_peer *peer, char *out, size_t size);

/* Cabo: veja intercom-stream.h (fica separado para este arquivo não precisar dos sockets). */

/* Quem este celular está ouvindo agora, para a linha "VOICE". O número devolvido muda sempre que
 * o texto muda: quem manda a linha guarda o último número enviado. */
uint32_t pl_voice_peer_voice_text(struct pl_voice_peer *peer, char *out, size_t size);

bool pl_voice_peer_is_connected(struct pl_voice_peer *peer);
bool pl_voice_peer_is_talking(struct pl_voice_peer *peer);

/* ---- Diretor ---- */

/* Segurando "Falar com todos" (botão ou tecla). */
void pl_intercom_set_talk_all(bool on);
/* Microfone travado aberto, falando com todos. */
void pl_intercom_set_latched(bool on);
bool pl_intercom_get_latched(void);
/* Segurando "Falar só com esta câmera". */
void pl_voice_peer_set_private(struct pl_voice_peer *peer, bool on);

struct pl_intercom_state {
	bool talking;                     /* a voz do diretor está indo para alguém */
	bool talk_all;                    /* ...para todos (segurando "Falar com todos" ou travado) */
	bool latched;                     /* microfone travado aberto */
	bool devices_open;                /* fone e microfone abertos */
	bool any_phone;                   /* há algum celular no intercom */
	char error[160];                  /* problema com o fone ou o microfone, se houver */
	char speaking[PL_VOICE_TEXT_MAX]; /* quais câmeras estão falando agora ("" = ninguém) */
};
void pl_intercom_get_state(struct pl_intercom_state *state);

/* Fone e microfone escolhidos ("" = padrão do sistema) e volume do fone (1.0 = normal). */
void pl_intercom_get_devices(char *capture, size_t capture_size, char *playback, size_t playback_size, float *volume);
void pl_intercom_set_devices(const char *capture, const char *playback, float volume);

#ifdef __cplusplus
}
#endif
