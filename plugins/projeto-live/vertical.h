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
 * Cenas verticais na ordem dos painéis: primeiro as gêmeas (na ordem das cenas do OBS), depois as
 * só verticais. `twin_of` é o nome da cena horizontal da gêmea, ou NULL numa cena só vertical.
 * Funções abaixo: thread da interface.
 */
typedef bool (*pl_vertical_scene_cb)(void *param, obs_source_t *scene, const char *twin_of);
void pl_vertical_enum_scenes(pl_vertical_scene_cb callback, void *param);

/* Cena que está no ar na tela vertical (nova referência ou NULL). */
obs_source_t *pl_vertical_current(void);
/* true: o vertical segue o Programa sozinho; false: alguém escolheu uma cena à mão. */
bool pl_vertical_following(void);
/* Põe uma cena vertical no ar à mão; fica até o Programa trocar de cena ou "Seguir o Programa". */
void pl_vertical_select(obs_source_t *scene);
void pl_vertical_follow_program(void);

/* Cena só vertical (nova referência). */
obs_source_t *pl_vertical_add_extra(const char *name);
/* Só remove cenas só verticais (as gêmeas somem junto com a cena horizontal). */
void pl_vertical_remove_extra(obs_source_t *scene);

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

/*
 * Fontes casadas. SAME: no vertical vai a própria fonte (o normal). OTHER: vai outra fonte (por
 * exemplo a mesma arte feita em pé). HIDE: a fonte não vai ao vertical. Mudar o casamento já
 * troca a fonte em todas as gêmeas.
 */
enum pl_vertical_pair_mode {
	PL_PAIR_SAME,
	PL_PAIR_OTHER,
	PL_PAIR_HIDE,
};
/* `*other` recebe uma nova referência (ou NULL) quando o modo é OTHER. */
enum pl_vertical_pair_mode pl_vertical_get_pair(obs_source_t *source, obs_source_t **other);
void pl_vertical_set_pair(obs_source_t *source, enum pl_vertical_pair_mode mode, obs_source_t *other);

#ifdef __cplusplus
}
#endif
