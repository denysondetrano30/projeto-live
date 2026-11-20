/*
 * somun live: a tecla de "segurar para falar com a equipe" (veja talk-key.cpp).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <QString>

class QWidget;

/* Liga a escuta do teclado (uma vez). */
void pl_talk_key_install(void);

/* Só vale enquanto a janela do somun live está em uso (no Modo OBS o teclado fica com o OBS). */
void pl_talk_key_set_active(bool active);

/* Nome da tecla escolhida, para o menu ("Espaço", "F1"...; vazio = nenhuma). */
QString pl_talk_key_name(void);

/* Janelinha "aperte a tecla": o operador escolhe a tecla (fica salva em tecla-falar.json). */
void pl_talk_key_choose(QWidget *parent);

/* Solta a fala, se estiver segurando (o OBS está fechando). */
void pl_talk_key_release(void);
