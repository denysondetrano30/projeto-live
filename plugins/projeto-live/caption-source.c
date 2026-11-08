/*
 * Projeto Live: fonte "Legenda (projeção)".
 *
 * Mostra o texto do slide que está no telão do app de projeção (veja projection-link.h). A fonte é
 * uma caixa de tamanho fixo (largura x altura, nas propriedades); o texto fica dentro dela, no
 * alinhamento escolhido. Por padrão é só o texto, sem contorno e sem fundo.
 *
 * O texto é desenhado por uma fonte de texto do próprio OBS guardada aqui dentro (GDI+ no Windows,
 * FreeType no Mac e no Linux).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include <obs-module.h>
#include <graphics/vec4.h>
#include <util/dstr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "projection-link.h"

#ifdef _WIN32
#define TEXT_SOURCE_ID "text_gdiplus"
#else
#define TEXT_SOURCE_ID "text_ft2_source"
#endif

#define CAPTION_TEXT_MAX 16384

struct caption {
	obs_source_t *self;
	obs_source_t *text;

	uint32_t width;
	uint32_t height;
	char align[8];
	char valign[8];
	bool uppercase;
	bool sample;
	bool background;
	uint32_t background_color;
	int background_opacity;

	obs_data_t *font;
	uint32_t color;
	bool outline;
	uint32_t outline_color;

	uint64_t seen_version;
	bool dirty;
};

static const char *caption_name(void *unused)
{
	(void)unused;
	return obs_module_text("Caption.Name");
}

/* Maiúsculas, inclusive as letras acentuadas do português (á, ç, õ...). */
static void to_upper(char *text)
{
	unsigned char *p = (unsigned char *)text;
	while (*p) {
		if (*p >= 'a' && *p <= 'z') {
			*p = (unsigned char)(*p - 32);
			p++;
		} else if (*p == 0xC3 && p[1] >= 0xA0 && p[1] <= 0xBE && p[1] != 0xB7) {
			/* Latin-1 em UTF-8: "à".."þ" (C3 A0..BE) viram "À".."Þ" (C3 80..9E). */
			p[1] = (unsigned char)(p[1] - 0x20);
			p += 2;
		} else {
			p++;
		}
	}
}

/* Monta as configurações da fonte de texto de dentro com o texto e a aparência atuais. */
static void update_text(struct caption *caption)
{
	char *text = bmalloc(CAPTION_TEXT_MAX);
	pl_projection_caption(text, CAPTION_TEXT_MAX);
	if (!text[0] && caption->sample) {
		snprintf(text, CAPTION_TEXT_MAX, "%s", obs_module_text("Caption.SampleText"));
		/* "\\n" no arquivo de textos vira quebra de linha. */
		for (char *mark = strstr(text, "\\n"); mark; mark = strstr(mark, "\\n")) {
			*mark = '\n';
			memmove(mark + 1, mark + 2, strlen(mark + 2) + 1);
		}
	}
	if (caption->uppercase) {
		to_upper(text);
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "text", text);
	if (caption->font) {
		obs_data_set_obj(settings, "font", caption->font);
	}
#ifdef _WIN32
	obs_data_set_int(settings, "color", caption->color & 0xFFFFFF);
	obs_data_set_int(settings, "opacity", 100);
	obs_data_set_string(settings, "align", caption->align);
	obs_data_set_string(settings, "valign", caption->valign);
	obs_data_set_bool(settings, "extents", true);
	obs_data_set_int(settings, "extents_cx", caption->width);
	obs_data_set_int(settings, "extents_cy", caption->height);
	obs_data_set_bool(settings, "extents_wrap", true);
	obs_data_set_bool(settings, "outline", caption->outline);
	obs_data_set_int(settings, "outline_size", 4);
	obs_data_set_int(settings, "outline_color", caption->outline_color & 0xFFFFFF);
	obs_data_set_int(settings, "outline_opacity", 100);
#else
	obs_data_set_int(settings, "color1", caption->color);
	obs_data_set_int(settings, "color2", caption->color);
	obs_data_set_bool(settings, "outline", caption->outline);
	obs_data_set_bool(settings, "word_wrap", true);
	obs_data_set_int(settings, "custom_width", caption->width);
	obs_data_set_bool(settings, "drop_shadow", false);
#endif
	obs_source_update(caption->text, settings);
	obs_data_release(settings);
	bfree(text);
}

static void caption_update(void *data, obs_data_t *settings)
{
	struct caption *caption = data;
	caption->width = (uint32_t)obs_data_get_int(settings, "width");
	caption->height = (uint32_t)obs_data_get_int(settings, "height");
	if (caption->width < 16) {
		caption->width = 16;
	}
	if (caption->height < 16) {
		caption->height = 16;
	}
	snprintf(caption->align, sizeof(caption->align), "%s", obs_data_get_string(settings, "align"));
	snprintf(caption->valign, sizeof(caption->valign), "%s", obs_data_get_string(settings, "valign"));
	caption->uppercase = obs_data_get_bool(settings, "uppercase");
	caption->sample = obs_data_get_bool(settings, "sample");
	caption->background = obs_data_get_bool(settings, "background");
	caption->background_color = (uint32_t)obs_data_get_int(settings, "background_color");
	caption->background_opacity = (int)obs_data_get_int(settings, "background_opacity");
	caption->color = (uint32_t)obs_data_get_int(settings, "color");
	caption->outline = obs_data_get_bool(settings, "outline");
	caption->outline_color = (uint32_t)obs_data_get_int(settings, "outline_color");
	obs_data_release(caption->font);
	caption->font = obs_data_get_obj(settings, "font");
	caption->dirty = true;
}

static void *caption_create(obs_data_t *settings, obs_source_t *source)
{
	struct caption *caption = bzalloc(sizeof(*caption));
	caption->self = source;
	caption->text = obs_source_create_private(TEXT_SOURCE_ID, "Projeto Live: legenda (texto)", NULL);
	caption_update(caption, settings);
	return caption;
}

static void caption_destroy(void *data)
{
	struct caption *caption = data;
	obs_source_release(caption->text);
	obs_data_release(caption->font);
	bfree(caption);
}

static void caption_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "width", 1600);
	obs_data_set_default_int(settings, "height", 260);
	obs_data_set_default_string(settings, "align", "center");
	obs_data_set_default_string(settings, "valign", "bottom");
	obs_data_set_default_bool(settings, "uppercase", false);
	obs_data_set_default_bool(settings, "sample", false);
	obs_data_set_default_int(settings, "color", 0xFFFFFFFF);
	obs_data_set_default_bool(settings, "outline", false);
	obs_data_set_default_int(settings, "outline_color", 0xFF000000);
	obs_data_set_default_bool(settings, "background", false);
	obs_data_set_default_int(settings, "background_color", 0xFF000000);
	obs_data_set_default_int(settings, "background_opacity", 60);

	obs_data_t *font = obs_data_create();
#ifdef _WIN32
	obs_data_set_default_string(font, "face", "Arial");
#else
	obs_data_set_default_string(font, "face", "Sans Serif");
#endif
	obs_data_set_default_int(font, "size", 64);
	obs_data_set_default_obj(settings, "font", font);
	obs_data_release(font);
}

static obs_properties_t *caption_properties(void *unused)
{
	(void)unused;
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "info", obs_module_text("Caption.Info"), OBS_TEXT_INFO);
	obs_properties_add_font(props, "font", obs_module_text("Caption.Font"));
	obs_properties_add_color(props, "color", obs_module_text("Caption.Color"));
	obs_properties_add_bool(props, "uppercase", obs_module_text("Caption.Uppercase"));

	obs_property_t *align = obs_properties_add_list(props, "align", obs_module_text("Caption.Align"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(align, obs_module_text("Caption.Left"), "left");
	obs_property_list_add_string(align, obs_module_text("Caption.Center"), "center");
	obs_property_list_add_string(align, obs_module_text("Caption.Right"), "right");
	obs_property_t *valign = obs_properties_add_list(props, "valign", obs_module_text("Caption.VAlign"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(valign, obs_module_text("Caption.Top"), "top");
	obs_property_list_add_string(valign, obs_module_text("Caption.Middle"), "center");
	obs_property_list_add_string(valign, obs_module_text("Caption.Bottom"), "bottom");

	obs_properties_add_int(props, "width", obs_module_text("Caption.Width"), 16, 8192, 1);
	obs_properties_add_int(props, "height", obs_module_text("Caption.Height"), 16, 8192, 1);

	obs_properties_add_bool(props, "outline", obs_module_text("Caption.Outline"));
	obs_properties_add_color(props, "outline_color", obs_module_text("Caption.OutlineColor"));
	obs_properties_add_bool(props, "background", obs_module_text("Caption.Background"));
	obs_properties_add_color(props, "background_color", obs_module_text("Caption.BackgroundColor"));
	obs_properties_add_int_slider(props, "background_opacity", obs_module_text("Caption.BackgroundOpacity"), 0, 100,
				      1);
	obs_properties_add_bool(props, "sample", obs_module_text("Caption.Sample"));
	return props;
}

static void caption_tick(void *data, float seconds)
{
	(void)seconds;
	struct caption *caption = data;
	if (!caption->text) {
		return;
	}
	char probe[1];
	uint64_t version = pl_projection_caption(probe, sizeof(probe));
	if (version != caption->seen_version || caption->dirty) {
		caption->seen_version = version;
		caption->dirty = false;
		update_text(caption);
	}
}

static uint32_t caption_width(void *data)
{
	struct caption *caption = data;
	return caption->width;
}

static uint32_t caption_height(void *data)
{
	struct caption *caption = data;
	return caption->height;
}

static void draw_background(struct caption *caption)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *param = gs_effect_get_param_by_name(solid, "color");
	struct vec4 color;
	vec4_from_rgba(&color, caption->background_color);
	color.w = (float)caption->background_opacity / 100.0f;
	gs_effect_set_vec4(param, &color);
	while (gs_effect_loop(solid, "Solid")) {
		gs_draw_sprite(NULL, 0, caption->width, caption->height);
	}
}

static void caption_render(void *data, gs_effect_t *effect)
{
	(void)effect;
	struct caption *caption = data;
	if (caption->background && caption->background_opacity > 0) {
		draw_background(caption);
	}
	if (!caption->text) {
		return;
	}
	uint32_t text_width = obs_source_get_width(caption->text);
	uint32_t text_height = obs_source_get_height(caption->text);
	if (!text_width || !text_height) {
		return;
	}

	/* No Windows a caixa de texto já tem o tamanho da legenda; no Mac/Linux encaixamos aqui. */
	float x = 0.0f;
	float y = 0.0f;
	float free_x = (float)caption->width - (float)text_width;
	float free_y = (float)caption->height - (float)text_height;
	if (strcmp(caption->align, "center") == 0) {
		x = free_x / 2.0f;
	} else if (strcmp(caption->align, "right") == 0) {
		x = free_x;
	}
	if (strcmp(caption->valign, "center") == 0) {
		y = free_y / 2.0f;
	} else if (strcmp(caption->valign, "bottom") == 0) {
		y = free_y;
	}

	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	obs_source_video_render(caption->text);
	gs_matrix_pop();
}

static void caption_enum_sources(void *data, obs_source_enum_proc_t enum_callback, void *param)
{
	struct caption *caption = data;
	if (caption->text) {
		enum_callback(caption->self, caption->text, param);
	}
}

struct obs_source_info pl_caption_source = {
	.id = "projeto_live_caption",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = caption_name,
	.create = caption_create,
	.destroy = caption_destroy,
	.update = caption_update,
	.get_defaults = caption_defaults,
	.get_properties = caption_properties,
	.video_tick = caption_tick,
	.video_render = caption_render,
	.get_width = caption_width,
	.get_height = caption_height,
	.enum_active_sources = caption_enum_sources,
	.icon_type = OBS_ICON_TYPE_TEXT,
};
