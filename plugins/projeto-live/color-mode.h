/*
 * somun live: o modo "Cor" (veja color-mode.cpp).
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

/* Registra o filtro de cor das câmeras (precisa ser no carregamento do módulo). */
void pl_color_mode_load(void);

#ifdef __cplusplus
}

#include <functional>

class QWidget;

/* O painel do modo Cor: o Look no alto e as câmeras grandes com os controles. Com onExit, mostra
 * também o botão "sair" (na tela de um monitor só). */
QWidget *pl_color_panel_create(QWidget *parent, std::function<void()> onExit);
#endif
