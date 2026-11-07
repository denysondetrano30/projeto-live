/*
 * Projeto Live: tela vertical (1080x1920) para Instagram, TikTok e gravação em pé.
 *
 * É uma segunda tela do OBS (um "canvas", recurso do OBS 31.1+) com cenas próprias, guardada
 * pelo próprio OBS dentro da coleção de cenas. Funciona como o plugin Aitum Vertical:
 *
 * - As cenas verticais são criadas pela pessoa, cada uma com as fontes dela (novas, ou fontes que
 *   já existem na tela horizontal). Cada tela controla as próprias fontes.
 * - Uma cena vertical pode ser "ligada" a uma cena horizontal (configuração privada "pl_link", com
 *   o uuid da horizontal): quando o Programa troca para a horizontal, o vertical troca para a
 *   ligada. Cena horizontal sem ligação não mexe no vertical. Cada horizontal liga com uma só.
 * - Clicar numa cena vertical põe ela no ar no vertical.
 *
 * As antigas "gêmeas" (até 07/11, "pl_twin_of") viram cenas comuns, já ligadas à cena delas.
 *
 * Transição: por padrão o vertical usa a mesma transição do Programa (o mesmo tipo, as mesmas
 * configurações e a mesma duração), numa cópia privada que fica no canal da tela vertical. Dá para
 * escolher uma só para o vertical no painel "Cenas verticais" (preferências em vertical.json).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "vertical.h"
#include "vertical-record.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/darray.h>
#include <util/threading.h>

#include <stdio.h>
#include <string.h>

#define CANVAS_NAME "Projeto Live Vertical"
#define LINK_KEY "pl_link"
#define OLD_TWIN_KEY "pl_twin_of"

static pthread_mutex_t canvas_mutex;
/* Com canvas_mutex para escrever; leitura livre na thread da interface. */
static obs_canvas_t *canvas;
static bool loaded;
static bool ready;
/* Cópia privada da transição do Programa (ou a própria), no canal 0 da tela vertical. */
static obs_source_t *transition;
/* A transição atual é uma escolhida só para o vertical (e não a cópia da do Programa)? */
static bool transition_is_own;
static bool ready_to_animate;
static volatile long version;

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
	return value ? value : "";
}

static void set_private_string(obs_source_t *source, const char *key, const char *value)
{
	obs_data_t *settings = obs_source_get_private_settings(source);
	obs_data_set_string(settings, key, value ? value : "");
	obs_data_release(settings);
}

/* A fonte (ou cena) pertence à tela vertical? */
static bool in_vertical(obs_source_t *source)
{
	obs_canvas_t *owner = source ? obs_source_get_canvas(source) : NULL;
	bool inside = owner && owner == canvas;
	obs_canvas_release(owner);
	return inside;
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

obs_sceneitem_t *pl_vertical_add_source(obs_source_t *scene_source, obs_source_t *source)
{
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (!scene || !source) {
		return NULL;
	}
	obs_sceneitem_t *item = obs_scene_add(scene, source);
	if (item) {
		/* Câmera preenche a tela em pé; o resto (slide, letra, arte) aparece inteiro. */
		pl_vertical_fit_item(item, is_camera(source) ? PL_FIT_FILL : PL_FIT_WHOLE);
	}
	return item;
}

/* ------------------------------------------------------------------------- */
/* Ligações entre cenas horizontais e verticais                               */

struct find_link {
	const char *uuid;
	obs_source_t *found;
};

static bool find_link_cb(void *param, obs_source_t *scene)
{
	struct find_link *find = param;
	if (strcmp(private_string(scene, LINK_KEY), find->uuid) == 0) {
		find->found = obs_source_get_ref(scene);
		return false;
	}
	return true;
}

/* A cena vertical ligada a uma cena horizontal (nova referência ou NULL). */
static obs_source_t *find_linked_vertical(obs_source_t *horizontal)
{
	struct find_link find = {obs_source_get_uuid(horizontal), NULL};
	if (canvas && find.uuid && *find.uuid) {
		obs_canvas_enum_scenes(canvas, find_link_cb, &find);
	}
	return find.found;
}

obs_source_t *pl_vertical_linked(obs_source_t *vertical)
{
	const char *uuid = vertical ? private_string(vertical, LINK_KEY) : "";
	return *uuid ? obs_get_source_by_uuid(uuid) : NULL;
}

void pl_vertical_link(obs_source_t *vertical, obs_source_t *horizontal)
{
	if (!in_vertical(vertical)) {
		return;
	}
	if (horizontal) {
		/* Cada horizontal liga com uma vertical só: a ligação antiga sai. */
		obs_source_t *previous = find_linked_vertical(horizontal);
		if (previous && previous != vertical) {
			set_private_string(previous, LINK_KEY, "");
		}
		obs_source_release(previous);
	}
	set_private_string(vertical, LINK_KEY, horizontal ? obs_source_get_uuid(horizontal) : "");
	blog(LOG_INFO, "[projeto-live] vertical: '%s' ligada a '%s'", obs_source_get_name(vertical),
	     horizontal ? obs_source_get_name(horizontal) : "(nenhuma)");
	pl_vertical_touch();
}

/* Gêmeas antigas viram cenas comuns, ligadas à cena horizontal delas. */
static bool migrate_cb(void *param, obs_source_t *scene)
{
	UNUSED_PARAMETER(param);
	const char *twin_of = private_string(scene, OLD_TWIN_KEY);
	if (*twin_of) {
		if (!*private_string(scene, LINK_KEY)) {
			set_private_string(scene, LINK_KEY, twin_of);
		}
		set_private_string(scene, OLD_TWIN_KEY, "");
	}
	return true;
}

/* Cena horizontal apagada: a ligação some (a cena vertical fica). */
static bool dangling_cb(void *param, obs_source_t *scene)
{
	UNUSED_PARAMETER(param);
	const char *uuid = private_string(scene, LINK_KEY);
	if (*uuid) {
		obs_source_t *horizontal = obs_get_source_by_uuid(uuid);
		if (!horizontal || obs_source_removed(horizontal)) {
			set_private_string(scene, LINK_KEY, "");
			pl_vertical_touch();
		}
		obs_source_release(horizontal);
	}
	return true;
}

/* ------------------------------------------------------------------------- */
/* Que cena está no ar (com transição)                                        */

obs_source_t *pl_vertical_active_scene(obs_canvas_t *target)
{
	obs_source_t *channel = target ? obs_canvas_get_channel(target, 0) : NULL;
	if (channel && obs_source_get_type(channel) == OBS_SOURCE_TYPE_TRANSITION) {
		obs_source_t *active = obs_transition_get_active_source(channel);
		obs_source_release(channel);
		return active;
	}
	return channel;
}

/* Guarda as configurações da transição própria do vertical (para voltarem ao abrir o OBS). */
static void save_own_transition(void)
{
	if (!transition || !transition_is_own) {
		return;
	}
	obs_data_t *settings = obs_source_get_settings(transition);
	pl_vertical_set_pref_obj("transition_settings", settings);
	pl_vertical_set_pref_string("transition_settings_id", obs_source_get_id(transition));
	obs_data_release(settings);
}

/* A transição do vertical: a do Programa ou a própria (cria ou troca se precisar). */
static obs_source_t *matching_transition(void)
{
	bool own = pl_vertical_pref("transition_own");
	obs_source_t *program = own ? NULL : obs_frontend_get_current_transition();
	const char *own_id = pl_vertical_pref_string("transition_id");
	const char *id = own ? (own_id && *own_id ? own_id : "fade_transition")
			     : (program ? obs_source_get_id(program) : "cut_transition");
	obs_data_t *settings = NULL;
	if (!own && program) {
		settings = obs_source_get_settings(program);
	} else if (own && strcmp(pl_vertical_pref_string("transition_settings_id"), id) == 0) {
		settings = pl_vertical_pref_obj("transition_settings");
	}

	if (transition && transition_is_own == own && strcmp(obs_source_get_id(transition), id) == 0) {
		/* A cópia acompanha as mudanças na transição do Programa; a própria fica como está. */
		if (!own) {
			obs_source_update(transition, settings);
		}
	} else {
		save_own_transition();
		obs_source_t *fresh = obs_source_create_private(id, "Projeto Live: transição vertical", settings);
		if (fresh) {
			obs_transition_set_size(fresh, PL_VERTICAL_WIDTH, PL_VERTICAL_HEIGHT);
			obs_transition_set_alignment(fresh, OBS_ALIGN_CENTER);
			obs_transition_set_scale_type(fresh, OBS_TRANSITION_SCALE_ASPECT);
			/* Começa mostrando o que já estava no ar, para não piscar. */
			obs_source_t *showing = pl_vertical_active_scene(canvas);
			obs_transition_set(fresh, showing);
			obs_source_release(showing);
			obs_canvas_set_channel(canvas, 0, fresh);
			obs_source_release(transition);
			transition = fresh;
			transition_is_own = own;
		}
	}
	obs_data_release(settings);
	obs_source_release(program);
	return transition;
}

static void set_on_air(obs_source_t *scene, bool animate)
{
	if (!canvas) {
		return;
	}
	obs_source_t *current = pl_vertical_active_scene(canvas);
	bool already = current == scene;
	obs_source_release(current);

	obs_source_t *with = matching_transition();
	if (!with) {
		obs_canvas_set_channel(canvas, 0, scene);
	} else if (!already) {
		int duration = transition_is_own ? pl_vertical_pref_int("transition_ms")
						 : obs_frontend_get_transition_duration();
		if (!animate || !obs_transition_start(with, OBS_TRANSITION_MODE_AUTO,
						      duration > 0 ? (uint32_t)duration : 300, scene)) {
			obs_transition_set(with, scene);
		}
	}
	pl_vertical_touch();
}

void pl_vertical_transition_changed(void)
{
	if (ready && canvas) {
		matching_transition();
	}
}

obs_source_t *pl_vertical_get_transition(void)
{
	return transition ? obs_source_get_ref(transition) : NULL;
}

/* O Programa trocou de cena: se ela tem uma cena vertical ligada, o vertical troca junto. */
static void program_changed(bool animate)
{
	if (!ready || !canvas) {
		return;
	}
	obs_source_t *program = obs_frontend_get_current_scene();
	obs_source_t *linked = program ? find_linked_vertical(program) : NULL;
	if (linked) {
		set_on_air(linked, animate);
	}
	obs_source_release(linked);
	obs_source_release(program);
}

void pl_vertical_select(obs_source_t *scene)
{
	if (in_vertical(scene)) {
		set_on_air(scene, true);
	}
}

obs_source_t *pl_vertical_current(void)
{
	return canvas ? pl_vertical_active_scene(canvas) : NULL;
}

/* ------------------------------------------------------------------------- */
/* Cenas verticais                                                            */

struct scene_list {
	pl_vertical_scene_cb callback;
	void *param;
};

static bool enum_cb(void *param, obs_source_t *scene)
{
	struct scene_list *list = param;
	obs_source_t *horizontal = pl_vertical_linked(scene);
	bool go_on = list->callback(list->param, scene, horizontal ? obs_source_get_name(horizontal) : NULL);
	obs_source_release(horizontal);
	return go_on;
}

struct scene_refs {
	DARRAY(obs_source_t *) refs;
};

static bool copy_scene_cb(void *data, obs_source_t *scene)
{
	struct scene_refs *list = data;
	obs_source_t *ref = obs_source_get_ref(scene);
	da_push_back(list->refs, &ref);
	return true;
}

void pl_vertical_enum_scenes(pl_vertical_scene_cb callback, void *param)
{
	if (!ready || !canvas) {
		return;
	}
	/* Copia a lista antes (a função de volta pode mexer nas cenas). */
	struct scene_refs scenes;
	da_init(scenes.refs);
	obs_canvas_enum_scenes(canvas, copy_scene_cb, &scenes);
	struct scene_list list = {callback, param};
	bool go_on = true;
	for (size_t i = 0; i < scenes.refs.num; i++) {
		if (go_on) {
			go_on = enum_cb(&list, scenes.refs.array[i]);
		}
		obs_source_release(scenes.refs.array[i]);
	}
	da_free(scenes.refs);
}

obs_source_t *pl_vertical_add_scene(const char *wanted)
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
	obs_scene_release(scene);

	/* A primeira cena criada já entra no ar, para o vertical não ficar preto. */
	obs_source_t *current = pl_vertical_current();
	if (!current) {
		set_on_air(source, false);
	}
	obs_source_release(current);
	pl_vertical_touch();
	return source;
}

bool pl_vertical_rename_scene(obs_source_t *scene, const char *wanted)
{
	if (!in_vertical(scene) || !wanted || !*wanted) {
		return false;
	}
	char name[512];
	unique_name(wanted, scene, name, sizeof(name));
	obs_source_set_name(scene, name);
	pl_vertical_touch();
	return true;
}

void pl_vertical_remove_scene(obs_source_t *scene)
{
	if (!in_vertical(scene)) {
		return;
	}
	obs_source_t *current = pl_vertical_current();
	bool on_air = current == scene;
	obs_source_release(current);

	obs_source_remove(scene);
	if (on_air && canvas) {
		/* A que estava no ar sumiu: entra a ligada ao Programa, ou nada. */
		if (transition) {
			obs_transition_set(transition, NULL);
		}
		program_changed(false);
	}
	pl_vertical_touch();
}

/* ------------------------------------------------------------------------- */
/* Criar ou achar a tela vertical                                             */

static void release_canvas(void)
{
	ready = false;
	ready_to_animate = false;
	save_own_transition();
	if (canvas && transition) {
		obs_canvas_set_channel(canvas, 0, NULL);
	}
	obs_source_release(transition);
	transition = NULL;
	pthread_mutex_lock(&canvas_mutex);
	obs_canvas_t *old = canvas;
	canvas = NULL;
	pthread_mutex_unlock(&canvas_mutex);
	obs_canvas_release(old);
	pl_vertical_touch();
}

static bool first_scene_cb(void *param, obs_source_t *scene)
{
	obs_source_t **first = param;
	*first = obs_source_get_ref(scene);
	return false;
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

	obs_canvas_enum_scenes(canvas, migrate_cb, NULL);
	obs_canvas_enum_scenes(canvas, dangling_cb, NULL);

	/* No ar ao abrir: a ligada ao Programa; se não tiver, a primeira cena vertical. */
	program_changed(false);
	obs_source_t *current = pl_vertical_current();
	if (!current) {
		obs_source_t *first = NULL;
		obs_canvas_enum_scenes(canvas, first_scene_cb, &first);
		if (first) {
			set_on_air(first, false);
		}
		obs_source_release(first);
	}
	obs_source_release(current);
	ready_to_animate = true;
	pl_vertical_touch();
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
		if (ready && canvas) {
			obs_canvas_enum_scenes(canvas, dangling_cb, NULL);
		}
		pl_vertical_touch();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		program_changed(ready_to_animate);
		break;
	default:
		break;
	}
}

/* Renomear uma cena horizontal muda o nome mostrado na ligação. */
static void on_source_rename(void *param, calldata_t *data)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(data);
	pl_vertical_touch();
}

void pl_vertical_load(void)
{
	if (loaded) {
		return;
	}
	pthread_mutex_init(&canvas_mutex, NULL);
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
	pthread_mutex_destroy(&canvas_mutex);
	loaded = false;
}
