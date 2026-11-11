/*
 * Projeto Live: o que o painel do diretor usa da fonte "Celular".
 *
 * Todas as funções daqui são chamadas na thread da interface do OBS.
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

#include <obs.h>

#include "pl-link.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PL_PHONE_SOURCE_ID "projeto_live_phone_source"
#define PL_COMMAND_MAX 200

struct pl_phone_status {
	bool connected; /* há celular pronto para receber avisos */
	bool on_air;
	bool usb;
	char camera[PL_CAMERA_NAME_MAX];
	uint32_t last_sent;  /* número do último aviso enviado (0 = nenhum) */
	uint32_t last_acked; /* número do último aviso que o operador confirmou */
	char last_text[PL_COMMAND_MAX + 1];
	bool intercom; /* o celular está no intercom */
	bool talking;  /* o operador está falando no intercom agora */

	/* Para as miniaturas do painel "Câmeras": */
	int latency_ms;    /* atraso medido desta câmera; -1 = sem medida */
	int stalled_ms;    /* há quanto tempo não aparece quadro novo; -1 = nenhum quadro ainda */
	bool has_image;    /* está chegando imagem (nem travada nem parada) */
};

/* Acima disto a imagem no ar está congelada (a miniatura fica amarela). */
#define PL_PHONE_STALL_MS 2000

bool pl_phone_is_source(obs_source_t *source);

bool pl_phone_get_status(obs_source_t *source, struct pl_phone_status *status);

/* Manda um aviso ao celular desta fonte. Devolve o número do aviso, ou 0 se não deu. */
uint32_t pl_phone_send_command(obs_source_t *source, const char *text);

/* Intercom: o diretor fala só com esta câmera enquanto `on` (segurando o botão). Qualquer thread. */
void pl_phone_set_private_talk(obs_source_t *source, bool on);

/* Avisos prontos ("Abre", "Fecha", "Foca no pregador", "Você está no ar"), já traduzidos. */
size_t pl_phone_command_count(void);
const char *pl_phone_command_text(size_t index);

#ifdef __cplusplus
}
#endif
