/*
 * somun live: o que a mesa usa da entrada "Celular sem app (navegador)".
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <stdbool.h>

#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Devolve false se a fonte não é um "Celular sem app". stalled_ms = há quanto tempo não chega quadro
 * do celular; -1 = nenhum celular mandou imagem ainda (tela de espera com o QR). */
bool pl_webcam_get_status(obs_source_t *source, int *stalled_ms);

#ifdef __cplusplus
}
#endif
