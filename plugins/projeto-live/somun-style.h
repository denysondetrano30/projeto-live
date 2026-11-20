/*
 * somun live: a cara do somun nas nossas janelas (cores, fonte Outfit e o logo "live.").
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <QString>

class QLabel;
class QWidget;

/* Cores do somun (as mesmas do site do Diretor). Vermelho só para o que está no ar, atrasado ou erro. */
#define SOMUN_BLACK "#141414"
#define SOMUN_PANEL "#1C1C1C"
#define SOMUN_PANEL2 "#232323"
#define SOMUN_LINE "#2A2A2A"
#define SOMUN_LINE2 "#3A3A3A"
#define SOMUN_TEXT "#F2F2F2"
#define SOMUN_GRAY "#8C8C8C"
#define SOMUN_GRAY2 "#A8A8A8"
#define SOMUN_WHITE "#FFFFFF"
#define SOMUN_RED "#E5483B"

/* Carrega a Outfit que vem junto com o plugin (uma vez). Devolve o nome da família ("Outfit"), ou o
 * nome da fonte normal do sistema se o arquivo não estiver lá. */
QString somun_font_family();

/* A folha de estilo das nossas janelas grandes (Operar e Acompanhar). */
QString somun_window_style();

/* O logo do app: a palavra em Outfit 800 branca e o ponto vermelho ("live.", "un."...). */
QLabel *somun_brand_label(QWidget *parent, const QString &word, int pixelSize = 22);

/* Deixa um botão com a cara do botão principal (branco, letra preta). */
void somun_make_primary(QWidget *button);
