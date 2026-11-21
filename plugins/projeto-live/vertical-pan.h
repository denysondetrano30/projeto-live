/*
 * somun live: a janela 9:16 que desliza dentro da câmera (veja vertical-pan.cpp).
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

/* Registra a fonte da arte de fundo e as teclas de atalho (no carregamento do módulo). */
void pl_vertical_pan_load(void);
void pl_vertical_pan_unload(void);

#ifdef __cplusplus
}

class QWidget;
struct obs_source;

/* Os botões de deslizar, "voltar ao lugar" e a guia (embaixo da transição). */
QWidget *pl_vertical_pan_create(QWidget *parent);

/* Esta entrada tem câmera para enquadrar? */
bool pl_vertical_frame_available(struct obs_source *scene);

/* Botão direito da entrada > "Enquadrar no vertical". */
void pl_vertical_frame_open(struct obs_source *scene, QWidget *parent);

/* Desenha a guia (a parte do 16:9 que vai para o vertical) em cima do No ar, na thread de vídeo. */
void pl_vertical_pan_draw_guide(void);
#endif
