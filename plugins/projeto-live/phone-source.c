/*
 * Projeto Live: fonte "Celular".
 *
 * Cada fonte "Celular" tem o seu pareamento: uma porta e uma senha próprias. Enquanto nenhum
 * celular estiver mandando imagem, a fonte mostra um QR code com o endereço deste computador,
 * a porta e a senha. O app Projeto Live lê o QR code e passa a ligar sempre para esta fonte.
 *
 * Por dentro, a imagem chega por uma Fonte de mídia do OBS (ffmpeg_source) escutando em SRT,
 * com senha, sem buffer e com decodificação por hardware.
 *
 * Plano B: com um endereço digitado, a fonte é quem liga para o celular (o app sem pareamento
 * fica esperando na porta 9000).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <obs-module.h>
#include <graphics/vec4.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include "net-info.h"
#include "qrcodegen.h"
#include "usb-relay.h"

#define PHONE_SOURCE_ID "projeto_live_phone_source"

#define S_PORT "pair_port"
#define S_KEY "pair_key"
#define S_MANUAL "manual_address"
#define S_HOW "how_to_pair"
#define S_ADDRESSES "this_computer"
#define S_NEW_KEY "new_qr_code"
#define S_CONNECTION "connection"
#define S_USB_HELP "usb_help"
#define MODE_WIFI "wifi"
#define MODE_USB "usb"

#define PHONE_LISTEN_PORT 9000
#define PORT_MIN 20000
#define PORT_RANGE 10000
#define KEY_LEN 16
#define URL_MAX 512
#define PAYLOAD_MAX 512
#define REFRESH_INTERVAL_SEC 1.0f
#define ADDRESS_CHECK_NS (5ULL * 1000000000ULL)
#define TYPING_PAUSE_NS (1500ULL * 1000000ULL)
#define QR_MAX_VERSION 15
#define QR_BORDER 4
#define QR_MODULE_PX 20
#define CANVAS_W 1920
#define CANVAS_H 1080

struct phone_source {
	obs_source_t *source;
	obs_source_t *media;

	int port;
	char key[KEY_LEN + 1];
	bool usb_mode;
	struct pl_usb_relay *usb;
	char *manual_address;
	uint64_t manual_changed_ns;
	char current_url[URL_MAX];
	char addresses_text[256];
	uint64_t addresses_checked_ns;

	/* O QR code é montado na thread da interface e enviado à placa de vídeo na thread de vídeo. */
	pthread_mutex_t qr_mutex;
	char qr_payload[PAYLOAD_MAX];
	uint8_t *qr_pixels;
	uint32_t qr_px;
	bool qr_dirty;
	gs_texture_t *qr_texture;

	float refresh_timer;
	volatile bool refresh_pending;
};

/* ------------------------------------------------------------------------- */
/* Porta e senha do pareamento                                               */

static uint64_t random_state;

static uint64_t next_random(void)
{
	if (!random_state)
		random_state = os_gettime_ns() ^ (uint64_t)(uintptr_t)&random_state ^ 0x9E3779B97F4A7C15ULL;
	random_state ^= random_state << 13;
	random_state ^= random_state >> 7;
	random_state ^= random_state << 17;
	return random_state;
}

static void make_key(char *key)
{
	/* Sem letras que se confundem (0/O, 1/l/I). */
	static const char chars[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
	for (size_t i = 0; i < KEY_LEN; i++)
		key[i] = chars[next_random() % (sizeof(chars) - 1)];
	key[KEY_LEN] = 0;
}

struct port_check {
	obs_source_t *self;
	int port;
	bool used;
};

static bool port_check_proc(void *param, obs_source_t *source)
{
	struct port_check *check = param;
	if (source == check->self || strcmp(obs_source_get_id(source), PHONE_SOURCE_ID) != 0)
		return true;

	obs_data_t *settings = obs_source_get_settings(source);
	if ((int)obs_data_get_int(settings, S_PORT) == check->port)
		check->used = true;
	obs_data_release(settings);
	return !check->used;
}

/* Sorteia uma porta que nenhuma outra fonte "Celular" esteja usando. */
static int pick_port(obs_source_t *self)
{
	for (int tries = 0; tries < 50; tries++) {
		struct port_check check = {self, PORT_MIN + (int)(next_random() % PORT_RANGE), false};
		obs_enum_sources(port_check_proc, &check);
		if (!check.used)
			return check.port;
	}
	return PORT_MIN;
}

/* ------------------------------------------------------------------------- */
/* QR code                                                                   */

static void url_encode(const char *text, char *out, size_t size)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t pos = 0;
	for (const unsigned char *p = (const unsigned char *)text; *p && pos + 4 < size; p++) {
		unsigned char c = *p;
		bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
			     c == '_' || c == '.' || c == '~';
		if (plain) {
			out[pos++] = (char)c;
		} else {
			out[pos++] = '%';
			out[pos++] = hex[c >> 4];
			out[pos++] = hex[c & 15];
		}
	}
	out[pos] = 0;
}

/* Monta o conteúdo do QR code e, se mudou, desenha os pixels. Roda na thread da interface. */
static void rebuild_qr(struct phone_source *ps)
{
	char addresses[PL_MAX_ADDRESSES][PL_ADDR_MAX];
	size_t count = pl_local_ipv4(addresses, PL_MAX_ADDRESSES);
	ps->addresses_checked_ns = os_gettime_ns();

	struct dstr hosts = {0};
	for (size_t i = 0; i < count; i++) {
		if (i)
			dstr_cat(&hosts, ",");
		dstr_cat(&hosts, addresses[i]);
	}

	if (count)
		snprintf(ps->addresses_text, sizeof(ps->addresses_text), "%s %s · %s %d",
			 obs_module_text("ThisComputer"), addresses[0], obs_module_text("Port"), ps->port);
	else
		snprintf(ps->addresses_text, sizeof(ps->addresses_text), "%s %s", obs_module_text("ThisComputer"),
			 obs_module_text("NoNetwork"));

	char name[256];
	url_encode(obs_source_get_name(ps->source), name, sizeof(name));

	char payload[PAYLOAD_MAX];
	snprintf(payload, sizeof(payload), "projetolive://pair?v=1&p=%d&k=%s&h=%s&n=%s", ps->port, ps->key,
		 hosts.array ? hosts.array : "", name);
	dstr_free(&hosts);

	pthread_mutex_lock(&ps->qr_mutex);
	bool changed = strcmp(payload, ps->qr_payload) != 0;
	pthread_mutex_unlock(&ps->qr_mutex);
	if (!changed)
		return;

	uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
	uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
	if (!qrcodegen_encodeText(payload, temp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, QR_MAX_VERSION,
				  qrcodegen_Mask_AUTO, true)) {
		blog(LOG_WARNING, "[projeto-live] Não consegui montar o QR code de '%s'",
		     obs_source_get_name(ps->source));
		return;
	}

	int modules = qrcodegen_getSize(qr);
	uint32_t px = (uint32_t)(modules + 2 * QR_BORDER) * QR_MODULE_PX;
	uint8_t *pixels = bmalloc((size_t)px * px * 4);
	for (uint32_t y = 0; y < px; y++) {
		for (uint32_t x = 0; x < px; x++) {
			int mx = (int)(x / QR_MODULE_PX) - QR_BORDER;
			int my = (int)(y / QR_MODULE_PX) - QR_BORDER;
			uint8_t value = qrcodegen_getModule(qr, mx, my) ? 0 : 255;
			uint8_t *pixel = pixels + ((size_t)y * px + x) * 4;
			pixel[0] = value;
			pixel[1] = value;
			pixel[2] = value;
			pixel[3] = 255;
		}
	}

	pthread_mutex_lock(&ps->qr_mutex);
	snprintf(ps->qr_payload, sizeof(ps->qr_payload), "%s", payload);
	bfree(ps->qr_pixels);
	ps->qr_pixels = pixels;
	ps->qr_px = px;
	ps->qr_dirty = true;
	pthread_mutex_unlock(&ps->qr_mutex);

	blog(LOG_INFO, "[projeto-live] '%s' pronto para parear: %s, porta %d", obs_source_get_name(ps->source),
	     count ? addresses[0] : "sem rede", ps->port);
}

/* ------------------------------------------------------------------------- */
/* Conexão                                                                   */

/* Endereço digitado só vale quando estiver completo, como 192.168.0.15 ou 192.168.0.15:9000. */
static bool manual_address_complete(const char *text)
{
	const char *p = text;
	if (strncmp(p, "srt://", 6) == 0)
		p += 6;

	unsigned int a, b, c, d;
	int used = 0;
	if (sscanf(p, "%3u.%3u.%3u.%3u%n", &a, &b, &c, &d, &used) != 4)
		return false;
	if (a > 255 || b > 255 || c > 255 || d > 255)
		return false;

	p += used;
	if (*p == 0)
		return true;
	if (*p != ':')
		return false;

	unsigned int port;
	used = 0;
	if (sscanf(p + 1, "%5u%n", &port, &used) != 1 || port == 0 || port > 65535)
		return false;
	return p[1 + used] == 0;
}

/* O endereço digitado (plano B) só vale no modo Wi-Fi. */
static bool has_manual_address(const struct phone_source *ps)
{
	return !ps->usb_mode && ps->manual_address && *ps->manual_address;
}

static void build_url(struct phone_source *ps, char *url, size_t size)
{
	url[0] = 0;

	if (ps->usb_mode) {
		/* Cabo USB: a Fonte de mídia lê do relé local, que busca o vídeo no iPhone pelo cabo. */
		if (ps->usb)
			snprintf(url, size, "tcp://127.0.0.1:%d", pl_usb_relay_local_port(ps->usb));
		return;
	}

	if (has_manual_address(ps)) {
		const char *manual = ps->manual_address;
		if (!manual_address_complete(manual))
			return;
		if (strncmp(manual, "srt://", 6) == 0)
			snprintf(url, size, "%s", manual);
		else if (strchr(manual, ':'))
			snprintf(url, size, "srt://%s", manual);
		else
			snprintf(url, size, "srt://%s:%d", manual, PHONE_LISTEN_PORT);
		return;
	}

	/* Pareado: esta fonte escuta na sua porta e só aceita quem tiver a senha do QR code. */
	snprintf(url, size, "srt://0.0.0.0:%d?mode=listener&passphrase=%s&pbkeylen=16&timeout=5000000", ps->port,
		 ps->key);
}

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
	obs_data_set_int(settings, "reconnect_delay_sec", 1);
	obs_data_set_bool(settings, "restart_on_activate", false);
	obs_data_set_bool(settings, "close_when_inactive", false);
	obs_data_set_bool(settings, "clear_on_media_end", true);
	obs_data_set_bool(settings, "log_changes", false);
	obs_source_update(ps->media, settings);
	obs_data_release(settings);

	if (ps->usb_mode)
		blog(LOG_INFO, "[projeto-live] '%s' esperando o iPhone pelo cabo USB", obs_source_get_name(ps->source));
	else if (has_manual_address(ps))
		blog(LOG_INFO, "[projeto-live] '%s' ligando para o celular em %s", obs_source_get_name(ps->source),
		     url);
	else
		blog(LOG_INFO, "[projeto-live] '%s' esperando o celular pareado na porta %d",
		     obs_source_get_name(ps->source), ps->port);
}

/* Roda na thread da interface, a cada segundo (pedido pelo video_tick). */
static void refresh(struct phone_source *ps)
{
	if (os_gettime_ns() - ps->addresses_checked_ns >= ADDRESS_CHECK_NS)
		rebuild_qr(ps);

	/* Espera a pessoa parar de digitar o endereço antes de conectar. */
	if (os_gettime_ns() - ps->manual_changed_ns < TYPING_PAUSE_NS)
		return;

	char url[URL_MAX];
	build_url(ps, url, sizeof(url));
	if (*url)
		apply_url(ps, url);
}

static void refresh_task(void *param)
{
	obs_weak_source_t *weak = param;
	obs_source_t *source = obs_weak_source_get_source(weak);
	obs_weak_source_release(weak);
	if (!source)
		return;

	struct phone_source *ps = obs_obj_get_data(source);
	if (ps) {
		refresh(ps);
		os_atomic_set_bool(&ps->refresh_pending, false);
	}
	obs_source_release(source);
}

/* ------------------------------------------------------------------------- */
/* Fonte do OBS                                                              */

static const char *phone_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("PhoneSource");
}

static void phone_update(void *data, obs_data_t *settings)
{
	struct phone_source *ps = data;

	int port = (int)obs_data_get_int(settings, S_PORT);
	if (port <= 0 || port > 65535) {
		port = pick_port(ps->source);
		obs_data_set_int(settings, S_PORT, port);
	}
	const char *key = obs_data_get_string(settings, S_KEY);
	if (strlen(key) < 10) {
		char new_key[KEY_LEN + 1];
		make_key(new_key);
		obs_data_set_string(settings, S_KEY, new_key);
		key = obs_data_get_string(settings, S_KEY);
	}
	ps->port = port;
	snprintf(ps->key, sizeof(ps->key), "%s", key);

	bool usb_mode = strcmp(obs_data_get_string(settings, S_CONNECTION), MODE_USB) == 0;
	bool mode_changed = usb_mode != ps->usb_mode;
	ps->usb_mode = usb_mode;
	struct pl_usb_relay *old_relay = NULL;
	if (usb_mode && !ps->usb) {
		ps->usb = pl_usb_relay_create(obs_source_get_name(ps->source));
		if (!ps->usb)
			blog(LOG_WARNING, "[projeto-live] Não consegui preparar o cabo USB");
	} else if (!usb_mode && ps->usb) {
		/* Fecha o relé só depois de a Fonte de mídia mudar de endereço (logo abaixo). */
		old_relay = ps->usb;
		ps->usb = NULL;
	}

	const char *new_manual = obs_data_get_string(settings, S_MANUAL);
	bool manual_changed = !ps->manual_address || strcmp(ps->manual_address, new_manual) != 0;
	bfree(ps->manual_address);
	ps->manual_address = bstrdup(new_manual);

	rebuild_qr(ps);

	/* Endereço digitado: quem conecta é o refresh, quando a digitação parar. */
	if (manual_changed && !mode_changed && has_manual_address(ps)) {
		ps->manual_changed_ns = os_gettime_ns();
		pl_usb_relay_destroy(old_relay);
		return;
	}

	char url[URL_MAX];
	build_url(ps, url, sizeof(url));
	if (*url)
		apply_url(ps, url);
	pl_usb_relay_destroy(old_relay);
}

static void *phone_create(obs_data_t *settings, obs_source_t *source)
{
	struct phone_source *ps = bzalloc(sizeof(struct phone_source));
	ps->source = source;
	pthread_mutex_init(&ps->qr_mutex, NULL);

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
	pl_usb_relay_destroy(ps->usb);
	if (ps->qr_texture) {
		obs_enter_graphics();
		gs_texture_destroy(ps->qr_texture);
		obs_leave_graphics();
	}
	bfree(ps->qr_pixels);
	bfree(ps->manual_address);
	pthread_mutex_destroy(&ps->qr_mutex);
	bfree(ps);
}

static void phone_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_PORT, 0);
	obs_data_set_default_string(settings, S_KEY, "");
	obs_data_set_default_string(settings, S_MANUAL, "");
	obs_data_set_default_string(settings, S_CONNECTION, MODE_WIFI);
}

/* Mostra só os campos do modo escolhido (Wi-Fi ou cabo USB). */
static bool connection_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	bool usb = strcmp(obs_data_get_string(settings, S_CONNECTION), MODE_USB) == 0;
	const char *wifi_only[] = {S_HOW, S_ADDRESSES, S_NEW_KEY, S_MANUAL};
	for (size_t i = 0; i < sizeof(wifi_only) / sizeof(wifi_only[0]); i++) {
		obs_property_t *p = obs_properties_get(props, wifi_only[i]);
		if (p)
			obs_property_set_visible(p, !usb);
	}
	obs_property_t *usb_help = obs_properties_get(props, S_USB_HELP);
	if (usb_help)
		obs_property_set_visible(usb_help, usb);
	return true;
}

static bool new_key_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	struct phone_source *ps = data;
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	if (!ps)
		return false;

	char key[KEY_LEN + 1];
	make_key(key);
	obs_data_t *settings = obs_source_get_settings(ps->source);
	obs_data_set_string(settings, S_KEY, key);
	obs_source_update(ps->source, settings);
	obs_data_release(settings);
	blog(LOG_INFO, "[projeto-live] '%s' ganhou um QR code novo; o celular precisa parear de novo",
	     obs_source_get_name(ps->source));
	return true;
}

static obs_properties_t *phone_properties(void *data)
{
	struct phone_source *ps = data;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *connection = obs_properties_add_list(props, S_CONNECTION, obs_module_text("Connection"),
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(connection, obs_module_text("Connection.Wifi"), MODE_WIFI);
	obs_property_list_add_string(connection, obs_module_text("Connection.Usb"), MODE_USB);
	obs_property_set_modified_callback(connection, connection_modified);

	obs_properties_add_text(props, S_USB_HELP, obs_module_text("UsbHelp"), OBS_TEXT_INFO);
	obs_properties_add_text(props, S_HOW, obs_module_text("HowToPair"), OBS_TEXT_INFO);
	if (ps)
		obs_properties_add_text(props, S_ADDRESSES, ps->addresses_text, OBS_TEXT_INFO);

	obs_property_t *button =
		obs_properties_add_button2(props, S_NEW_KEY, obs_module_text("NewQrCode"), new_key_clicked, ps);
	obs_property_set_long_description(button, obs_module_text("NewQrCode.ToolTip"));

	obs_property_t *manual =
		obs_properties_add_text(props, S_MANUAL, obs_module_text("ManualAddress"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(manual, obs_module_text("ManualAddress.ToolTip"));

	return props;
}

static void phone_tick(void *data, float seconds)
{
	struct phone_source *ps = data;

	ps->refresh_timer += seconds;
	if (ps->refresh_timer < REFRESH_INTERVAL_SEC)
		return;
	ps->refresh_timer = 0.0f;

	/* As mudanças de conexão e de QR code são feitas na thread da interface, nunca aqui. */
	if (os_atomic_exchange_bool(&ps->refresh_pending, true))
		return;

	obs_weak_source_t *weak = obs_source_get_weak_source(ps->source);
	obs_queue_task(OBS_TASK_UI, refresh_task, weak, false);
}

static bool is_receiving(struct phone_source *ps)
{
	return ps->media && obs_source_get_width(ps->media) > 0 && obs_source_get_height(ps->media) > 0;
}

static uint32_t phone_width(void *data)
{
	struct phone_source *ps = data;
	return is_receiving(ps) ? obs_source_get_width(ps->media) : CANVAS_W;
}

static uint32_t phone_height(void *data)
{
	struct phone_source *ps = data;
	return is_receiving(ps) ? obs_source_get_height(ps->media) : CANVAS_H;
}

static void draw_background(void)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");

	struct vec4 dark;
	vec4_set(&dark, 0.10f, 0.10f, 0.12f, 1.0f);
	gs_effect_set_vec4(color, &dark);

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_draw_sprite(NULL, 0, CANVAS_W, CANVAS_H);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}

/* Tela de espera: fundo escuro e, se a fonte estiver pronta para parear, o QR code no meio. */
static void draw_waiting_screen(struct phone_source *ps)
{
	draw_background();
	if (has_manual_address(ps) || ps->usb_mode)
		return;

	pthread_mutex_lock(&ps->qr_mutex);
	if (ps->qr_dirty) {
		if (ps->qr_texture)
			gs_texture_destroy(ps->qr_texture);
		const uint8_t *pixels = ps->qr_pixels;
		ps->qr_texture = pixels ? gs_texture_create(ps->qr_px, ps->qr_px, GS_RGBA, 1, &pixels, 0) : NULL;
		ps->qr_dirty = false;
	}
	pthread_mutex_unlock(&ps->qr_mutex);

	if (!ps->qr_texture)
		return;

	const float side = CANVAS_H * 0.8f;
	gs_matrix_push();
	gs_matrix_translate3f((CANVAS_W - side) / 2.0f, (CANVAS_H - side) / 2.0f, 0.0f);

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, ps->qr_texture);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(ps->qr_texture, 0, (uint32_t)side, (uint32_t)side);

	gs_matrix_pop();
}

static void phone_render(void *data, gs_effect_t *effect)
{
	struct phone_source *ps = data;
	UNUSED_PARAMETER(effect);

	if (is_receiving(ps))
		obs_source_video_render(ps->media);
	else
		draw_waiting_screen(ps);
}

static void phone_enum_sources(void *data, obs_source_enum_proc_t enum_callback, void *param)
{
	struct phone_source *ps = data;
	if (ps->media)
		enum_callback(ps->source, ps->media, param);
}

struct obs_source_info projeto_live_phone_source = {
	.id = PHONE_SOURCE_ID,
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
