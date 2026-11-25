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

/* A grade grande do monitor 2 ("Acompanhar"): as mesmas entradas, do tamanho que couber. */
QWidget *pl_camera_grid_create(QWidget *parent);

/* Encolhe (ou volta) as entradas de uma cópia do painel em tirinhas com nome e luzinha. */
void pl_camera_dock_set_compact(QWidget *dock, bool compact);

/* A janela do somun live diz quem abre a janelinha "Fontes desta entrada" (nulo = o painel do OBS). */
void pl_entries_set_sources_opener(void (*opener)());

/* Põe a entrada na prévia e abre as fontes dela (a janelinha, ou o painel Fontes do OBS). */
void pl_entries_show_sources(struct obs_source *scene);

/* "Vai limpa": esta entrada ignora o que está "No programa" (o telão já vem marcado). */
bool pl_entry_goes_clean(struct obs_source *scene);
#include <string>
#include <vector>
/* Deck pelo celular: os nomes das entradas na ordem da fileira, sem as escondidas. */
std::vector<std::string> pl_entries_visible();
#endif
