/*
 * Projeto Live: o som que segue a cena (veja scene-audio.cpp).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <obs.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void pl_scene_audio_load(void);
void pl_scene_audio_unload(void);

/* Aquele som fica calado enquanto esta cena estiver no ar? */
bool pl_scene_audio_is_silenced(obs_source_t *scene, const char *source_name);

/* Liga ou desliga aquele som nesta cena, e já vale se a cena estiver no ar. */
void pl_scene_audio_set_silenced(obs_source_t *scene, const char *source_name, bool silenced);

/* Tem algum som calado por causa da cena que está no ar? */
bool pl_scene_audio_has_silenced(void);

/* "Mesa junto": devolve os sons calados até o próximo corte. */
void pl_scene_audio_bring_back(void);

/* O "Mesa junto" está valendo agora? */
bool pl_scene_audio_brought_back(void);

/* Abre a janela "Som das cenas". */
void pl_scene_audio_show_window(void);

#ifdef __cplusplus
}
#endif
