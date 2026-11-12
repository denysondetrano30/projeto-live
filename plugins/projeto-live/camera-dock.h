/*
 * Projeto Live: painel "Câmeras" dentro do OBS (veja camera-dock.cpp).
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

/* Coloca o painel no menu "Painéis" do OBS. Chamado no carregamento do módulo. */
void pl_camera_dock_load(void);

#ifdef __cplusplus
}

class QWidget;
struct obs_source;

/* Mais uma cópia do painel Entradas (a janela do Projeto Live usa uma; a do OBS, outra). */
QWidget *pl_camera_dock_create(QWidget *parent);

/* Abre a janela "Adicionar fonte" do OBS para esta entrada (cena). */
void pl_entries_add_source(struct obs_source *scene);
#endif
