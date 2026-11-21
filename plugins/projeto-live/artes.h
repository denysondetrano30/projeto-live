/*
 * somun live: "Artes" / "No programa" — a camada depois do corte (veja artes.cpp).
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

/* Liga a camada (chamado no carregamento do módulo) e solta tudo no fim. */
void pl_artes_load(void);
void pl_artes_unload(void);

#ifdef __cplusplus
}

class QWidget;

/* A barra "Artes" da mesa: "+" e um botão por peça alfinetada (liga e desliga). */
QWidget *pl_artes_bar_create(QWidget *parent);

/* Abre a tela "No programa" (configurar as peças). */
void pl_artes_open_editor(QWidget *parent);
#endif
