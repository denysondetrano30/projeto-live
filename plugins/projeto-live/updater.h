/*
 * somun live: atualização automática (veja updater.cpp).
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

/* Liga no carregamento do módulo e desliga no fim. */
void pl_updater_load(void);
void pl_updater_unload(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
/* Versão nova já baixada e pronta para instalar. */
bool pl_updater_ready();
/* Avisa que a transmissão ou a gravação está ligada (não baixa no ar). */
void pl_updater_set_busy(bool busy);
/* "Atualizar agora": fecha o somun live, instala e abre de novo. */
void pl_updater_install_now();
#endif
