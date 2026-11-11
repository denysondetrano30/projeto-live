/*
 * Projeto Live: arranjos prontos dos painéis ("Layout") e a lista de painéis.
 *
 * Veja layout.cpp.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#ifdef __cplusplus

class QWidget;

/* Abre o menu "Layout" (arranjos prontos) embaixo de `button`. */
void pl_layout_show_menu(QWidget *button);

/* Abre o menu "Painéis" (ligar e desligar cada painel) embaixo de `button`. */
void pl_layout_show_panels_menu(QWidget *button);

#endif
