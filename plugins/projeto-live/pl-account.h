/*
 * Projeto Live: a conta da igreja dentro do OBS (veja pl-account.cpp).
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

void pl_account_load(void);
void pl_account_unload(void);

#ifdef __cplusplus
}

class QWidget;

/* O selo do topo da janela do Projeto Live: o culto de hoje, ou "Entrar na conta". Clicar abre a
 * conta. */
QWidget *pl_account_badge_create(QWidget *parent);

/* Mostra os recados do diretor no canto de cima, à direita, desta janela. */
void pl_account_attach_toasts(QWidget *host);
#endif
