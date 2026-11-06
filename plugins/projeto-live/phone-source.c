/*
 * Projeto Live: fonte "Celular".
 *
 * Mostra a imagem de um celular com o app do Projeto Live. Por dentro, é uma Fonte de mídia
 * do OBS (ffmpeg_source) já configurada para SRT, sem buffer e com decodificação por hardware.
 * O voluntário só escolhe o celular numa lista; se o celular mudar de endereço na rede,
 * a fonte acha de novo sozinha.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include <stdio.h>
#include <string.h>

#include <obs-module.h>
#include <util/bmem.h>
#include <util/threading.h>

#include "phone-discovery.h"

#define S_DEVICE "device"
#define S_MANUAL "manual_address"
#define S_REFRESH "refresh"
#define S_HELP "help"

#define REFRESH_INTERVAL_SEC 3.0f
#define URL_MAX 256

struct phone_source {
	obs_source_t *source;
	obs_source_t *media;
	char *device_name;
	char *manual_address;
	char current_url[URL_MAX];
	float refresh_timer;
	volatile bool refresh_pending;
};

static const char *phone_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("PhoneSource");
}

/* Monta o endereço SRT a partir do que foi escolhido: endereço digitado ou celular da lista. */
static void build_url(struct phone_source *ps, char *url, size_t size)
{
	url[0] = 0;

	const char *manual = ps->manual_address;
	if (manual && *manual) {
		if (strncmp(manual, "srt://", 6) == 0)
			snprintf(url, size, "%s", manual);
		else if (strchr(manual, ':'))
			snprintf(url, size, "srt://%s", manual);
		else
			snprintf(url, size, "srt://%s:%d", manual, PL_DEFAULT_SRT_PORT);
		return;
	}

	struct pl_phone phone;
	if (ps->device_name && *ps->device_name && pl_discovery_find(ps->device_name, &phone))
		snprintf(url, size, "srt://%s:%d", phone.address, phone.srt_port);
}

/* Passa o endereço para a Fonte de mídia interna, com os ajustes de atraso mínimo. */
static void apply_url(struct phone_source *ps, const char *url)
{
	if (!ps->media || strcmp(url, ps->current_url) == 0)
		return;

	snprintf(ps->current_url, sizeof(ps->current_url), "%s", url);

	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", false);
	obs_data_set_string(settings, "input", url);
	obs_data_set_string(settings, "input_format", "mpegts");
	obs_data_set_int(settings, "buffering_mb", 0);
	obs_data_set_bool(settings, "hw_decode", true);
	obs_data_set_int(settings, "reconnect_delay_sec", 2);
	obs_data_set_bool(settings, "restart_on_activate", false);
	obs_data_set_bool(settings, "close_when_inactive", false);
	obs_data_set_bool(settings, "clear_on_media_end", true);
	obs_data_set_bool(settings, "log_changes", false);
	obs_source_update(ps->media, settings);
	obs_data_release(settings);

	if (*url)
		blog(LOG_INFO, "[projeto-live] '%s' recebendo a imagem de %s", obs_source_get_name(ps->source), url);
}

static void refresh_url(struct phone_source *ps)
{
	char url[URL_MAX];
	build_url(ps, url, sizeof(url));

	/* Se o celular sumiu da lista por um instante, mantém o último endereço:
	 * a Fonte de mídia tenta reconectar sozinha. */
	if (*url)
		apply_url(ps, url);
}

/* Roda na thread da interface, chamada a partir do video_tick. */
static void refresh_task(void *param)
{
	obs_weak_source_t *weak = param;
	obs_source_t *source = obs_weak_source_get_source(weak);
	obs_weak_source_release(weak);
	if (!source)
		return;

	struct phone_source *ps = obs_obj_get_data(source);
	if (ps) {
		refresh_url(ps);
		os_atomic_set_bool(&ps->refresh_pending, false);
	}
	obs_source_release(source);
}

static void phone_update(void *data, obs_data_t *settings)
{
	struct phone_source *ps = data;

	bfree(ps->device_name);
	bfree(ps->manual_address);
	ps->device_name = bstrdup(obs_data_get_string(settings, S_DEVICE));
	ps->manual_address = bstrdup(obs_data_get_string(settings, S_MANUAL));

	char url[URL_MAX];
	build_url(ps, url, sizeof(url));

	/* Celular escolhido mas ainda não encontrado na rede: espera a descoberta (o video_tick
	 * confere de novo a cada poucos segundos). Só limpa a imagem se nada estiver escolhido. */
	bool nothing_chosen = !*ps->device_name && !*ps->manual_address;
	if (*url || nothing_chosen)
		apply_url(ps, url);
}

static void *phone_create(obs_data_t *settings, obs_source_t *source)
{
	struct phone_source *ps = bzalloc(sizeof(struct phone_source));
	ps->source = source;

	obs_data_t *media_settings = obs_data_create();
	obs_data_set_bool(media_settings, "is_local_file", false);
	obs_data_set_string(media_settings, "input", "");
	obs_data_set_bool(media_settings, "log_changes", false);
	ps->media = obs_source_create_private("ffmpeg_source", "Projeto Live: imagem do celular", media_settings);
	obs_data_release(media_settings);

	if (ps->media)
		obs_source_add_active_child(source, ps->media);
	else
		blog(LOG_WARNING, "[projeto-live] Não consegui criar a Fonte de mídia interna");

	phone_update(ps, settings);
	return ps;
}

static void phone_destroy(void *data)
{
	struct phone_source *ps = data;

	if (ps->media) {
		obs_source_remove_active_child(ps->source, ps->media);
		obs_source_release(ps->media);
	}
	bfree(ps->device_name);
	bfree(ps->manual_address);
	bfree(ps);
}

static void phone_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_DEVICE, "");
	obs_data_set_default_string(settings, S_MANUAL, "");
}

static bool refresh_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	UNUSED_PARAMETER(data);
	/* Devolver true faz o OBS montar a janela de novo, com a lista atualizada. */
	return true;
}

static obs_properties_t *phone_properties(void *data)
{
	struct phone_source *ps = data;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(props, S_DEVICE, obs_module_text("Phone"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, obs_module_text("Phone.None"), "");

	struct pl_phone phones[PL_MAX_PHONES];
	size_t count = pl_discovery_get(phones, PL_MAX_PHONES);
	bool selected_found = false;
	char label[PL_NAME_MAX + PL_ADDR_MAX + 64];

	for (size_t i = 0; i < count; i++) {
		snprintf(label, sizeof(label), "%s (%s)", phones[i].name, phones[i].address);
		obs_property_list_add_string(list, label, phones[i].name);
		if (ps && ps->device_name && strcmp(ps->device_name, phones[i].name) == 0)
			selected_found = true;
	}

	if (ps && ps->device_name && *ps->device_name && !selected_found) {
		snprintf(label, sizeof(label), "%s (%s)", ps->device_name, obs_module_text("Phone.NotFound"));
		obs_property_list_add_string(list, label, ps->device_name);
	}

	obs_properties_add_button2(props, S_REFRESH, obs_module_text("Refresh"), refresh_clicked, ps);

	obs_property_t *manual =
		obs_properties_add_text(props, S_MANUAL, obs_module_text("ManualAddress"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(manual, obs_module_text("ManualAddress.ToolTip"));

	obs_properties_add_text(props, S_HELP, obs_module_text("Help"), OBS_TEXT_INFO);

	return props;
}

static void phone_tick(void *data, float seconds)
{
	struct phone_source *ps = data;

	ps->refresh_timer += seconds;
	if (ps->refresh_timer < REFRESH_INTERVAL_SEC)
		return;
	ps->refresh_timer = 0.0f;

	/* Confere de vez em quando se o celular mudou de endereço. A troca em si é feita
	 * na thread da interface, nunca aqui na thread de vídeo. */
	if (os_atomic_exchange_bool(&ps->refresh_pending, true))
		return;

	obs_weak_source_t *weak = obs_source_get_weak_source(ps->source);
	obs_queue_task(OBS_TASK_UI, refresh_task, weak, false);
}

static uint32_t phone_width(void *data)
{
	struct phone_source *ps = data;
	uint32_t width = ps->media ? obs_source_get_width(ps->media) : 0;
	return width ? width : 1920;
}

static uint32_t phone_height(void *data)
{
	struct phone_source *ps = data;
	uint32_t height = ps->media ? obs_source_get_height(ps->media) : 0;
	return height ? height : 1080;
}

static void phone_render(void *data, gs_effect_t *effect)
{
	struct phone_source *ps = data;
	UNUSED_PARAMETER(effect);

	if (ps->media)
		obs_source_video_render(ps->media);
}

static void phone_enum_sources(void *data, obs_source_enum_proc_t enum_callback, void *param)
{
	struct phone_source *ps = data;
	if (ps->media)
		enum_callback(ps->source, ps->media, param);
}

struct obs_source_info projeto_live_phone_source = {
	.id = "projeto_live_phone_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = phone_get_name,
	.create = phone_create,
	.destroy = phone_destroy,
	.update = phone_update,
	.get_defaults = phone_defaults,
	.get_properties = phone_properties,
	.video_tick = phone_tick,
	.video_render = phone_render,
	.get_width = phone_width,
	.get_height = phone_height,
	.enum_active_sources = phone_enum_sources,
	.enum_all_sources = phone_enum_sources,
	.icon_type = OBS_ICON_TYPE_CAMERA,
};
