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

#include <QDateTime>
#include <QString>
#include <vector>

/* Um item do roteiro do culto (da última versão publicada no Diretor). */
struct PLRoteiroItem {
	QString id;
	QString titulo;
	QString tipo;
	int minutos = 0; /* duração prevista; 0 = sem duração */
};

/* O culto que está sendo ouvido: o roteiro e o item que a Projeção diz que está no telão. */
struct PLRoteiro {
	QString nome;
	QString hora; /* HH:MM */
	std::vector<PLRoteiroItem> itens;
	QString agoraId; /* vazio = a Projeção não disse */
	QString agoraTitulo;
	QDateTime agoraEm; /* quando o item entrou no telão */
};

/* Falso quando não há conta ou culto. */
bool pl_account_roteiro(PLRoteiro &out);
#endif
