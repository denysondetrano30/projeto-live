/*
 * Projeto Live: painéis da tela vertical (veja vertical-dock.cpp).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Coloca os painéis "Programa vertical", "Cenas verticais" e "Fontes verticais" no menu "Painéis". */
void pl_vertical_dock_load(void);

/* O "Programa vertical" está ligado (Painéis > Programa vertical)? */
bool pl_vertical_program_wanted(void);

#ifdef __cplusplus
}

class QWidget;

/* Cópias para a janela do Projeto Live: a tela do vertical (sem "Soltar") e as Fontes verticais. */
QWidget *pl_vertical_program_create(QWidget *parent);
QWidget *pl_vertical_sources_create(QWidget *parent);
#endif
