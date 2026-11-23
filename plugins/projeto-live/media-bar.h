/*
 * somun live: controles do vídeo que está no ar (a faixa fininha embaixo do No ar) e os mesmos
 * controles no botão direito da entrada.
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

/* A faixa: tocar/pausar, recomeçar e quanto falta do vídeo da entrada que está no ar. Some sozinha
 * quando não tem vídeo no ar. */
QWidget *pl_media_bar_create(QWidget *parent);

/* Liga ou desliga a faixa (no modo normal ela fica embaixo da Prévia, que vira o No ar; no modo
 * estúdio, embaixo do No ar). Desligada, ela não aparece nem com vídeo. */
void pl_media_bar_set_allowed(QWidget *bar, bool allowed);

/* Acrescenta ao menu da entrada "Pausar o vídeo"/"Tocar o vídeo" e "Recomeçar o vídeo", se a entrada
 * tiver vídeo. Não faz nada se não tiver. */
void pl_media_menu(QMenu *menu, obs_source_t *scene);
