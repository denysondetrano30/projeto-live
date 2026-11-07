/*
 * Projeto Live: tela vertical (veja vertical.c).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PL_VERTICAL_WIDTH 1080
#define PL_VERTICAL_HEIGHT 1920

/* Passa a seguir os avisos do OBS (chamado no carregamento do módulo). */
void pl_vertical_load(void);
void pl_vertical_unload(void);

/* A tela vertical (nova referência, ou NULL se ainda não existe). Pode ser chamada de qualquer thread. */
obs_canvas_t *pl_vertical_get_canvas(void);
/* O vídeo da tela vertical, para os codificadores (NULL se não existe). Thread da interface. */
video_t *pl_vertical_get_video(void);

/* Aumenta sempre que a lista de cenas verticais ou o conteúdo delas muda (para os painéis). */
uint64_t pl_vertical_version(void);
void pl_vertical_touch(void);

/*
 * Cenas verticais, na ordem em que foram criadas. `linked_to` é o nome da cena horizontal ligada a
 * ela, ou NULL. Funções abaixo: thread da interface (menos pl_vertical_active_scene).
 */
typedef bool (*pl_vertical_scene_cb)(void *param, obs_source_t *scene, const char *linked_to);
void pl_vertical_enum_scenes(pl_vertical_scene_cb callback, void *param);

/* Cena que está no ar na tela vertical (nova referência ou NULL). */
obs_source_t *pl_vertical_current(void);
/* O mesmo, para uma tela já em mãos (serve na thread de vídeo). */
obs_source_t *pl_vertical_active_scene(obs_canvas_t *canvas);
/* Põe uma cena vertical no ar (com a transição do vertical). */
void pl_vertical_select(obs_source_t *scene);

/* Cenas verticais: criar (nova referência), renomear e apagar. */
obs_source_t *pl_vertical_add_scene(const char *name);
bool pl_vertical_rename_scene(obs_source_t *scene, const char *name);
void pl_vertical_remove_scene(obs_source_t *scene);

/*
 * Ligar uma cena vertical a uma cena horizontal: quando o Programa troca para a horizontal, o
 * vertical troca para a ligada. Cada cena horizontal liga com uma vertical só. NULL desliga.
 */
void pl_vertical_link(obs_source_t *vertical, obs_source_t *horizontal);
/* A cena horizontal ligada (nova referência ou NULL). */
obs_source_t *pl_vertical_linked(obs_source_t *vertical);

/*
 * Transição do vertical: por padrão a mesma do Programa; com a preferência "transition_own", uma
 * escolhida só para o vertical. Chamar depois de mudar as preferências da transição.
 */
void pl_vertical_transition_changed(void);
/* A transição que o vertical está usando (nova referência ou NULL), para abrir as propriedades. */
obs_source_t *pl_vertical_get_transition(void);

/* Coloca uma fonte numa cena vertical com o enquadramento automático. */
obs_sceneitem_t *pl_vertical_add_source(obs_source_t *scene, obs_source_t *source);

/*
 * Enquadramentos rápidos para um item da tela vertical. FILL: preenche a tela inteira, cortando as
 * sobras. WHOLE: imagem inteira, sem cortar. WIDTH: ocupa a largura toda, no meio da altura.
 */
enum pl_vertical_fit {
	PL_FIT_FILL,
	PL_FIT_WHOLE,
	PL_FIT_WIDTH,
};
void pl_vertical_fit_item(obs_sceneitem_t *item, enum pl_vertical_fit fit);

#ifdef __cplusplus
}
#endif
