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
 * Versão vertical da cena que está no ar (nova referência ou NULL). Cada cena do OBS tem a sua,
 * criada sozinha; o vertical sempre acompanha o Programa. Thread da interface.
 */
obs_source_t *pl_vertical_current(void);
/* O mesmo, para uma tela já em mãos (serve na thread de vídeo). */
obs_source_t *pl_vertical_active_scene(obs_canvas_t *canvas);

/* A fonte original (da cena horizontal) que um item vertical representa (nova referência). */
obs_source_t *pl_vertical_item_original(obs_sceneitem_t *item);

/*
 * Trocar fonte no vertical (vale em todas as cenas). SAME: no vertical vai a própria fonte (o
 * normal). OTHER: vai outra fonte (ex.: a mesma contagem em 9:16). HIDE: a fonte não vai ao vertical.
 */
enum pl_vertical_pair_mode {
	PL_PAIR_SAME,
	PL_PAIR_OTHER,
	PL_PAIR_HIDE,
};
/* `*other` recebe uma nova referência (ou NULL) quando o modo é OTHER. */
enum pl_vertical_pair_mode pl_vertical_get_pair(obs_source_t *source, obs_source_t **other);
void pl_vertical_set_pair(obs_source_t *source, enum pl_vertical_pair_mode mode, obs_source_t *other);

/*
 * Transição do vertical: por padrão a mesma do Programa; com a preferência "transition_own", uma
 * escolhida só para o vertical. Chamar depois de mudar as preferências da transição.
 */
void pl_vertical_transition_changed(void);
/* A transição que o vertical está usando (nova referência ou NULL), para abrir as propriedades. */
obs_source_t *pl_vertical_get_transition(void);

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
