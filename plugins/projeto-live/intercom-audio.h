/*
 * Projeto Live: fone e microfone do diretor para o intercom.
 *
 * Usa a biblioteca miniaudio (deps/miniaudio, domínio público ou MIT-0) direto no aparelho de
 * som escolhido, sem passar pela mistura do OBS. Por isso a voz do intercom nunca vai para a live.
 * Tudo em 16 kHz, mono, 16 bits; a miniaudio converte para o formato do aparelho.
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

typedef void (*pl_audio_device_cb)(void *param, const char *name, bool is_default);

/* Lista os aparelhos (capture = microfones; senão, fones e caixas de som). */
void pl_audio_enum_devices(bool capture, pl_audio_device_cb callback, void *param);

/* Abre microfone e fone pelo nome ("" = o padrão do sistema). Em caso de problema, explica em
 * `error` e devolve false; o que deu certo continua aberto. */
bool pl_audio_open(const char *capture_name, const char *playback_name, char *error, size_t error_size);
void pl_audio_close(void);

/* Pega `samples` amostras do microfone. Devolve false se ainda não tem o suficiente. */
bool pl_audio_capture_read(int16_t *out, size_t samples);

/* Põe `samples` amostras na fila do fone. */
void pl_audio_playback_write(const int16_t *samples_in, size_t samples);

/* Libera a biblioteca (no descarregamento do módulo). */
void pl_audio_shutdown(void);

#ifdef __cplusplus
}
#endif
