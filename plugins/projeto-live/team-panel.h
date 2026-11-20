/*
 * somun live: a Equipe pequena da tela Operar (veja team-panel.cpp).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

class QWidget;

/* A lista da equipe: um nome por celular, bolinha de quem fala, clique = recado, segurar = falar. */
QWidget *pl_team_panel_create(QWidget *parent);
