/*
 * somun live: a tela "Acompanhar" do monitor 2 (veja watch-window.cpp).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

class QWidget;

/* O operador quer o Acompanhar? (o "⋯" liga e desliga; começa ligado; fica em acompanhar.json) */
bool pl_watch_wanted(void);
void pl_watch_set_wanted(bool wanted);

/* Tem um segundo monitor? */
bool pl_watch_available(void);

/* Chamado pelo relógio da tela Operar: mostra o Acompanhar no outro monitor quando ele é querido, há
 * dois monitores e a tela Operar está em uso; esconde no resto. Devolve se ele está aparecendo. */
bool pl_watch_update(QWidget *operate);

/* Solta as telas (o OBS está fechando a parte gráfica). */
void pl_watch_release(void);
