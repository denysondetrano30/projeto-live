/*
 * Projeto Live: painel "Som" dentro do OBS (veja audio-dock.cpp).
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
void pl_audio_dock_load(void);
void pl_audio_dock_unload(void);

#ifdef __cplusplus
}

class QWidget;

/* Mais um painel Som (para a janela do Projeto Live). Já começa ligado. */
QWidget *pl_audio_dock_create(QWidget *parent);
#endif
