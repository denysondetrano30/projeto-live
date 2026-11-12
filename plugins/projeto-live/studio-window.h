/*
 * Projeto Live: a janela própria do Projeto Live (veja studio-window.cpp).
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

/* Liga a janela no carregamento do módulo (ela só abre depois que o OBS terminar de carregar). */
void pl_studio_window_load(void);
void pl_studio_window_unload(void);

/* Troca para o modo Projeto Live (esconde a janela do OBS e mostra a nossa). */
void pl_studio_window_enter_live(void);

#ifdef __cplusplus
}
#endif
