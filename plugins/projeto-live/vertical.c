/*
 * Projeto Live: tela vertical (1080x1920) para Instagram, TikTok e gravação em pé.
 *
 * É uma segunda tela do OBS (um "canvas", recurso do OBS 31.1+) com cenas próprias, guardada
 * pelo próprio OBS dentro da coleção de cenas. Duas espécies de cena vertical:
 *
 * - Gêmeas: cada cena horizontal ganha uma gêmea "<nome> (vertical)", criada sozinha com as
 *   mesmas fontes num enquadramento automático (câmeras preenchem a tela em pé; telas cheias de
 *   slide ou letra aparecem inteiras; faixas e logos encolhem e ficam no alto ou embaixo). Quando
 *   o Programa troca de cena, o vertical troca para a gêmea. A pessoa pode ajeitar a gêmea à
 *   vontade depois: o plugin não refaz o enquadramento.
 * - Só verticais: criadas pela pessoa; entram no ar à mão e ficam até o Programa trocar de cena.
 *
 * Fontes casadas: o que acontece com uma fonte numa cena horizontal acontece com a "par vertical"
 * dela na gêmea (aparecer, sumir, entrar, sair). A par é a própria fonte, a não ser que a pessoa
 * case com outra (configuração privada "pl_vertical_pair", com o uuid da outra) ou diga que ela
 * não vai ao vertical ("pl_vertical_hide").
 *
 * Grupos: um grupo da cena horizontal vai para a gêmea como as fontes dele, cada uma no lugar
 * certo. Aparecer/sumir o grupo ou uma fonte dentro dele também vale no vertical.
 *
 * Por enquanto o vertical corta seco (sem transição).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "vertical.h"

#include <graphics/matrix4.h>
#include <graphics/vec3.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/darray.h>
#include <util/threading.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CANVAS_NAME "Projeto Live Vertical"
#define TWIN_KEY "pl_twin_of"
#define EXTRA_KEY "pl_vertical_extra"
#define PAIR_KEY "pl_vertical_pair"
#define HIDE_KEY "pl_vertical_hide"

static pthread_mutex_t canvas_mutex;
static obs_canvas_t *canvas; /* com canvas_mutex para escrever; leitura livre na thread da interface */
static bool loaded;
static bool ready;
static bool following = true;
static volatile long version;

/* Cenas horizontais cujos avisos (item entrou, saiu, apareceu) estão ligados. */
static DARRAY(obs_source_t *) watched;

/* ------------------------------------------------------------------------- */
/* Utilidades                                                                 */

uint64_t pl_vertical_version(void)
{
	return (uint64_t)os_atomic_load_long(&version);
}

void pl_vertical_touch(void)
{
	os_atomic_inc_long(&version);
}

obs_canvas_t *pl_vertical_get_canvas(void)
{
	pthread_mutex_lock(&canvas_mutex);
	obs_canvas_t *ref = canvas ? obs_canvas_get_ref(canvas) : NULL;
	pthread_mutex_unlock(&canvas_mutex);
	return ref;
}

video_t *pl_vertical_get_video(void)
{
	return canvas ? obs_canvas_get_video(canvas) : NULL;
}

static const char *private_string(obs_source_t *source, const char *key)
{
	/* A string fica válida enquanto as configurações privadas existirem (vivem com a fonte). */
	obs_data_t *settings = obs_source_get_private_settings(source);
	const char *value = obs_data_get_string(settings, key);
	obs_data_release(settings);
	return value;
}

static bool private_bool(obs_source_t *source, const char *key)
{
	obs_data_t *settings = obs_source_get_private_settings(source);
	bool value = obs_data_get_bool(settings, key);
	obs_data_release(settings);
	return value;
}

/* A fonte (ou cena) pertence à tela vertical? */
static bool in_vertical(obs_source_t *source)
{
	obs_canvas_t *owner = source ? obs_source_get_canvas(source) : NULL;
	bool inside = owner && owner == canvas;
	obs_canvas_release(owner);
	return inside;
}

static obs_source_t *find_twin(obs_source_t *horizontal);

/* A par vertical de uma fonte (nova referência), ou NULL se ela não vai ao vertical. */
static obs_source_t *vertical_pair(obs_source_t *source)
{
	if (private_bool(source, HIDE_KEY)) {
		return NULL;
	}
	const char *uuid = private_string(source, PAIR_KEY);
	if (uuid && *uuid) {
		obs_source_t *pair = obs_get_source_by_uuid(uuid);
		if (pair) {
			return pair;
		}
	}
	/* Cena horizontal dentro de outra cena: no vertical vai a gêmea dela. */
	if (obs_source_is_scene(source) && !in_vertical(source)) {
		obs_source_t *twin = find_twin(source);
		if (twin) {
			return twin;
		}
	}
	return obs_source_get_ref(source);
}

static bool is_camera(obs_source_t *source)
{
	static const char *cameras[] = {
		"projeto_live_phone_source",
		"dshow_input",
		"av_capture_input",
		"macos-avcapture",
		"macos-avcapture-fast",
		"v4l2_input",
		"decklink-input",
		"aja_source",
		"ndi_source",
	};
	const char *id = obs_source_get_unversioned_id(source);
	for (size_t i = 0; id && i < sizeof(cameras) / sizeof(cameras[0]); i++) {
		if (strcmp(id, cameras[i]) == 0) {
			return true;
		}
	}
	return false;
}

/* ------------------------------------------------------------------------- */
/* Enquadramento                                                              */

static void set_full_screen(obs_sceneitem_t *item, enum obs_bounds_type type, bool crop)
{
	struct obs_transform_info info = {0};
	info.pos.x = PL_VERTICAL_WIDTH / 2.0f;
	info.pos.y = PL_VERTICAL_HEIGHT / 2.0f;
	info.scale.x = 1.0f;
	info.scale.y = 1.0f;
	info.alignment = OBS_ALIGN_CENTER;
	info.bounds_type = type;
	info.bounds_alignment = OBS_ALIGN_CENTER;
	info.bounds.x = PL_VERTICAL_WIDTH;
	info.bounds.y = PL_VERTICAL_HEIGHT;
	info.crop_to_bounds = crop;
	obs_sceneitem_set_info2(item, &info);
}

void pl_vertical_fit_item(obs_sceneitem_t *item, enum pl_vertical_fit fit)
{
	switch (fit) {
	case PL_FIT_FILL:
		set_full_screen(item, OBS_BOUNDS_SCALE_OUTER, true);
		break;
	case PL_FIT_WHOLE:
		set_full_screen(item, OBS_BOUNDS_SCALE_INNER, false);
		break;
	case PL_FIT_WIDTH:
		set_full_screen(item, OBS_BOUNDS_SCALE_TO_WIDTH, false);
		break;
	}
	pl_vertical_touch();
}

/* Matriz da caixa de um item na tela horizontal; dentro de um grupo, já somada à do grupo. */
static void horizontal_transform(obs_sceneitem_t *item, obs_sceneitem_t *group, struct matrix4 *transform)
{
	obs_sceneitem_get_box_transform(item, transform);
	if (group) {
		struct matrix4 group_transform;
		obs_sceneitem_get_draw_transform(group, &group_transform);
		matrix4_mul(transform, transform, &group_transform);
	}
}

/* Retângulo que o item ocupa na tela horizontal. */
static void item_box(obs_sceneitem_t *item, obs_sceneitem_t *group, float *min_x, float *min_y, float *max_x,
		     float *max_y)
{
	struct matrix4 transform;
	horizontal_transform(item, group, &transform);
	*min_x = *min_y = INFINITY;
	*max_x = *max_y = -INFINITY;
	for (int corner = 0; corner < 4; corner++) {
		struct vec3 point;
		vec3_set(&point, (float)(corner & 1), (float)(corner >> 1), 0.0f);
		vec3_transform(&point, &point, &transform);
		*min_x = fminf(*min_x, point.x);
		*min_y = fminf(*min_y, point.y);
		*max_x = fmaxf(*max_x, point.x);
		*max_y = fmaxf(*max_y, point.y);
	}
}

/* Enquadramento automático na gêmea, a partir de onde o item está na cena horizontal. */
static void auto_frame(obs_sceneitem_t *vertical_item, obs_sceneitem_t *horizontal_item, obs_sceneitem_t *group)
{
	obs_source_t *source = obs_sceneitem_get_source(vertical_item);
	if (!horizontal_item) {
		pl_vertical_fit_item(vertical_item, is_camera(source) ? PL_FIT_FILL : PL_FIT_WHOLE);
		return;
	}

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi) || !ovi.base_width || !ovi.base_height) {
		pl_vertical_fit_item(vertical_item, PL_FIT_WHOLE);
		return;
	}
	float width = (float)ovi.base_width;
	float height = (float)ovi.base_height;

	float min_x, min_y, max_x, max_y;
	item_box(horizontal_item, group, &min_x, &min_y, &max_x, &max_y);

	struct obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(horizontal_item, &crop);
	obs_sceneitem_set_crop(vertical_item, &crop);

	/* Ocupa quase a tela toda: é câmera (preenche) ou slide/letra (inteiro). */
	float visible_w = fminf(max_x, width) - fmaxf(min_x, 0.0f);
	float visible_h = fminf(max_y, height) - fmaxf(min_y, 0.0f);
	float area = (visible_w > 0 && visible_h > 0) ? visible_w * visible_h : 0.0f;
	if (area >= 0.8f * width * height) {
		pl_vertical_fit_item(vertical_item, is_camera(source) ? PL_FIT_FILL : PL_FIT_WHOLE);
		return;
	}

	/* Faixa, logo etc.: encolhe na mesma proporção da largura e fica no alto ou embaixo. */
	float scale = PL_VERTICAL_WIDTH / width;
	float box_w = (max_x - min_x) * scale;
	float box_h = (max_y - min_y) * scale;
	float x = min_x * scale;
	bool top = (min_y + max_y) / 2.0f < height / 2.0f;
	float y = top ? min_y * scale : PL_VERTICAL_HEIGHT - (height - min_y) * scale;

	struct obs_transform_info info = {0};
	info.pos.x = x;
	info.pos.y = y;
	info.scale.x = 1.0f;
	info.scale.y = 1.0f;
	info.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	info.bounds_type = OBS_BOUNDS_SCALE_INNER;
	info.bounds_alignment = OBS_ALIGN_CENTER;
	info.bounds.x = fmaxf(box_w, 1.0f);
	info.bounds.y = fmaxf(box_h, 1.0f);
	obs_sceneitem_set_info2(vertical_item, &info);
	pl_vertical_touch();
}

obs_sceneitem_t *pl_vertical_add_source(obs_source_t *scene_source, obs_source_t *source)
{
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (!scene || !source) {
		return NULL;
	}
	obs_sceneitem_t *item = obs_scene_add(scene, source);
	if (item) {
		auto_frame(item, NULL, NULL);
	}
	return item;
}

/* ------------------------------------------------------------------------- */
/* Gêmeas                                                                     */

struct find_twin {
	const char *uuid;
	obs_source_t *found;
};

static bool find_twin_cb(void *param, obs_source_t *scene)
{
	struct find_twin *find = param;
	if (strcmp(private_string(scene, TWIN_KEY), find->uuid) == 0) {
		find->found = obs_source_get_ref(scene);
		return false;
	}
	return true;
}

/* A gêmea de uma cena horizontal (nova referência ou NULL). */
static obs_source_t *find_twin(obs_source_t *horizontal)
{
	struct find_twin find = {obs_source_get_uuid(horizontal), NULL};
	if (canvas && find.uuid) {
		obs_canvas_enum_scenes(canvas, find_twin_cb, &find);
	}
	return find.found;
}

static void twin_name(obs_source_t *horizontal, char *out, size_t size)
{
	snprintf(out, size, "%s (vertical)", obs_source_get_name(horizontal));
}

/* Um nome que ainda não existe na tela vertical. */
static void unique_name(const char *wanted, obs_source_t *self, char *out, size_t size)
{
	snprintf(out, size, "%s", wanted);
	for (int n = 2; n < 1000; n++) {
		obs_source_t *other = obs_canvas_get_source_by_name(canvas, out);
		bool taken = other && other != self;
		obs_source_release(other);
		if (!taken) {
			return;
		}
		snprintf(out, size, "%s %d", wanted, n);
	}
}

/* Itens da gêmea que mostram a par vertical de `source`. */
struct twin_items {
	obs_source_t *pair;
	DARRAY(obs_sceneitem_t *) items;
};

static bool twin_items_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct twin_items *found = param;
	if (obs_sceneitem_get_source(item) == found->pair) {
		obs_sceneitem_addref(item);
		da_push_back(found->items, &item);
	}
	return true;
}

static void collect_twin_items(obs_source_t *twin, obs_source_t *pair, struct twin_items *found)
{
	found->pair = pair;
	da_init(found->items);
	obs_scene_t *scene = obs_scene_from_source(twin);
	if (scene && pair) {
		obs_scene_enum_items(scene, twin_items_cb, found);
	}
}

static void free_twin_items(struct twin_items *found)
{
	for (size_t i = 0; i < found->items.num; i++) {
		obs_sceneitem_release(found->items.array[i]);
	}
	da_free(found->items);
}

static void add_twin_item(obs_source_t *twin, obs_sceneitem_t *horizontal_item, obs_sceneitem_t *group);

struct group_fill {
	obs_source_t *twin;
	obs_sceneitem_t *group;
};

static bool group_fill_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct group_fill *fill = param;
	add_twin_item(fill->twin, item, fill->group);
	return true;
}

/*
 * Põe na gêmea a par vertical de um item horizontal (se ainda não estiver lá). `group`: o grupo
 * em que o item está, ou NULL. Um grupo entra como as fontes dele.
 */
static void add_twin_item(obs_source_t *twin, obs_sceneitem_t *horizontal_item, obs_sceneitem_t *group)
{
	if (obs_sceneitem_is_group(horizontal_item)) {
		struct group_fill fill = {twin, horizontal_item};
		obs_sceneitem_group_enum_items(horizontal_item, group_fill_cb, &fill);
		return;
	}
	obs_source_t *pair = vertical_pair(obs_sceneitem_get_source(horizontal_item));
	if (!pair) {
		return;
	}
	struct twin_items found;
	collect_twin_items(twin, pair, &found);
	if (found.items.num == 0) {
		obs_sceneitem_t *item = obs_scene_add(obs_scene_from_source(twin), pair);
		if (item) {
			bool paired = pair != obs_sceneitem_get_source(horizontal_item);
			auto_frame(item, paired ? NULL : horizontal_item, group);
			bool visible = obs_sceneitem_visible(horizontal_item) &&
				       (!group || obs_sceneitem_visible(group));
			obs_sceneitem_set_visible(item, visible);
		}
	}
	free_twin_items(&found);
	obs_source_release(pair);
}

static bool fill_twin_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	add_twin_item(param, item, NULL);
	return true;
}

static void create_twin(obs_source_t *horizontal)
{
	char wanted[512];
	char name[512];
	twin_name(horizontal, wanted, sizeof(wanted));
	unique_name(wanted, NULL, name, sizeof(name));

	obs_scene_t *scene = obs_canvas_scene_create(canvas, name);
	if (!scene) {
		return;
	}
	obs_source_t *twin = obs_scene_get_source(scene);
	obs_data_t *settings = obs_source_get_private_settings(twin);
	obs_data_set_string(settings, TWIN_KEY, obs_source_get_uuid(horizontal));
	obs_data_release(settings);

	obs_scene_t *horizontal_scene = obs_scene_from_source(horizontal);
	if (horizontal_scene) {
		obs_scene_enum_items(horizontal_scene, fill_twin_cb, twin);
	}
	blog(LOG_INFO, "[projeto-live] vertical: gêmea '%s' criada", name);
	obs_scene_release(scene);
}

/* ------------------------------------------------------------------------- */
/* Procurar fontes também dentro dos grupos                                   */

struct has_source {
	obs_source_t *source;
	bool found;
};

static bool has_source_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct has_source *search = param;
	if (obs_sceneitem_get_source(item) == search->source) {
		search->found = true;
	} else if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, has_source_cb, search);
	}
	return !search->found;
}

/* A fonte está na cena horizontal (solta ou dentro de um grupo)? */
static bool scene_contains(obs_scene_t *scene, obs_source_t *source)
{
	struct has_source search = {source, false};
	if (scene && source) {
		obs_scene_enum_items(scene, has_source_cb, &search);
	}
	return search.found;
}

struct item_ref {
	obs_sceneitem_t *item;
	obs_sceneitem_t *group;
};

struct item_refs {
	DARRAY(struct item_ref) refs;
	obs_sceneitem_t *group;
};

static bool collect_refs_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct item_refs *list = param;
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_t *outer = list->group;
		list->group = item;
		obs_sceneitem_group_enum_items(item, collect_refs_cb, list);
		list->group = outer;
		return true;
	}
	struct item_ref ref = {item, list->group};
	obs_sceneitem_addref(item);
	if (ref.group) {
		obs_sceneitem_addref(ref.group);
	}
	da_push_back(list->refs, &ref);
	return true;
}

/* Todos os itens da cena horizontal, com o grupo de cada um (referências novas). */
static void collect_refs(obs_scene_t *scene, struct item_refs *list)
{
	da_init(list->refs);
	list->group = NULL;
	if (scene) {
		obs_scene_enum_items(scene, collect_refs_cb, list);
	}
}

static void free_refs(struct item_refs *list)
{
	for (size_t i = 0; i < list->refs.num; i++) {
		obs_sceneitem_release(list->refs.array[i].item);
		obs_sceneitem_release(list->refs.array[i].group);
	}
	da_free(list->refs);
}

/* ------------------------------------------------------------------------- */
/* Casar fontes                                                               */

enum pl_vertical_pair_mode pl_vertical_get_pair(obs_source_t *source, obs_source_t **other)
{
	*other = NULL;
	if (!source) {
		return PL_PAIR_SAME;
	}
	if (private_bool(source, HIDE_KEY)) {
		return PL_PAIR_HIDE;
	}
	const char *uuid = private_string(source, PAIR_KEY);
	if (uuid && *uuid) {
		*other = obs_get_source_by_uuid(uuid);
		if (*other) {
			return PL_PAIR_OTHER;
		}
	}
	return PL_PAIR_SAME;
}

/* Troca, nas gêmeas, a par antiga da fonte pela nova. */
static void repair_twins(obs_source_t *source, obs_source_t *old_pair)
{
	struct obs_frontend_source_list scenes = {0};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *horizontal = scenes.sources.array[i];
		obs_scene_t *scene = obs_scene_from_source(horizontal);
		obs_source_t *twin = find_twin(horizontal);
		if (!scene || !twin) {
			obs_source_release(twin);
			continue;
		}

		struct item_refs list;
		collect_refs(scene, &list);
		bool uses_source = false;
		for (size_t j = 0; j < list.refs.num; j++) {
			uses_source = uses_source || obs_sceneitem_get_source(list.refs.array[j].item) == source;
		}

		if (uses_source) {
			if (old_pair) {
				struct twin_items found;
				collect_twin_items(twin, old_pair, &found);
				for (size_t j = 0; j < found.items.num; j++) {
					obs_sceneitem_remove(found.items.array[j]);
				}
				free_twin_items(&found);
			}
			for (size_t j = 0; j < list.refs.num; j++) {
				if (obs_sceneitem_get_source(list.refs.array[j].item) == source) {
					add_twin_item(twin, list.refs.array[j].item, list.refs.array[j].group);
				}
			}
		}
		free_refs(&list);
		obs_source_release(twin);
	}
	obs_frontend_source_list_free(&scenes);
	pl_vertical_touch();
}

void pl_vertical_set_pair(obs_source_t *source, enum pl_vertical_pair_mode mode, obs_source_t *other)
{
	if (!source) {
		return;
	}
	obs_source_t *old_pair = vertical_pair(source);

	obs_data_t *settings = obs_source_get_private_settings(source);
	obs_data_set_bool(settings, HIDE_KEY, mode == PL_PAIR_HIDE);
	obs_data_set_string(settings, PAIR_KEY, (mode == PL_PAIR_OTHER && other) ? obs_source_get_uuid(other) : "");
	obs_data_release(settings);
	blog(LOG_INFO, "[projeto-live] vertical: '%s' casada com '%s'", obs_source_get_name(source),
	     mode == PL_PAIR_HIDE ? "(nada)"
				  : (mode == PL_PAIR_OTHER && other ? obs_source_get_name(other) : "ela mesma"));

	if (ready && canvas) {
		repair_twins(source, old_pair);
	}
	obs_source_release(old_pair);
}

/* ------------------------------------------------------------------------- */
/* Avisos das cenas horizontais (chegam em qualquer thread; o trabalho vai para a interface) */

enum item_event { ITEM_ADDED, ITEM_REMOVED, ITEM_VISIBLE };

struct child_state {
	obs_weak_source_t *source;
	bool visible;
};

struct item_task {
	enum item_event event;
	obs_weak_source_t *scene;
	obs_weak_source_t *source;
	int64_t item_id;
	bool visible;
	/* Quando o item é um grupo: as fontes dele, guardadas na hora do aviso (o grupo pode sumir). */
	DARRAY(struct child_state) children;
};

static void rewatch_all(void);

/* Grupo: a cena horizontal em que ele está (nova referência) e o item dele. */
static obs_source_t *group_parent(obs_source_t *group, obs_sceneitem_t **group_item)
{
	*group_item = NULL;
	obs_source_t *parent = NULL;
	struct obs_frontend_source_list scenes = {0};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; !parent && i < scenes.sources.num; i++) {
		obs_scene_t *scene = obs_scene_from_source(scenes.sources.array[i]);
		obs_sceneitem_t *item = scene ? obs_scene_get_group(scene, obs_source_get_name(group)) : NULL;
		if (item && obs_sceneitem_get_source(item) == group) {
			*group_item = item;
			parent = obs_source_get_ref(scenes.sources.array[i]);
		}
	}
	obs_frontend_source_list_free(&scenes);
	return parent;
}

/* Aparecer/sumir ou tirar da gêmea as pares de uma fonte. */
static void apply_to_pairs(obs_source_t *twin, obs_scene_t *top_scene, obs_source_t *source, enum item_event event,
			   bool visible)
{
	if (event == ITEM_REMOVED && scene_contains(top_scene, source)) {
		/* A fonte continua na cena horizontal em outro lugar (ex.: saiu de um grupo). */
		return;
	}
	obs_source_t *pair = vertical_pair(source);
	struct twin_items found;
	collect_twin_items(twin, pair, &found);
	for (size_t i = 0; i < found.items.num; i++) {
		if (event == ITEM_VISIBLE) {
			obs_sceneitem_set_visible(found.items.array[i], visible);
		} else {
			obs_sceneitem_remove(found.items.array[i]);
		}
	}
	free_twin_items(&found);
	obs_source_release(pair);
}

static void item_task_run(void *param)
{
	struct item_task *task = param;
	obs_source_t *horizontal = obs_weak_source_get_source(task->scene);
	obs_source_t *source = obs_weak_source_get_source(task->source);
	bool rewatch = false;

	/* O aviso pode vir de dentro de um grupo: o que vale é a cena horizontal em volta dele. */
	obs_sceneitem_t *group = NULL;
	obs_source_t *top = NULL;
	if (horizontal && obs_source_is_group(horizontal)) {
		top = group_parent(horizontal, &group);
	} else if (horizontal) {
		top = obs_source_get_ref(horizontal);
	}
	obs_source_t *twin = (ready && top) ? find_twin(top) : NULL;
	obs_scene_t *top_scene = top ? obs_scene_from_source(top) : NULL;
	obs_scene_t *scene = NULL;
	if (horizontal) {
		scene = obs_source_is_group(horizontal) ? obs_group_from_source(horizontal)
							: obs_scene_from_source(horizontal);
	}

	if (twin && task->event == ITEM_ADDED && source) {
		obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, task->item_id);
		if (item) {
			add_twin_item(twin, item, group);
			rewatch = obs_sceneitem_is_group(item);
		}
	} else if (twin && task->children.num) {
		/* Um grupo inteiro apareceu, sumiu ou saiu. */
		for (size_t i = 0; i < task->children.num; i++) {
			obs_source_t *child = obs_weak_source_get_source(task->children.array[i].source);
			if (child) {
				bool visible = task->visible && task->children.array[i].visible;
				apply_to_pairs(twin, top_scene, child, task->event, visible);
			}
			obs_source_release(child);
		}
		rewatch = task->event == ITEM_REMOVED;
	} else if (twin && source) {
		bool visible = task->visible && (!group || obs_sceneitem_visible(group));
		apply_to_pairs(twin, top_scene, source, task->event, visible);
	}
	if (twin) {
		pl_vertical_touch();
	}
	if (rewatch) {
		rewatch_all();
	}

	obs_source_release(twin);
	obs_source_release(top);
	obs_source_release(source);
	obs_source_release(horizontal);
	obs_weak_source_release(task->scene);
	obs_weak_source_release(task->source);
	for (size_t i = 0; i < task->children.num; i++) {
		obs_weak_source_release(task->children.array[i].source);
	}
	da_free(task->children);
	bfree(task);
}

static bool task_children_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct item_task *task = param;
	struct child_state child = {obs_source_get_weak_source(obs_sceneitem_get_source(item)),
				    obs_sceneitem_visible(item)};
	da_push_back(task->children, &child);
	return true;
}

static void queue_item_task(enum item_event event, calldata_t *data, bool visible)
{
	obs_scene_t *scene = calldata_ptr(data, "scene");
	obs_sceneitem_t *item = calldata_ptr(data, "item");
	if (!scene || !item) {
		return;
	}
	struct item_task *task = bzalloc(sizeof(*task));
	task->event = event;
	task->scene = obs_source_get_weak_source(obs_scene_get_source(scene));
	task->source = obs_source_get_weak_source(obs_sceneitem_get_source(item));
	task->item_id = obs_sceneitem_get_id(item);
	task->visible = visible;
	da_init(task->children);
	if (event != ITEM_ADDED && obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, task_children_cb, task);
	}
	obs_queue_task(OBS_TASK_UI, item_task_run, task, false);
}

static void on_item_add(void *param, calldata_t *data)
{
	UNUSED_PARAMETER(param);
	queue_item_task(ITEM_ADDED, data, true);
}

static void on_item_remove(void *param, calldata_t *data)
{
	UNUSED_PARAMETER(param);
	queue_item_task(ITEM_REMOVED, data, false);
}

static void on_item_visible(void *param, calldata_t *data)
{
	UNUSED_PARAMETER(param);
	queue_item_task(ITEM_VISIBLE, data, calldata_bool(data, "visible"));
}

static void unwatch_all(void)
{
	for (size_t i = 0; i < watched.num; i++) {
		signal_handler_t *signals = obs_source_get_signal_handler(watched.array[i]);
		signal_handler_disconnect(signals, "item_add", on_item_add, NULL);
		signal_handler_disconnect(signals, "item_remove", on_item_remove, NULL);
		signal_handler_disconnect(signals, "item_visible", on_item_visible, NULL);
		obs_source_release(watched.array[i]);
	}
	da_resize(watched, 0);
}

static void watch(obs_source_t *horizontal)
{
	signal_handler_t *signals = obs_source_get_signal_handler(horizontal);
	signal_handler_connect(signals, "item_add", on_item_add, NULL);
	signal_handler_connect(signals, "item_remove", on_item_remove, NULL);
	signal_handler_connect(signals, "item_visible", on_item_visible, NULL);
	obs_source_t *ref = obs_source_get_ref(horizontal);
	da_push_back(watched, &ref);
}

static bool watch_groups_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	UNUSED_PARAMETER(param);
	if (obs_sceneitem_is_group(item)) {
		watch(obs_sceneitem_get_source(item));
	}
	return true;
}

/* Liga os avisos de todas as cenas horizontais e dos grupos dentro delas. */
static void rewatch_all(void)
{
	unwatch_all();
	if (!ready) {
		return;
	}
	struct obs_frontend_source_list scenes = {0};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		watch(scenes.sources.array[i]);
		obs_scene_t *scene = obs_scene_from_source(scenes.sources.array[i]);
		if (scene) {
			obs_scene_enum_items(scene, watch_groups_cb, NULL);
		}
	}
	obs_frontend_source_list_free(&scenes);
}

/* ------------------------------------------------------------------------- */
/* Manter as gêmeas iguais à lista de cenas                                   */

struct orphans {
	struct obs_frontend_source_list *scenes;
	DARRAY(obs_source_t *) remove;
};

static bool orphans_cb(void *param, obs_source_t *scene)
{
	struct orphans *data = param;
	const char *uuid = private_string(scene, TWIN_KEY);
	if (!uuid || !*uuid) {
		return true;
	}
	for (size_t i = 0; i < data->scenes->sources.num; i++) {
		if (strcmp(obs_source_get_uuid(data->scenes->sources.array[i]), uuid) == 0) {
			return true;
		}
	}
	obs_source_t *ref = obs_source_get_ref(scene);
	da_push_back(data->remove, &ref);
	return true;
}

static void sync_twins(void)
{
	if (!ready || !canvas) {
		return;
	}

	struct obs_frontend_source_list scenes = {0};
	obs_frontend_get_scenes(&scenes);

	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *horizontal = scenes.sources.array[i];
		obs_source_t *twin = find_twin(horizontal);
		if (!twin) {
			create_twin(horizontal);
		} else {
			/* A cena horizontal mudou de nome: a gêmea acompanha. */
			char wanted[512];
			char name[512];
			twin_name(horizontal, wanted, sizeof(wanted));
			if (strncmp(obs_source_get_name(twin), wanted, strlen(wanted)) != 0) {
				unique_name(wanted, twin, name, sizeof(name));
				obs_source_set_name(twin, name);
			}
			obs_source_release(twin);
		}
	}
	rewatch_all();

	/* Cena horizontal apagada: a gêmea vai junto. */
	struct orphans data;
	data.scenes = &scenes;
	da_init(data.remove);
	obs_canvas_enum_scenes(canvas, orphans_cb, &data);
	for (size_t i = 0; i < data.remove.num; i++) {
		blog(LOG_INFO, "[projeto-live] vertical: gêmea '%s' removida",
		     obs_source_get_name(data.remove.array[i]));
		obs_source_remove(data.remove.array[i]);
		obs_source_release(data.remove.array[i]);
	}
	da_free(data.remove);

	obs_frontend_source_list_free(&scenes);
	pl_vertical_touch();
}

/* ------------------------------------------------------------------------- */
/* Que cena está no ar                                                        */

static void set_on_air(obs_source_t *scene)
{
	if (canvas) {
		obs_canvas_set_channel(canvas, 0, scene);
		pl_vertical_touch();
	}
}

void pl_vertical_follow_program(void)
{
	following = true;
	if (!ready || !canvas) {
		return;
	}
	obs_source_t *program = obs_frontend_get_current_scene();
	if (!program) {
		return;
	}
	obs_source_t *twin = find_twin(program);
	if (!twin) {
		sync_twins();
		twin = find_twin(program);
	}
	if (twin) {
		set_on_air(twin);
	}
	obs_source_release(twin);
	obs_source_release(program);
}

void pl_vertical_select(obs_source_t *scene)
{
	if (!in_vertical(scene)) {
		return;
	}
	following = false;
	set_on_air(scene);
}

bool pl_vertical_following(void)
{
	return following;
}

obs_source_t *pl_vertical_current(void)
{
	return canvas ? obs_canvas_get_channel(canvas, 0) : NULL;
}

/* ------------------------------------------------------------------------- */
/* Lista de cenas para os painéis                                             */

struct scene_list {
	DARRAY(obs_source_t *) extras;
};

static bool extras_cb(void *param, obs_source_t *scene)
{
	struct scene_list *list = param;
	const char *twin = private_string(scene, TWIN_KEY);
	if (!twin || !*twin) {
		obs_source_t *ref = obs_source_get_ref(scene);
		da_push_back(list->extras, &ref);
	}
	return true;
}

void pl_vertical_enum_scenes(pl_vertical_scene_cb callback, void *param)
{
	if (!ready || !canvas) {
		return;
	}

	bool go_on = true;
	struct obs_frontend_source_list scenes = {0};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; go_on && i < scenes.sources.num; i++) {
		obs_source_t *twin = find_twin(scenes.sources.array[i]);
		if (twin) {
			go_on = callback(param, twin, obs_source_get_name(scenes.sources.array[i]));
			obs_source_release(twin);
		}
	}
	obs_frontend_source_list_free(&scenes);

	struct scene_list list;
	da_init(list.extras);
	obs_canvas_enum_scenes(canvas, extras_cb, &list);
	for (size_t i = 0; i < list.extras.num; i++) {
		if (go_on) {
			go_on = callback(param, list.extras.array[i], NULL);
		}
		obs_source_release(list.extras.array[i]);
	}
	da_free(list.extras);
}

obs_source_t *pl_vertical_add_extra(const char *wanted)
{
	if (!ready || !canvas) {
		return NULL;
	}
	char name[512];
	unique_name(wanted && *wanted ? wanted : "Cena vertical", NULL, name, sizeof(name));
	obs_scene_t *scene = obs_canvas_scene_create(canvas, name);
	if (!scene) {
		return NULL;
	}
	obs_source_t *source = obs_source_get_ref(obs_scene_get_source(scene));
	obs_data_t *settings = obs_source_get_private_settings(source);
	obs_data_set_bool(settings, EXTRA_KEY, true);
	obs_data_release(settings);
	obs_scene_release(scene);
	pl_vertical_touch();
	return source;
}

void pl_vertical_remove_extra(obs_source_t *scene)
{
	if (!in_vertical(scene)) {
		return;
	}
	const char *twin = private_string(scene, TWIN_KEY);
	if (twin && *twin) {
		return;
	}
	obs_source_t *current = pl_vertical_current();
	bool on_air = current == scene;
	obs_source_release(current);

	obs_source_remove(scene);
	if (on_air) {
		pl_vertical_follow_program();
	}
	pl_vertical_touch();
}

/* ------------------------------------------------------------------------- */
/* Criar ou achar a tela vertical                                             */

static void release_canvas(void)
{
	ready = false;
	unwatch_all();
	pthread_mutex_lock(&canvas_mutex);
	obs_canvas_t *old = canvas;
	canvas = NULL;
	pthread_mutex_unlock(&canvas_mutex);
	obs_canvas_release(old);
	pl_vertical_touch();
}

static void setup_canvas(void)
{
	release_canvas();

	obs_canvas_t *found = NULL;
	struct obs_frontend_canvas_list list = {0};
	obs_frontend_get_canvases(&list);
	for (size_t i = 0; i < list.canvases.num; i++) {
		const char *name = obs_canvas_get_name(list.canvases.array[i]);
		if (!found && name && strcmp(name, CANVAS_NAME) == 0) {
			found = obs_canvas_get_ref(list.canvases.array[i]);
		}
	}
	obs_frontend_canvas_list_free(&list);

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		obs_canvas_release(found);
		return;
	}
	ovi.base_width = ovi.output_width = PL_VERTICAL_WIDTH;
	ovi.base_height = ovi.output_height = PL_VERTICAL_HEIGHT;

	if (!found) {
		/* Sem MIX_AUDIO: o som continua sendo só o da mesa do OBS (senão sairia dobrado). */
		found = obs_frontend_add_canvas(CANVAS_NAME, &ovi, ACTIVATE | SCENE_REF);
		blog(LOG_INFO, "[projeto-live] vertical: tela criada");
	} else if (!obs_canvas_has_video(found)) {
		/* Tela carregada da coleção de cenas: o OBS não guarda o tamanho dela. */
		obs_canvas_reset_video(found, &ovi);
	}
	if (!found) {
		blog(LOG_WARNING, "[projeto-live] vertical: não foi possível criar a tela");
		return;
	}

	pthread_mutex_lock(&canvas_mutex);
	canvas = found;
	pthread_mutex_unlock(&canvas_mutex);
	ready = true;

	sync_twins();
	pl_vertical_follow_program();
}

static void frontend_event(enum obs_frontend_event event, void *param)
{
	UNUSED_PARAMETER(param);
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		setup_canvas();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_EXIT:
		release_canvas();
		break;
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		sync_twins();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		pl_vertical_follow_program();
		break;
	default:
		break;
	}
}

/* Renomear uma cena não avisa a lista de cenas: o aviso global vai para a interface. */
static void rename_task(void *param)
{
	UNUSED_PARAMETER(param);
	sync_twins();
}

static void on_source_rename(void *param, calldata_t *data)
{
	UNUSED_PARAMETER(param);
	obs_source_t *source = calldata_ptr(data, "source");
	if (source && obs_source_is_scene(source) && !in_vertical(source)) {
		obs_queue_task(OBS_TASK_UI, rename_task, NULL, false);
	}
}

void pl_vertical_load(void)
{
	if (loaded) {
		return;
	}
	pthread_mutex_init(&canvas_mutex, NULL);
	da_init(watched);
	obs_frontend_add_event_callback(frontend_event, NULL);
	signal_handler_connect(obs_get_signal_handler(), "source_rename", on_source_rename, NULL);
	loaded = true;
}

void pl_vertical_unload(void)
{
	if (!loaded) {
		return;
	}
	signal_handler_disconnect(obs_get_signal_handler(), "source_rename", on_source_rename, NULL);
	obs_frontend_remove_event_callback(frontend_event, NULL);
	release_canvas();
	da_free(watched);
	pthread_mutex_destroy(&canvas_mutex);
	loaded = false;
}
