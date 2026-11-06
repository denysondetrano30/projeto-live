/*
 * Projeto Live: estado da ligação com o celular, visto pela fonte e pelo painel do diretor.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PL_CAMERA_NAME_MAX 128

struct pl_link_status {
	bool connected;                  /* há um celular pronto para receber comandos */
	char camera[PL_CAMERA_NAME_MAX]; /* nome que o app mostra, como "Câmera iPhone 4F2A" */
	uint32_t last_ack;               /* número do último comando que o operador confirmou */
};
