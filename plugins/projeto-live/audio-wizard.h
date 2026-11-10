/*
 * Projeto Live: assistente de áudio (veja audio-wizard.cpp).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Põe o assistente em Ferramentas e abre sozinho na primeira vez (sem entrada Mic/Aux). */
void pl_audio_wizard_load(void);

#ifdef __cplusplus
}
#endif
