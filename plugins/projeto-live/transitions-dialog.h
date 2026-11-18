/*
 * Projeto Live: janelinha das transições (engrenagem do meio da janela Projeto Live) e a
 * "Transição desta entrada" no botão direito das Entradas.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <obs.h>

class QMenu;
class QWidget;

/* Abre (ou traz para a frente) a janela das transições. */
void pl_transitions_dialog_show(QWidget *parent);

/* Acrescenta ao menu da Entrada o submenu "Transição desta entrada" (a transição e a duração que
 * valem quando esta entrada entra no ar). */
void pl_entry_transition_menu(QMenu *menu, obs_source_t *scene);
