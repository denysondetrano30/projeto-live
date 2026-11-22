/*
 * somun live: janela "Filtros do som" (parte H1; veja audio-filters.cpp).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <obs.h>

class QWidget;

/* Abre a janela "Filtros do som" deste canal (uma por canal; se já estiver aberta, vem para a frente). */
void pl_audio_filters_open(obs_source_t *source, QWidget *parent);
