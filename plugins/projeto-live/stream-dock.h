/*
 * Projeto Live: aba "Multistream" nas Configurações e bolinhas na barra de baixo (veja
 * stream-dock.cpp).
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

/* Liga a aba nas Configurações e as bolinhas. Chamado no carregamento do módulo. */
void pl_multistream_ui_load(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
class QWidget;
/* A janela "Destinos" do somun live (uma só; se já estiver aberta, vem para a frente). */
void pl_destinations_window_open(QWidget *parent);
#endif
