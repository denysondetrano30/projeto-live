/*
 * Projeto Live: painéis da tela vertical.
 *
 * - Programa vertical: fica ao lado do preview do OBS (liga e desliga em Painéis > "Programa
 *   vertical"), com o nome da cena no ar, "Seguir o Programa" quando foi escolhida à mão e
 *   "Soltar", que leva para uma janela solta (fechar a janela traz de volta).
 * - "Fontes verticais": as fontes da versão vertical da cena no ar (é uma live só: o vertical
 *   acompanha o Programa). Um botão, "Trocar fonte...", troca a fonte pela versão em pé dela (vale
 *   em todas as cenas); botão direito: trocar, propriedades, filtros. Fonte cinza = escondida na
 *   cena horizontal.
 * - Nas duas janelas de vídeo do vertical dá para mexer com o mouse, como no preview do OBS: um
 *   clique escolhe a fonte (moldura vermelha em volta da imagem), arrastar move (gruda nas bordas e
 *   no meio), os cantos redimensionam sem deformar e os meios dos lados cortam. Mexe na cena
 *   que está no ar no vertical.
 *
 * O trabalho de verdade fica em vertical.c.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "vertical-dock.h"
#include "source-row.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>
#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#include <QAction>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QBoxLayout>
#include <QCloseEvent>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlatformSurfaceEvent>
#include <QPoint>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <graphics/matrix4.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "vertical-record.h"
#include "vertical.h"

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* ------------------------------------------------------------------------- */
/* A cena vertical em que se mexe é sempre a que está no ar (uma live só)   */

obs_source_t *editingScene()
{
	return pl_vertical_current();
}

/* ------------------------------------------------------------------------- */
/* Fonte escolhida com o mouse (a janela de vídeo desenha na thread de vídeo)  */

std::mutex selectionMutex;
obs_weak_source_t *selectedScene = nullptr;
int64_t selectedId = 0;

void setSelection(obs_source_t *scene, int64_t id)
{
	std::lock_guard<std::mutex> lock(selectionMutex);
	obs_weak_source_release(selectedScene);
	selectedScene = scene ? obs_source_get_weak_source(scene) : nullptr;
	selectedId = scene ? id : 0;
}

/* Item escolhido, se ele estiver na cena `scene` (nova referência ou nullptr). */
obs_sceneitem_t *selectedItemIn(obs_source_t *scene)
{
	int64_t id = 0;
	bool same = false;
	{
		std::lock_guard<std::mutex> lock(selectionMutex);
		obs_source_t *chosen = selectedScene ? obs_weak_source_get_source(selectedScene) : nullptr;
		same = chosen && chosen == scene;
		id = selectedId;
		obs_source_release(chosen);
	}
	obs_scene_t *sceneData = same ? obs_scene_from_source(scene) : nullptr;
	obs_sceneitem_t *item = sceneData ? obs_scene_find_sceneitem_by_id(sceneData, id) : nullptr;
	if (item) {
		obs_sceneitem_addref(item);
	}
	return item;
}

struct Box {
	float left, top, right, bottom;
	float width() const { return right - left; }
	float height() const { return bottom - top; }
};

/* Retângulo do "quadro" do item na tela vertical (com as faixas vazias, se houver). */
Box itemBox(obs_sceneitem_t *item)
{
	matrix4 transform;
	obs_sceneitem_get_box_transform(item, &transform);
	const float unit[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
	Box box = {INFINITY, INFINITY, -INFINITY, -INFINITY};
	for (const auto &corner : unit) {
		vec3 point;
		vec3_set(&point, corner[0], corner[1], 0.0f);
		vec3_transform(&point, &point, &transform);
		box.left = std::min(box.left, point.x);
		box.top = std::min(box.top, point.y);
		box.right = std::max(box.right, point.x);
		box.bottom = std::max(box.bottom, point.y);
	}
	return box;
}

/* Tamanho da imagem da fonte, já sem o que foi cortado. */
void contentSize(obs_sceneitem_t *item, float &width, float &height)
{
	obs_source_t *source = obs_sceneitem_get_source(item);
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	width = (float)obs_source_get_width(source) - (float)(crop.left + crop.right);
	height = (float)obs_source_get_height(source) - (float)(crop.top + crop.bottom);
}

/* Onde a imagem aparece de verdade: no modo "Inteira" o quadro é a tela toda, a imagem não. */
Box visibleBox(obs_sceneitem_t *item)
{
	Box rect = itemBox(item);
	enum obs_bounds_type type = obs_sceneitem_get_bounds_type(item);
	float cw, ch;
	contentSize(item, cw, ch);
	if (type == OBS_BOUNDS_NONE || type == OBS_BOUNDS_STRETCH || cw <= 0.0f || ch <= 0.0f) {
		return rect;
	}

	float rw = rect.width();
	float rh = rect.height();
	float scale;
	switch (type) {
	case OBS_BOUNDS_SCALE_OUTER:
		if (obs_sceneitem_get_bounds_crop(item)) {
			return rect;
		}
		scale = std::max(rw / cw, rh / ch);
		break;
	case OBS_BOUNDS_SCALE_TO_WIDTH:
		scale = rw / cw;
		break;
	case OBS_BOUNDS_SCALE_TO_HEIGHT:
		scale = rh / ch;
		break;
	case OBS_BOUNDS_MAX_ONLY:
		scale = std::min(1.0f, std::min(rw / cw, rh / ch));
		break;
	default:
		scale = std::min(rw / cw, rh / ch);
		break;
	}

	float w = cw * scale;
	float h = ch * scale;
	uint32_t align = obs_sceneitem_get_bounds_alignment(item);
	float x = (align & OBS_ALIGN_LEFT)    ? rect.left
		  : (align & OBS_ALIGN_RIGHT) ? rect.right - w
					      : rect.left + (rw - w) / 2.0f;
	float y = (align & OBS_ALIGN_TOP)      ? rect.top
		  : (align & OBS_ALIGN_BOTTOM) ? rect.bottom - h
					       : rect.top + (rh - h) / 2.0f;
	return {x, y, x + w, y + h};
}

/*
 * Antes de mexer com o mouse, a fonte vira uma fonte "normal" do OBS (sem quadro), sem mudar
 * nada do que aparece: a moldura passa a ser exatamente a imagem.
 */
void normalizeItem(obs_sceneitem_t *item)
{
	if (std::fabs(obs_sceneitem_get_rot(item)) > 0.01f) {
		return;
	}
	enum obs_bounds_type type = obs_sceneitem_get_bounds_type(item);
	Box visible = visibleBox(item);
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	float cw, ch;
	contentSize(item, cw, ch);

	vec2 scale;
	if (type == OBS_BOUNDS_NONE) {
		obs_sceneitem_get_scale(item, &scale);
	} else if (cw <= 0.0f || ch <= 0.0f) {
		return;
	} else if (type == OBS_BOUNDS_STRETCH) {
		vec2_set(&scale, visible.width() / cw, visible.height() / ch);
	} else if (type == OBS_BOUNDS_SCALE_OUTER && obs_sceneitem_get_bounds_crop(item)) {
		/* "Preencher": o pedaço cortado para caber vira um corte de verdade. */
		Box rect = itemBox(item);
		float s = std::max(rect.width() / cw, rect.height() / ch);
		float extraX = std::max(cw - rect.width() / s, 0.0f);
		float extraY = std::max(ch - rect.height() / s, 0.0f);
		uint32_t align = obs_sceneitem_get_bounds_alignment(item);
		float leftShare = (align & OBS_ALIGN_LEFT) ? 0.0f : (align & OBS_ALIGN_RIGHT) ? 1.0f : 0.5f;
		float topShare = (align & OBS_ALIGN_TOP) ? 0.0f : (align & OBS_ALIGN_BOTTOM) ? 1.0f : 0.5f;
		int addLeft = (int)std::lround(extraX * leftShare);
		int addTop = (int)std::lround(extraY * topShare);
		crop.left += addLeft;
		crop.right += (int)std::lround(extraX) - addLeft;
		crop.top += addTop;
		crop.bottom += (int)std::lround(extraY) - addTop;
		vec2_set(&scale, s, s);
	} else {
		float s = visible.width() / cw;
		vec2_set(&scale, s, s);
	}

	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_NONE);
	obs_sceneitem_set_crop(item, &crop);
	obs_sceneitem_set_scale(item, &scale);
	obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
	vec2 pos;
	vec2_set(&pos, visible.left, visible.top);
	obs_sceneitem_set_pos(item, &pos);
	obs_sceneitem_defer_update_end(item);
}

/*
 * Os 8 quadradinhos da moldura: 0 alto-esquerda, 1 alto-direita, 2 baixo-direita, 3 baixo-esquerda
 * (aumentam e diminuem sem deformar) e 4 alto, 5 direita, 6 baixo, 7 esquerda (cortam a imagem).
 */
void handlePoints(const Box &box, float x[8], float y[8])
{
	float midX = (box.left + box.right) / 2.0f;
	float midY = (box.top + box.bottom) / 2.0f;
	const float xs[8] = {box.left, box.right, box.right, box.left, midX, box.right, midX, box.left};
	const float ys[8] = {box.top, box.top, box.bottom, box.bottom, box.top, midY, box.bottom, midY};
	for (int i = 0; i < 8; i++) {
		x[i] = xs[i];
		y[i] = ys[i];
	}
}

/*
 * Medidas em px da fonte escolhida até as bordas da tela, como no preview do OBS: o texto é feito
 * com fontes de texto privadas (uma por lado), criadas e atualizadas na thread de vídeo.
 */
obs_source_t *spacingLabels[4] = {};
int spacingPx[4] = {-1, -1, -1, -1};

obs_source_t *createLabel(int index)
{
	obs_data_t *settings = obs_data_create();
	obs_data_t *font = obs_data_create();
#if defined(_WIN32)
	obs_data_set_string(font, "face", "Arial");
#elif defined(__APPLE__)
	obs_data_set_string(font, "face", "Helvetica");
#else
	obs_data_set_string(font, "face", "Monospace");
#endif
	obs_data_set_int(font, "flags", 1);
	obs_data_set_int(font, "size", 16);
	obs_data_set_obj(settings, "font", font);
	obs_data_set_bool(settings, "outline", true);
#ifdef _WIN32
	obs_data_set_int(settings, "outline_color", 0x000000);
	obs_data_set_int(settings, "outline_size", 3);
	const char *id = "text_gdiplus";
#else
	const char *id = "text_ft2_source";
#endif
	char name[64];
	snprintf(name, sizeof(name), "Projeto Live: medida vertical %d", index);
	obs_source_t *label = obs_source_create_private(id, name, settings);
	obs_data_release(font);
	obs_data_release(settings);
	return label;
}

void releaseLabels()
{
	for (int i = 0; i < 4; i++) {
		obs_source_release(spacingLabels[i]);
		spacingLabels[i] = nullptr;
		spacingPx[i] = -1;
	}
}

/* Uma linha da fonte até a borda da tela, com a medida em px no meio. */
void drawSpacing(int index, bool horizontal, float x1, float y1, float x2, float y2, float pixelsPerUnit)
{
	int px = (int)std::lround(horizontal ? x2 - x1 : y2 - y1);
	if (px <= 0) {
		return;
	}

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	vec4 red;
	vec4_set(&red, 0.9f, 0.2f, 0.2f, 1.0f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &red);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(true);
		gs_vertex2f(x1, y1);
		gs_vertex2f(x2, y2);
		gs_render_stop(GS_LINESTRIP);
	}

	if (!spacingLabels[index]) {
		spacingLabels[index] = createLabel(index);
	}
	obs_source_t *label = spacingLabels[index];
	if (!label) {
		return;
	}
	if (spacingPx[index] != px) {
		obs_data_t *settings = obs_source_get_settings(label);
		char text[32];
		snprintf(text, sizeof(text), "%d px", px);
		obs_data_set_string(settings, "text", text);
		obs_source_update(label, settings);
		obs_data_release(settings);
		spacingPx[index] = px;
	}

	float labelW = (float)obs_source_get_width(label) / pixelsPerUnit;
	float labelH = (float)obs_source_get_height(label) / pixelsPerUnit;
	float margin = 4.0f / pixelsPerUnit;
	float x, y;
	if (horizontal) {
		x = (x1 + x2) / 2.0f - labelW / 2.0f;
		y = y1 - labelH - margin;
	} else {
		x = x1 + margin;
		y = (y1 + y2) / 2.0f - labelH / 2.0f;
	}
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	gs_matrix_scale3f(1.0f / pixelsPerUnit, 1.0f / pixelsPerUnit, 1.0f);
	obs_source_video_render(label);
	gs_matrix_pop();
}

/* Moldura vermelha e quadradinhos da fonte escolhida. */
void drawSelection(obs_canvas_t *canvas, float pixelsPerUnit)
{
	obs_source_t *current = pl_vertical_active_scene(canvas);
	obs_sceneitem_t *item = current ? selectedItemIn(current) : nullptr;
	obs_source_release(current);
	if (!item) {
		return;
	}
	Box box = visibleBox(item);
	obs_sceneitem_release(item);
	float hx[8], hy[8];
	handlePoints(box, hx, hy);

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	vec4 red;
	vec4_set(&red, 0.9f, 0.2f, 0.2f, 1.0f);
	gs_effect_set_vec4(color, &red);
	float half = 5.0f / pixelsPerUnit;

	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(true);
		gs_vertex2f(box.left, box.top);
		gs_vertex2f(box.right, box.top);
		gs_vertex2f(box.right, box.bottom);
		gs_vertex2f(box.left, box.bottom);
		gs_vertex2f(box.left, box.top);
		gs_render_stop(GS_LINESTRIP);

		for (int i = 0; i < 8; i++) {
			gs_render_start(true);
			gs_vertex2f(hx[i] - half, hy[i] - half);
			gs_vertex2f(hx[i] + half, hy[i] - half);
			gs_vertex2f(hx[i] - half, hy[i] + half);
			gs_vertex2f(hx[i] + half, hy[i] + half);
			gs_render_stop(GS_TRISTRIP);
		}
	}

	const float width = PL_VERTICAL_WIDTH;
	const float height = PL_VERTICAL_HEIGHT;
	float midX = (box.left + box.right) / 2.0f;
	float midY = (box.top + box.bottom) / 2.0f;
	drawSpacing(0, false, midX, 0.0f, midX, box.top, pixelsPerUnit);
	drawSpacing(1, false, midX, box.bottom, midX, height, pixelsPerUnit);
	drawSpacing(2, true, 0.0f, midY, box.left, midY, pixelsPerUnit);
	drawSpacing(3, true, box.right, midY, width, midY, pixelsPerUnit);
}

/* Põe a fonte no meio da tela vertical (na horizontal, na vertical ou nos dois), sem mudar o tamanho. */
void centerItem(obs_sceneitem_t *item, bool across, bool down)
{
	normalizeItem(item);
	Box box = visibleBox(item);
	vec2 pos;
	obs_sceneitem_get_pos(item, &pos);
	if (across) {
		pos.x += PL_VERTICAL_WIDTH / 2.0f - (box.left + box.right) / 2.0f;
	}
	if (down) {
		pos.y += PL_VERTICAL_HEIGHT / 2.0f - (box.top + box.bottom) / 2.0f;
	}
	obs_sceneitem_set_pos(item, &pos);
}

/* Fonte cujos filtros foram copiados no menu ("Copiar filtros"). */
obs_weak_source_t *copiedFilters = nullptr;

using ItemAction = std::function<void(obs_sceneitem_t *)>;
using ItemRunner = std::function<void(const ItemAction &)>;

/*
 * O item pode sumir enquanto o menu está aberto (a cena trocou, alguém apagou a fonte). Cada opção
 * procura o item de novo pela cena e pelo número dele antes de mexer.
 */
ItemRunner itemRunner(obs_sceneitem_t *item)
{
	std::shared_ptr<obs_weak_source_t> scene(
		obs_source_get_weak_source(obs_scene_get_source(obs_sceneitem_get_scene(item))),
		obs_weak_source_release);
	int64_t id = obs_sceneitem_get_id(item);
	return [scene, id](const ItemAction &action) {
		obs_source_t *source = obs_weak_source_get_source(scene.get());
		obs_sceneitem_t *found = source ? obs_scene_find_sceneitem_by_id(obs_scene_from_source(source), id)
						: nullptr;
		if (found) {
			action(found);
			pl_vertical_touch();
		}
		obs_source_release(source);
	};
}

QAction *addItemAction(QMenu *menu, const char *key, const ItemRunner &run, ItemAction action)
{
	QAction *entry = menu->addAction(moduleText(key));
	QObject::connect(entry, &QAction::triggered, menu, [run, action]() { run(action); });
	return entry;
}

/* Opção marcável (o valor atual da fonte aparece marcado). */
void addChoice(QMenu *menu, const char *key, bool current, const ItemRunner &run, ItemAction action)
{
	QAction *entry = addItemAction(menu, key, run, std::move(action));
	entry->setCheckable(true);
	entry->setChecked(current);
}

/*
 * As opções do botão direito do OBS que fazem sentido no vertical, mais os enquadramentos do
 * Projeto Live. "Trocar fonte" e "Remover" ficam com o painel Fontes verticais.
 */
void addItemMenu(QMenu &menu, QWidget *parent, obs_sceneitem_t *item)
{
	if (!item) {
		return;
	}
	ItemRunner run = itemRunner(item);
	obs_source_t *source = obs_sceneitem_get_source(item);
	QString name = QString::fromUtf8(obs_source_get_name(source));

	addItemAction(&menu, "Vertical.Menu.Project", run, [](obs_sceneitem_t *found) {
		obs_frontend_open_projector("Source", -1, "", obs_source_get_name(obs_sceneitem_get_source(found)));
	});
	addItemAction(&menu, "Vertical.Menu.Screenshot", run, [](obs_sceneitem_t *found) {
		obs_frontend_take_source_screenshot(obs_sceneitem_get_source(found));
	});
	menu.addSeparator();

	QMenu *scaleMenu = menu.addMenu(moduleText("Vertical.Menu.ScaleFilter"));
	const struct {
		const char *key;
		enum obs_scale_type type;
	} filters[] = {{"Vertical.Menu.Scale.Disable", OBS_SCALE_DISABLE},
		       {"Vertical.Menu.Scale.Point", OBS_SCALE_POINT},
		       {"Vertical.Menu.Scale.Bilinear", OBS_SCALE_BILINEAR},
		       {"Vertical.Menu.Scale.Bicubic", OBS_SCALE_BICUBIC},
		       {"Vertical.Menu.Scale.Lanczos", OBS_SCALE_LANCZOS},
		       {"Vertical.Menu.Scale.Area", OBS_SCALE_AREA}};
	for (const auto &filter : filters) {
		enum obs_scale_type type = filter.type;
		addChoice(scaleMenu, filter.key, obs_sceneitem_get_scale_filter(item) == type, run,
			  [type](obs_sceneitem_t *found) { obs_sceneitem_set_scale_filter(found, type); });
	}

	QMenu *blendMenu = menu.addMenu(moduleText("Vertical.Menu.BlendMode"));
	const struct {
		const char *key;
		enum obs_blending_type type;
	} blends[] = {{"Vertical.Menu.Blend.Normal", OBS_BLEND_NORMAL},
		      {"Vertical.Menu.Blend.Additive", OBS_BLEND_ADDITIVE},
		      {"Vertical.Menu.Blend.Subtract", OBS_BLEND_SUBTRACT},
		      {"Vertical.Menu.Blend.Screen", OBS_BLEND_SCREEN},
		      {"Vertical.Menu.Blend.Multiply", OBS_BLEND_MULTIPLY},
		      {"Vertical.Menu.Blend.Lighten", OBS_BLEND_LIGHTEN},
		      {"Vertical.Menu.Blend.Darken", OBS_BLEND_DARKEN}};
	for (const auto &blend : blends) {
		enum obs_blending_type type = blend.type;
		addChoice(blendMenu, blend.key, obs_sceneitem_get_blending_mode(item) == type, run,
			  [type](obs_sceneitem_t *found) { obs_sceneitem_set_blending_mode(found, type); });
	}

	QMenu *methodMenu = menu.addMenu(moduleText("Vertical.Menu.BlendMethod"));
	enum obs_blending_method method = obs_sceneitem_get_blending_method(item);
	addChoice(methodMenu, "Vertical.Menu.Method.Default", method == OBS_BLEND_METHOD_DEFAULT, run,
		  [](obs_sceneitem_t *found) { obs_sceneitem_set_blending_method(found, OBS_BLEND_METHOD_DEFAULT); });
	addChoice(methodMenu, "Vertical.Menu.Method.SrgbOff", method == OBS_BLEND_METHOD_SRGB_OFF, run,
		  [](obs_sceneitem_t *found) { obs_sceneitem_set_blending_method(found, OBS_BLEND_METHOD_SRGB_OFF); });
	menu.addSeparator();

	QMenu *orderMenu = menu.addMenu(moduleText("Vertical.Menu.Order"));
	const struct {
		const char *key;
		enum obs_order_movement movement;
	} orders[] = {{"Vertical.Menu.Order.Up", OBS_ORDER_MOVE_UP},
		      {"Vertical.Menu.Order.Down", OBS_ORDER_MOVE_DOWN},
		      {"Vertical.Menu.Order.Top", OBS_ORDER_MOVE_TOP},
		      {"Vertical.Menu.Order.Bottom", OBS_ORDER_MOVE_BOTTOM}};
	for (const auto &order : orders) {
		enum obs_order_movement movement = order.movement;
		addItemAction(orderMenu, order.key, run,
			      [movement](obs_sceneitem_t *found) { obs_sceneitem_set_order(found, movement); });
	}

	QMenu *transformMenu = menu.addMenu(moduleText("Vertical.Menu.Transform"));
	addItemAction(transformMenu, "Vertical.Fit.Center", run,
		      [](obs_sceneitem_t *found) { centerItem(found, true, true); });
	addItemAction(transformMenu, "Vertical.Fit.CenterAcross", run,
		      [](obs_sceneitem_t *found) { centerItem(found, true, false); });
	addItemAction(transformMenu, "Vertical.Fit.CenterDown", run,
		      [](obs_sceneitem_t *found) { centerItem(found, false, true); });
	transformMenu->addSeparator();
	addItemAction(transformMenu, "Vertical.Fit.Fill", run,
		      [](obs_sceneitem_t *found) { pl_vertical_fit_item(found, PL_FIT_FILL); });
	addItemAction(transformMenu, "Vertical.Fit.Whole", run,
		      [](obs_sceneitem_t *found) { pl_vertical_fit_item(found, PL_FIT_WHOLE); });
	addItemAction(transformMenu, "Vertical.Fit.Width", run,
		      [](obs_sceneitem_t *found) { pl_vertical_fit_item(found, PL_FIT_WIDTH); });
	transformMenu->addSeparator();
	addItemAction(transformMenu, "Vertical.Fit.Reset", run, [](obs_sceneitem_t *found) {
		obs_sceneitem_crop crop = {0, 0, 0, 0};
		obs_sceneitem_set_crop(found, &crop);
		obs_sceneitem_set_rot(found, 0.0f);
		pl_vertical_fit_item(found, PL_FIT_WHOLE);
	});
	menu.addSeparator();

	addItemAction(&menu, "Vertical.Menu.CopyFilters", run, [](obs_sceneitem_t *found) {
		obs_weak_source_release(copiedFilters);
		copiedFilters = obs_source_get_weak_source(obs_sceneitem_get_source(found));
	});
	QAction *paste = addItemAction(&menu, "Vertical.Menu.PasteFilters", run, [](obs_sceneitem_t *found) {
		obs_source_t *from = obs_weak_source_get_source(copiedFilters);
		obs_source_t *to = obs_sceneitem_get_source(found);
		if (from && from != to) {
			obs_source_copy_filters(to, from);
		}
		obs_source_release(from);
	});
	paste->setEnabled(copiedFilters != nullptr);
	menu.addSeparator();

	addItemAction(&menu, "Vertical.Menu.Rename", run, [parent, name](obs_sceneitem_t *found) {
		bool ok = false;
		QString wanted = QInputDialog::getText(parent, moduleText("Vertical.Menu.Rename"),
						       moduleText("Vertical.Menu.RenameLabel"), QLineEdit::Normal, name,
						       &ok)
					 .trimmed();
		if (!ok || wanted.isEmpty() || wanted == name) {
			return;
		}
		QByteArray bytes = wanted.toUtf8();
		obs_source_t *taken = obs_get_source_by_name(bytes.constData());
		if (taken) {
			obs_source_release(taken);
			QMessageBox::warning(parent, moduleText("Vertical.Menu.Rename"),
					     moduleText("Vertical.Menu.NameTaken"));
			return;
		}
		obs_source_set_name(obs_sceneitem_get_source(found), bytes.constData());
	});
	menu.addSeparator();
	addItemAction(&menu, "Vertical.Sources.Filters", run, [](obs_sceneitem_t *found) {
		obs_frontend_open_source_filters(obs_sceneitem_get_source(found));
	});
	addItemAction(&menu, "Vertical.Sources.Properties", run, [](obs_sceneitem_t *found) {
		obs_frontend_open_source_properties(obs_sceneitem_get_source(found));
	});
}

/* Menu do botão direito no vídeo vertical; mora junto do painel Fontes verticais (mais abaixo). */
void showItemMenu(QWidget *parent, obs_sceneitem_t *item, const QPoint &where);

/* ------------------------------------------------------------------------- */
/* Janela de vídeo da tela vertical                                           */

void drawVertical(void *, uint32_t cx, uint32_t cy)
{
	obs_canvas_t *canvas = pl_vertical_get_canvas();
	if (!canvas) {
		return;
	}

	float base_w = PL_VERTICAL_WIDTH;
	float base_h = PL_VERTICAL_HEIGHT;
	obs_video_info ovi;
	if (obs_canvas_get_video_info(canvas, &ovi) && ovi.output_width && ovi.output_height) {
		base_w = (float)ovi.output_width;
		base_h = (float)ovi.output_height;
	}

	float scale = std::min((float)cx / base_w, (float)cy / base_h);
	int width = (int)(base_w * scale);
	int height = (int)(base_h * scale);
	int x = ((int)cx - width) / 2;
	int y = ((int)cy - height) / 2;

	gs_projection_push();
	gs_viewport_push();
	gs_set_viewport(x, y, width, height);
	gs_ortho(0.0f, base_w, 0.0f, base_h, -100.0f, 100.0f);
	obs_render_canvas_texture(canvas);
	drawSelection(canvas, scale);
	gs_viewport_pop();
	gs_projection_pop();

	obs_canvas_release(canvas);
}

class VerticalDisplay : public QWidget {
public:
	explicit VerticalDisplay(QWidget *parent) : QWidget(parent)
	{
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_StaticContents);
		setAttribute(Qt::WA_NoSystemBackground);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_DontCreateNativeAncestors);
		setAttribute(Qt::WA_NativeWindow);
		setMinimumSize(90, 160);
		setMouseTracking(true);

		if (QWindow *window = windowHandle()) {
			window->installEventFilter(this);
			connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
				if (visible) {
					createDisplay();
					resizeDisplay();
				}
			});
		}
	}

	~VerticalDisplay() override { destroyDisplay(); }

	QPaintEngine *paintEngine() const override { return nullptr; }

protected:
	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::RightButton) {
			showMenu(event);
			return;
		}
		if (event->button() != Qt::LeftButton) {
			return;
		}
		float x, y;
		toCanvas(event->position(), x, y);
		obs_source_t *current = pl_vertical_current();
		obs_scene_t *scene = obs_scene_from_source(current);
		mode = Mode::None;

		/* Primeiro: um quadradinho da fonte já escolhida? */
		obs_sceneitem_t *item = current ? selectedItemIn(current) : nullptr;
		int handle = item ? handleAt(visibleBox(item), x, y) : -1;
		if (item && handle < 0) {
			obs_sceneitem_release(item);
			item = nullptr;
		}
		/* Senão: a fonte mais de cima embaixo do mouse. */
		if (!item && scene) {
			item = itemAt(scene, x, y);
		}

		if (item && !obs_sceneitem_locked(item)) {
			setSelection(current, obs_sceneitem_get_id(item));
			normalizeItem(item);
			startBox = visibleBox(item);
			startMouseX = x;
			startMouseY = y;
			obs_sceneitem_get_pos(item, &startPos);
			obs_sceneitem_get_scale(item, &startScale);
			obs_sceneitem_get_crop(item, &startCrop);
			activeHandle = handle;
			mode = handle < 0 ? Mode::Move : Mode::Handle;
		} else {
			setSelection(item ? current : nullptr, item ? obs_sceneitem_get_id(item) : 0);
		}
		obs_sceneitem_release(item);

		obs_source_release(current);
		pl_vertical_touch();
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		float x, y;
		toCanvas(event->position(), x, y);
		obs_source_t *current = pl_vertical_current();
		obs_sceneitem_t *item = current ? selectedItemIn(current) : nullptr;
		obs_source_release(current);

		if (!item) {
			unsetCursor();
			return;
		}
		if (mode == Mode::Move) {
			moveItem(item, x - startMouseX, y - startMouseY);
		} else if (mode == Mode::Handle) {
			/* Como no OBS: segurando Alt corta; sem Alt aumenta ou diminui sem deformar. */
			if (event->modifiers() & Qt::AltModifier) {
				cropItem(item, x - startMouseX, y - startMouseY);
			} else {
				scaleItem(item, x, y);
			}
		} else {
			int handle = handleAt(visibleBox(item), x, y);
			if (handle == 0 || handle == 2) {
				setCursor(Qt::SizeFDiagCursor);
			} else if (handle == 1 || handle == 3) {
				setCursor(Qt::SizeBDiagCursor);
			} else if (handle == 4 || handle == 6) {
				setCursor(Qt::SizeVerCursor);
			} else if (handle == 5 || handle == 7) {
				setCursor(Qt::SizeHorCursor);
			} else {
				unsetCursor();
			}
		}
		obs_sceneitem_release(item);
	}

	void mouseReleaseEvent(QMouseEvent *) override
	{
		if (mode != Mode::None) {
			mode = Mode::None;
			pl_vertical_touch();
		}
	}

	void paintEvent(QPaintEvent *event) override
	{
		createDisplay();
		QWidget::paintEvent(event);
	}

	void resizeEvent(QResizeEvent *event) override
	{
		QWidget::resizeEvent(event);
		createDisplay();
		resizeDisplay();
	}

	bool eventFilter(QObject *object, QEvent *event) override
	{
		/* A janela nativa vai ser trocada (painel solto ou preso de novo): solta a tela antes. */
		if (event->type() == QEvent::PlatformSurface) {
			auto *surface = static_cast<QPlatformSurfaceEvent *>(event);
			if (surface->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
				destroyDisplay();
			}
		}
		return QWidget::eventFilter(object, event);
	}

private:
	enum class Mode { None, Move, Handle };

	/* Botão direito numa fonte do vertical: escolhe ela e abre o mesmo menu da lista. */
	void showMenu(QMouseEvent *event)
	{
		float x, y;
		toCanvas(event->position(), x, y);
		obs_source_t *current = pl_vertical_current();
		obs_scene_t *scene = obs_scene_from_source(current);
		obs_sceneitem_t *item = scene ? itemAt(scene, x, y) : nullptr;
		if (item) {
			setSelection(current, obs_sceneitem_get_id(item));
			pl_vertical_touch();
			showItemMenu(this, item, QCursor::pos());
			obs_sceneitem_release(item);
		}
		obs_source_release(current);
	}

	/* Quanto da tela vertical cabe em cada pixel da janela, e onde ela começa. */
	void viewMap(float &scale, float &offsetX, float &offsetY) const
	{
		QSize size = pixelSize();
		float baseW = PL_VERTICAL_WIDTH;
		float baseH = PL_VERTICAL_HEIGHT;
		scale = std::min((float)size.width() / baseW, (float)size.height() / baseH);
		if (scale <= 0.0f) {
			scale = 1.0f;
		}
		offsetX = ((float)size.width() - baseW * scale) / 2.0f;
		offsetY = ((float)size.height() - baseH * scale) / 2.0f;
	}

	void toCanvas(const QPointF &point, float &x, float &y) const
	{
		float scale, offsetX, offsetY;
		viewMap(scale, offsetX, offsetY);
		qreal ratio = devicePixelRatioF();
		x = ((float)(point.x() * ratio) - offsetX) / scale;
		y = ((float)(point.y() * ratio) - offsetY) / scale;
	}

	/* Distância (em unidades da tela vertical) que conta como "em cima" de um canto ou borda. */
	float grabDistance() const
	{
		float scale, offsetX, offsetY;
		viewMap(scale, offsetX, offsetY);
		return 9.0f * (float)devicePixelRatioF() / scale;
	}

	int handleAt(const Box &box, float x, float y) const
	{
		float hx[8], hy[8];
		handlePoints(box, hx, hy);
		float grab = grabDistance();
		for (int i = 0; i < 8; i++) {
			if (std::fabs(hx[i] - x) <= grab && std::fabs(hy[i] - y) <= grab) {
				return i;
			}
		}
		return -1;
	}

	/* A fonte visível mais de cima no ponto, contando só onde a imagem aparece (nova referência). */
	obs_sceneitem_t *itemAt(obs_scene_t *scene, float x, float y) const
	{
		struct Hit {
			float x, y;
			obs_sceneitem_t *found;
		} hit = {x, y, nullptr};
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
				auto *h = static_cast<Hit *>(param);
				if (!obs_sceneitem_visible(item)) {
					return true;
				}
				Box box = visibleBox(item);
				if (h->x >= box.left && h->x <= box.right && h->y >= box.top && h->y <= box.bottom) {
					/* A lista vai de baixo para cima: o último que acertar é o de cima. */
					h->found = item;
				}
				return true;
			},
			&hit);
		if (hit.found) {
			obs_sceneitem_addref(hit.found);
		}
		return hit.found;
	}

	/* Gruda um valor no 0, no meio ou no fim da tela, se estiver perto. */
	float snapEdge(float value, float limit) const
	{
		float grab = grabDistance() * 1.5f;
		const float targets[] = {0.0f, limit / 2.0f, limit};
		for (float target : targets) {
			if (std::fabs(value - target) < grab) {
				return target;
			}
		}
		return value;
	}

	/* Com Alt: corta pelos lados do quadradinho puxado (os lados opostos ficam parados). */
	void cropItem(obs_sceneitem_t *item, float dx, float dy)
	{
		obs_source_t *source = obs_sceneitem_get_source(item);
		float fullW = (float)obs_source_get_width(source);
		float fullH = (float)obs_source_get_height(source);
		float sx = std::max(std::fabs(startScale.x), 0.0001f);
		float sy = std::max(std::fabs(startScale.y), 0.0001f);
		const float minimum = 8.0f;
		int h = activeHandle;
		bool left = h == 0 || h == 3 || h == 7;
		bool right = h == 1 || h == 2 || h == 5;
		bool top = h == 0 || h == 1 || h == 4;
		bool bottom = h == 2 || h == 3 || h == 6;

		obs_sceneitem_crop crop = startCrop;
		vec2 pos = startPos;
		if (right) {
			float edge = snapEdge(startBox.right + dx, (float)PL_VERTICAL_WIDTH);
			float cut = (float)startCrop.right - (edge - startBox.right) / sx;
			crop.right = (int)std::lround(std::clamp(cut, 0.0f, fullW - (float)startCrop.left - minimum));
		}
		if (left) {
			float edge = snapEdge(startBox.left + dx, (float)PL_VERTICAL_WIDTH);
			float cut = (float)startCrop.left + (edge - startBox.left) / sx;
			crop.left = (int)std::lround(std::clamp(cut, 0.0f, fullW - (float)startCrop.right - minimum));
			pos.x = startPos.x + (float)(crop.left - startCrop.left) * sx;
		}
		if (bottom) {
			float edge = snapEdge(startBox.bottom + dy, (float)PL_VERTICAL_HEIGHT);
			float cut = (float)startCrop.bottom - (edge - startBox.bottom) / sy;
			crop.bottom = (int)std::lround(std::clamp(cut, 0.0f, fullH - (float)startCrop.top - minimum));
		}
		if (top) {
			float edge = snapEdge(startBox.top + dy, (float)PL_VERTICAL_HEIGHT);
			float cut = (float)startCrop.top + (edge - startBox.top) / sy;
			crop.top = (int)std::lround(std::clamp(cut, 0.0f, fullH - (float)startCrop.bottom - minimum));
			pos.y = startPos.y + (float)(crop.top - startCrop.top) * sy;
		}

		obs_sceneitem_defer_update_begin(item);
		obs_sceneitem_set_crop(item, &crop);
		obs_sceneitem_set_pos(item, &pos);
		obs_sceneitem_defer_update_end(item);
	}

	/* Move, grudando as bordas e o meio da fonte nas bordas e no meio da tela. */
	void moveItem(obs_sceneitem_t *item, float dx, float dy)
	{
		float grab = grabDistance() * 1.5f;
		auto snap = [grab](float start, float size, float delta, float limit) {
			const float targets[] = {0.0f, limit / 2.0f, limit};
			const float edges[] = {0.0f, size / 2.0f, size};
			float best = delta;
			float bestDistance = grab;
			for (float edge : edges) {
				for (float target : targets) {
					float wanted = target - (start + edge);
					if (std::fabs(wanted - delta) < bestDistance) {
						bestDistance = std::fabs(wanted - delta);
						best = wanted;
					}
				}
			}
			return best;
		};
		dx = snap(startBox.left, startBox.width(), dx, (float)PL_VERTICAL_WIDTH);
		dy = snap(startBox.top, startBox.height(), dy, (float)PL_VERTICAL_HEIGHT);

		vec2 pos;
		vec2_set(&pos, startPos.x + dx, startPos.y + dy);
		obs_sceneitem_set_pos(item, &pos);
	}

	/*
	 * Sem Alt: aumenta ou diminui sem deformar. Canto: o canto oposto fica parado. Meio de um lado:
	 * o lado oposto fica parado e a fonte cresce igual para os dois lados no outro sentido.
	 */
	void scaleItem(obs_sceneitem_t *item, float x, float y)
	{
		float w0 = std::max(startBox.width(), 1.0f);
		float h0 = std::max(startBox.height(), 1.0f);
		float minimum = 16.0f / std::min(w0, h0);
		float factor;
		float left, top;
		int h = activeHandle;

		if (h < 4) {
			bool right = h == 1 || h == 2;
			bool bottom = h == 2 || h == 3;
			float anchorX = right ? startBox.left : startBox.right;
			float anchorY = bottom ? startBox.top : startBox.bottom;
			factor = std::max(std::fabs(x - anchorX) / w0, std::fabs(y - anchorY) / h0);
			factor = std::max(factor, minimum);
			left = right ? anchorX : anchorX - w0 * factor;
			top = bottom ? anchorY : anchorY - h0 * factor;
		} else if (h == 5 || h == 7) {
			float edge = snapEdge(x, (float)PL_VERTICAL_WIDTH);
			float width = h == 5 ? edge - startBox.left : startBox.right - edge;
			factor = std::max(width / w0, minimum);
			left = h == 5 ? startBox.left : startBox.right - w0 * factor;
			top = (startBox.top + startBox.bottom) / 2.0f - h0 * factor / 2.0f;
		} else {
			float edge = snapEdge(y, (float)PL_VERTICAL_HEIGHT);
			float height = h == 6 ? edge - startBox.top : startBox.bottom - edge;
			factor = std::max(height / h0, minimum);
			top = h == 6 ? startBox.top : startBox.bottom - h0 * factor;
			left = (startBox.left + startBox.right) / 2.0f - w0 * factor / 2.0f;
		}

		obs_sceneitem_defer_update_begin(item);
		obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
		obs_sceneitem_crop crop = startCrop;
		obs_sceneitem_set_crop(item, &crop);
		vec2 pos;
		vec2_set(&pos, left, top);
		obs_sceneitem_set_pos(item, &pos);
		vec2 scale;
		vec2_set(&scale, startScale.x * factor, startScale.y * factor);
		obs_sceneitem_set_scale(item, &scale);
		obs_sceneitem_defer_update_end(item);
	}

	QSize pixelSize() const
	{
		qreal ratio = devicePixelRatioF();
		return QSize((int)(width() * ratio), (int)(height() * ratio));
	}

	void createDisplay()
	{
		if (display || !windowHandle() || !windowHandle()->isExposed()) {
			return;
		}
		QSize size = pixelSize();
		gs_init_data info = {};
		info.cx = (uint32_t)std::max(size.width(), 1);
		info.cy = (uint32_t)std::max(size.height(), 1);
		info.format = GS_BGRA;
		info.zsformat = GS_ZS_NONE;
		/* Igual ao OBS (OBSQTDisplay): cada sistema entrega a janela de um jeito. */
#ifdef _WIN32
		info.window.hwnd = (void *)windowHandle()->winId();
#elif defined(__APPLE__)
		info.window.view = (id)windowHandle()->winId();
#else
		if (obs_get_nix_platform() != OBS_NIX_PLATFORM_X11_EGL) {
			/* Wayland: a janela de vídeo do vertical ainda não foi feita para lá. */
			return;
		}
		info.window.id = (uint32_t)windowHandle()->winId();
		info.window.display = obs_get_nix_platform_display();
#endif
		display = obs_display_create(&info, 0xFF202020);
		if (display) {
			obs_display_add_draw_callback(display, drawVertical, nullptr);
		}
	}

	void resizeDisplay()
	{
		if (display && isVisible()) {
			QSize size = pixelSize();
			obs_display_resize(display, (uint32_t)std::max(size.width(), 1),
					   (uint32_t)std::max(size.height(), 1));
		}
	}

	void destroyDisplay()
	{
		if (display) {
			obs_display_remove_draw_callback(display, drawVertical, nullptr);
			obs_display_destroy(display);
			display = nullptr;
		}
	}

	obs_display_t *display = nullptr;
	Mode mode = Mode::None;
	Box startBox = {0.0f, 0.0f, 0.0f, 0.0f};
	vec2 startPos = {};
	vec2 startScale = {};
	obs_sceneitem_crop startCrop = {};
	float startMouseX = 0.0f;
	float startMouseY = 0.0f;
	int activeHandle = -1;
};

void placeEmbedded();

/* ------------------------------------------------------------------------- */
/* Painel "Cenas verticais"                                                   */

QStringList allSourceNames()
{
	QStringList names;
	obs_enum_sources(
		[](void *param, obs_source_t *source) -> bool {
			static_cast<QStringList *>(param)->append(QString::fromUtf8(obs_source_get_name(source)));
			return true;
		},
		&names);
	names.sort(Qt::CaseInsensitive);
	return names;
}

/* Uma fonte nova (ex.: o arquivo da contagem em 9:16), do tipo que a pessoa escolher (nova ref.). */
obs_source_t *createSourceDialog(QWidget *parent, const QString &suggested)
{
	QStringList labels;
	QStringList ids;
	const char *id;
	for (size_t i = 0; obs_enum_input_types(i, &id); i++) {
		if (obs_get_source_output_flags(id) & (OBS_SOURCE_DEPRECATED | OBS_SOURCE_CAP_DISABLED)) {
			continue;
		}
		const char *label = obs_source_get_display_name(id);
		labels.append(QString::fromUtf8(label ? label : id));
		ids.append(QString::fromUtf8(id));
	}
	bool ok = false;
	QString type = QInputDialog::getItem(parent, moduleText("Vertical.Swap.New"), moduleText("Vertical.Swap.Type"),
					     labels, 0, false, &ok);
	int index = labels.indexOf(type);
	if (!ok || index < 0) {
		return nullptr;
	}
	QString name = QInputDialog::getText(parent, moduleText("Vertical.Swap.New"), moduleText("Vertical.Swap.Name"),
					     QLineEdit::Normal, suggested.isEmpty() ? type : suggested, &ok)
			       .trimmed();
	if (!ok || name.isEmpty()) {
		return nullptr;
	}
	QString unique = name;
	for (int n = 2; n < 1000; n++) {
		QByteArray probe = unique.toUtf8();
		obs_source_t *other = obs_get_source_by_name(probe.constData());
		bool taken = other != nullptr;
		obs_source_release(other);
		if (!taken) {
			break;
		}
		unique = name + QStringLiteral(" ") + QString::number(n);
	}
	QByteArray typeId = ids[index].toUtf8();
	QByteArray bytes = unique.toUtf8();
	return obs_source_create(typeId.constData(), bytes.constData(), nullptr, nullptr);
}

/* ------------------------------------------------------------------------- */
/* Janela "Trocar fonte"                                                      */

class SwapDialog : public QDialog {
public:
	SwapDialog(QWidget *parent, obs_source_t *original) : QDialog(parent), source(obs_source_get_ref(original))
	{
		setWindowTitle(moduleText("Vertical.Swap.Title"));
		setMinimumWidth(420);
		auto *layout = new QVBoxLayout(this);
		auto *question =
			new QLabel(moduleText("Vertical.Swap.Question")
					   .arg(QString::fromUtf8(obs_source_get_name(original)).toHtmlEscaped()),
				   this);
		question->setTextFormat(Qt::RichText);
		question->setWordWrap(true);
		layout->addWidget(question);

		choice = new QComboBox(this);
		choice->addItem(moduleText("Vertical.Swap.Same"), QStringLiteral("same"));
		choice->addItem(moduleText("Vertical.Swap.Hide"), QStringLiteral("hide"));
		choice->addItem(moduleText("Vertical.Swap.New"), QStringLiteral("new"));
		for (const QString &name : allSourceNames()) {
			if (name != QString::fromUtf8(obs_source_get_name(original))) {
				choice->addItem(name, name);
			}
		}
		layout->addWidget(choice);

		auto *help = new QLabel(moduleText("Vertical.Swap.Help"), this);
		help->setWordWrap(true);
		help->setStyleSheet("color: #8a8a8a;");
		layout->addWidget(help);

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		layout->addWidget(buttons);

		obs_source_t *other = nullptr;
		enum pl_vertical_pair_mode mode = pl_vertical_get_pair(original, &other);
		int index = 0;
		if (mode == PL_PAIR_HIDE) {
			index = 1;
		} else if (mode == PL_PAIR_OTHER && other) {
			index = choice->findData(QString::fromUtf8(obs_source_get_name(other)));
		}
		choice->setCurrentIndex(index < 0 ? 0 : index);
		obs_source_release(other);
	}

	~SwapDialog() override { obs_source_release(source); }

	void apply()
	{
		QString data = choice->currentData().toString();
		if (data == QStringLiteral("same")) {
			pl_vertical_set_pair(source, PL_PAIR_SAME, nullptr);
		} else if (data == QStringLiteral("hide")) {
			pl_vertical_set_pair(source, PL_PAIR_HIDE, nullptr);
		} else if (data == QStringLiteral("new")) {
			obs_source_t *fresh = createSourceDialog(this, QString::fromUtf8(obs_source_get_name(source)) +
									       QStringLiteral(" (vertical)"));
			if (fresh) {
				pl_vertical_set_pair(source, PL_PAIR_OTHER, fresh);
				obs_frontend_open_source_properties(fresh);
			}
			obs_source_release(fresh);
		} else {
			QByteArray name = data.toUtf8();
			obs_source_t *other = obs_get_source_by_name(name.constData());
			pl_vertical_set_pair(source, other ? PL_PAIR_OTHER : PL_PAIR_SAME, other);
			obs_source_release(other);
		}
	}

private:
	obs_source_t *source = nullptr;
	QComboBox *choice = nullptr;
};

/* ------------------------------------------------------------------------- */
/* Painel "Fontes verticais"                                                  */

/* Um item da cena vertical que está sendo mexida, se ele ainda existir (sem referência; usar na hora). */
obs_sceneitem_t *verticalItemById(qlonglong id)
{
	obs_source_t *sceneSource = editingScene();
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_sceneitem_t *item = scene ? obs_scene_find_sceneitem_by_id(scene, id) : nullptr;
	obs_source_release(sceneSource);
	return item;
}

void setVerticalVisible(qlonglong id, bool on)
{
	if (obs_sceneitem_t *item = verticalItemById(id)) {
		obs_sceneitem_set_visible(item, on);
	}
}

void setVerticalLocked(qlonglong id, bool on)
{
	if (obs_sceneitem_t *item = verticalItemById(id)) {
		obs_sceneitem_set_locked(item, on);
	}
}

class SourcesDock : public QWidget {
public:
	explicit SourcesDock(QWidget *parent) : QWidget(parent)
	{
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(4, 4, 4, 4);

		/* Em cima: o nome da cena e só dois ícones, "+" e a engrenagem (o resto fica no botão direito). */
		auto *head = new QHBoxLayout();
		title = new QLabel(this);
		title->setWordWrap(true);
		title->setTextFormat(Qt::RichText);
		head->addWidget(title, 1);
		auto *add =
			plIconButton(this, "icon-plus", ":/res/images/plus.svg", moduleText("Vertical.Sources.Add"));
		connect(add, &QToolButton::clicked, this,
			[this, add]() { addSource(add->mapToGlobal(QPoint(0, add->height()))); });
		head->addWidget(add);
		auto *gear = plIconButton(this, "icon-gear", ":/settings/images/settings/general.svg",
					  moduleText("Vertical.Sources.Properties"));
		connect(gear, &QToolButton::clicked, this, [this]() { openProperties(); });
		head->addWidget(gear);
		layout->addLayout(head);

		list = new QListWidget(this);
		list->setContextMenuPolicy(Qt::CustomContextMenu);
		/* Como na lista de Fontes do OBS: arrastar para cima ou para baixo muda o que fica por cima. */
		list->setDragDropMode(QAbstractItemView::InternalMove);
		list->setDefaultDropAction(Qt::MoveAction);
		layout->addWidget(list, 1);
		connect(list->model(), &QAbstractItemModel::rowsMoved, this, [this]() {
			if (!rebuilding) {
				applyOrder();
			}
		});
		connect(list, &QListWidget::currentRowChanged, this, [this](int row) {
			if (rebuilding || row < 0 || row >= (int)rows.size() || rows[row].missing) {
				return;
			}
			/* Escolher na lista também mostra a moldura vermelha no vídeo. */
			obs_source_t *sceneSource = editingScene();
			setSelection(sceneSource, rows[row].id);
			obs_source_release(sceneSource);
		});
		connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { swap(); });
		connect(list, &QListWidget::customContextMenuRequested, this,
			[this](const QPoint &point) { showMenu(list->mapToGlobal(point)); });
	}

	~SourcesDock() override { clearRows(); }

	void rebuild()
	{
		rebuilding = true;
		int previousRow = list->currentRow();
		list->clear();
		clearRows();

		obs_source_t *sceneSource = editingScene();
		obs_scene_t *scene = obs_scene_from_source(sceneSource);
		if (!scene) {
			title->setText(moduleText("Vertical.Sources.NoScene"));
		} else {
			QString name = QString::fromUtf8(obs_source_get_name(sceneSource));
			name.remove(QStringLiteral(" (vertical)"));
			title->setText(moduleText("Vertical.Sources.Title").arg(name.toHtmlEscaped()));

			/* O OBS lista de baixo para cima; o painel mostra o que fica por cima primeiro. */
			std::vector<obs_sceneitem_t *> items;
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
					static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
					return true;
				},
				&items);
			std::reverse(items.begin(), items.end());
			for (obs_sceneitem_t *item : items) {
				obs_source_t *shown = obs_sceneitem_get_source(item);
				QString text = QString::fromUtf8(obs_source_get_name(shown));
				if (pl_vertical_item_is_extra(item)) {
					text += QStringLiteral("   ") + moduleText("Vertical.Sources.OnlyVertical");
				} else {
					obs_source_t *original = pl_vertical_item_original(item);
					if (original && original != shown) {
						text += QStringLiteral("   ") +
							moduleText("Vertical.Sources.InsteadOf")
								.arg(QString::fromUtf8(obs_source_get_name(original)));
					}
					obs_source_release(original);
				}
				auto *row = new QListWidgetItem();
				qlonglong id = (qlonglong)obs_sceneitem_get_id(item);
				row->setData(Qt::UserRole, QVariant(id));
				list->addItem(row);
				/* Nome, olho e cadeado, como no painel Fontes do OBS. */
				auto onVisible = [id](bool on) {
					setVerticalVisible(id, on);
				};
				auto onLock = [id](bool on) {
					setVerticalLocked(id, on);
				};
				bool shownNow = obs_sceneitem_visible(item);
				bool lockedNow = obs_sceneitem_locked(item);
				auto *widget = new PLSourceRow(list, text, shownNow, lockedNow, onVisible, onLock);
				row->setSizeHint(QSize(0, std::max(widget->sizeHint().height(), 26)));
				list->setItemWidget(row, widget);
				Row entry;
				entry.id = obs_sceneitem_get_id(item);
				rows.push_back(entry);
			}

			/* Fontes da cena horizontal que não estão no vertical: em cinza, para poder voltar. */
			pl_vertical_enum_missing(
				sceneSource,
				[](void *param, obs_source_t *source, bool hidden) {
					auto *self = static_cast<SourcesDock *>(param);
					QString text = QString::fromUtf8(obs_source_get_name(source)) +
						       QStringLiteral("   ") + moduleText("Vertical.Sources.Missing");
					auto *row = new QListWidgetItem(text);
					row->setForeground(QColor(128, 128, 128));
					QFont font = row->font();
					font.setItalic(true);
					row->setFont(font);
					/* Linha cinza não entra na ordem (a fonte não está no vertical). */
					row->setFlags(row->flags() & ~Qt::ItemIsDragEnabled);
					self->list->addItem(row);
					Row entry;
					entry.missing = obs_source_get_weak_source(source);
					entry.hidden = hidden;
					self->rows.push_back(entry);
				},
				this);
		}

		/* A fonte escolhida com o mouse fica marcada na lista. */
		obs_sceneitem_t *selected = sceneSource ? selectedItemIn(sceneSource) : nullptr;
		if (selected) {
			int64_t id = obs_sceneitem_get_id(selected);
			for (size_t i = 0; i < rows.size(); i++) {
				if (!rows[i].missing && rows[i].id == id) {
					previousRow = (int)i;
				}
			}
			obs_sceneitem_release(selected);
		}
		obs_source_release(sceneSource);

		if (previousRow >= list->count()) {
			previousRow = list->count() - 1;
		}
		list->setCurrentRow(previousRow);
		rebuilding = false;
	}

private:
	struct Row {
		int64_t id = 0;
		/* Fonte da cena horizontal que não está no vertical (senão, nullptr). */
		obs_weak_source_t *missing = nullptr;
		bool hidden = false;
	};

	void clearRows()
	{
		for (Row &row : rows) {
			obs_weak_source_release(row.missing);
		}
		rows.clear();
	}

	/* Depois de arrastar: a ordem da lista (de cima para baixo) vira a ordem da cena. */
	void applyOrder()
	{
		obs_source_t *sceneSource = editingScene();
		obs_scene_t *scene = obs_scene_from_source(sceneSource);
		if (scene) {
			/* O OBS guarda de baixo para cima: a última linha da lista é a primeira da cena. */
			std::vector<obs_sceneitem_t *> order;
			for (int i = list->count() - 1; i >= 0; i--) {
				QVariant id = list->item(i)->data(Qt::UserRole);
				obs_sceneitem_t *item =
					id.isValid() ? obs_scene_find_sceneitem_by_id(scene, id.toLongLong()) : nullptr;
				if (item) {
					order.push_back(item);
				}
			}
			if (!order.empty()) {
				obs_scene_reorder_items(scene, order.data(), order.size());
			}
		}
		obs_source_release(sceneSource);
		pl_vertical_touch();
	}

	/* Item da cena no ar na linha `row` (nova referência; nullptr numa linha cinza). */
	obs_sceneitem_t *itemAt(int row)
	{
		if (row < 0 || row >= (int)rows.size() || rows[row].missing) {
			return nullptr;
		}
		obs_source_t *sceneSource = editingScene();
		obs_scene_t *scene = obs_scene_from_source(sceneSource);
		obs_sceneitem_t *item = scene ? obs_scene_find_sceneitem_by_id(scene, rows[row].id) : nullptr;
		if (item) {
			obs_sceneitem_addref(item);
		}
		obs_source_release(sceneSource);
		return item;
	}

	/* A fonte original da linha (nova referência): a da cena horizontal. */
	obs_source_t *originalAt(int row)
	{
		if (row < 0 || row >= (int)rows.size()) {
			return nullptr;
		}
		if (rows[row].missing) {
			return obs_weak_source_get_source(rows[row].missing);
		}
		obs_sceneitem_t *item = itemAt(row);
		obs_source_t *original = item && !pl_vertical_item_is_extra(item) ? pl_vertical_item_original(item)
										  : nullptr;
		obs_sceneitem_release(item);
		return original;
	}

	void swap()
	{
		int row = list->currentRow();
		obs_source_t *original = originalAt(row);
		if (!original) {
			return;
		}
		bool missing = rows[row].missing != nullptr;
		SwapDialog dialog(this, original);
		if (dialog.exec() == QDialog::Accepted) {
			dialog.apply();
			if (missing) {
				/* Linha cinza: a fonte volta para esta cena vertical. */
				obs_source_t *sceneSource = editingScene();
				pl_vertical_restore(sceneSource, original);
				obs_source_release(sceneSource);
			}
		}
		obs_source_release(original);
	}

	/* "+": uma fonte só no vertical (nova ou que já existe). */
	void addSource(const QPoint &where)
	{
		obs_source_t *sceneSource = editingScene();
		if (!sceneSource) {
			return;
		}
		QMenu menu(this);
		QAction *fresh = menu.addAction(moduleText("Vertical.Sources.New"));
		QAction *existing = menu.addAction(moduleText("Vertical.Sources.Existing"));
		QAction *chosen = menu.exec(where);
		obs_source_t *source = nullptr;
		if (chosen == fresh) {
			source = createSourceDialog(this, QString());
			if (source) {
				obs_frontend_open_source_properties(source);
			}
		} else if (chosen == existing) {
			bool ok = false;
			QString name = QInputDialog::getItem(this, moduleText("Vertical.Sources.Existing"),
							     moduleText("Vertical.Sources.Choose"), allSourceNames(), 0,
							     false, &ok);
			if (ok && !name.isEmpty()) {
				QByteArray bytes = name.toUtf8();
				source = obs_get_source_by_name(bytes.constData());
			}
		}
		if (source) {
			obs_sceneitem_t *item = pl_vertical_add_extra_source(sceneSource, source);
			if (item) {
				setSelection(sceneSource, obs_sceneitem_get_id(item));
			}
		}
		obs_source_release(source);
		obs_source_release(sceneSource);
	}

	/* "−": tira a fonte só do vertical (se vier da cena horizontal, ela fica em cinza para voltar). */
	void removeSource()
	{
		obs_sceneitem_t *item = itemAt(list->currentRow());
		if (item) {
			setSelection(nullptr, 0);
			obs_sceneitem_remove(item);
			obs_sceneitem_release(item);
			pl_vertical_touch();
		}
	}

public:
	/* Escolhe na lista a linha do item com este número (false se não estiver na lista). */
	bool selectId(int64_t id)
	{
		for (int row = 0; row < (int)rows.size(); row++) {
			if (!rows[row].missing && rows[row].id == id) {
				list->setCurrentRow(row);
				return true;
			}
		}
		return false;
	}

	/* Botão direito: o menu do OBS para a fonte, mais trocar, adicionar e remover só no vertical. */
	/* A engrenagem: propriedades da fonte escolhida. */
	void openProperties()
	{
		obs_sceneitem_t *item = itemAt(list->currentRow());
		if (item) {
			obs_frontend_open_source_properties(obs_sceneitem_get_source(item));
			obs_sceneitem_release(item);
		}
	}

	void showMenu(const QPoint &where)
	{
		int row = list->currentRow();
		obs_sceneitem_t *item = itemAt(row);
		obs_source_t *original = originalAt(row);

		QMenu menu(this);
		connect(menu.addAction(moduleText("Vertical.Sources.Add")), &QAction::triggered, this,
			[this, where]() { addSource(where); });
		QAction *swapAction = menu.addAction(moduleText("Vertical.Swap.Button"));
		swapAction->setEnabled(original != nullptr);
		connect(swapAction, &QAction::triggered, this, [this]() { swap(); });
		obs_source_release(original);
		if (item) {
			QAction *removeAction = menu.addAction(moduleText("Vertical.Sources.Remove"));
			connect(removeAction, &QAction::triggered, this, [this]() { removeSource(); });
			menu.addSeparator();
			addItemMenu(menu, this, item);
		}
		obs_sceneitem_release(item);
		menu.exec(where);
	}

private:
	QLabel *title = nullptr;
	QListWidget *list = nullptr;
	std::vector<Row> rows;
	bool rebuilding = false;
};

/* ------------------------------------------------------------------------- */
/* Programa vertical: ao lado do preview ou numa janela solta                 */

bool floating = false;

class VerticalProgram : public QWidget {
public:
	/* withDetach: a cópia da janela do Projeto Live não tem "Soltar" (ele mexe no da janela do OBS). */
	VerticalProgram(QWidget *parent, bool isFloating, bool withDetach = true) : QWidget(parent)
	{
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(4, isFloating ? 4 : 0, isFloating ? 4 : 0, isFloating ? 4 : 0);
		layout->setSpacing(2);

		auto *header = new QHBoxLayout();
		header->setSpacing(4);
		status = new QLabel(this);
		status->setTextFormat(Qt::RichText);
		status->setWordWrap(true);
		status->setStyleSheet("color: #a0a0a0;");
		auto *detach = new QPushButton(moduleText(isFloating ? "Vertical.Attach" : "Vertical.Detach"), this);
		connect(detach, &QPushButton::clicked, this, [isFloating]() {
			floating = !isFloating;
			QTimer::singleShot(0, []() { placeEmbedded(); });
		});
		header->addWidget(status, 1);
		header->addWidget(detach);
		detach->setVisible(withDetach);
		layout->addLayout(header);
		layout->addWidget(new VerticalDisplay(this), 1);

		if (!isFloating && parent) {
			parent->installEventFilter(this);
		}
		embeddedMode = !isFloating;
		refresh();
		fitWidth();
	}

	void refresh()
	{
		obs_source_t *current = pl_vertical_current();
		QString name = current ? QString::fromUtf8(obs_source_get_name(current))
				       : moduleText("Vertical.Nothing");
		name.remove(QStringLiteral(" (vertical)"));
		name = name.toHtmlEscaped();
		obs_source_release(current);
		status->setText(moduleText("Vertical.Short.OnAir").arg(name));
	}

	/* Ao lado do preview: largura certa para a tela em pé caber na altura da área. */
	void fitWidth()
	{
		if (!embeddedMode) {
			return;
		}
		QWidget *area = parentWidget();
		int height = (area ? area->height() : 400) - 28;
		setFixedWidth(std::max(140, height * 9 / 16 + 4));
	}

protected:
	bool eventFilter(QObject *object, QEvent *event) override
	{
		if (embeddedMode && object == parentWidget() && event->type() == QEvent::Resize) {
			fitWidth();
		}
		return QWidget::eventFilter(object, event);
	}

private:
	QLabel *status = nullptr;
	bool embeddedMode = true;
};

/* Janela solta: fechar põe o Programa vertical de volta ao lado do preview. */
class FloatingWindow : public QWidget {
public:
	explicit FloatingWindow(QWidget *parent) : QWidget(parent, Qt::Window)
	{
		setWindowTitle(moduleText("Vertical.Embedded.Label"));
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		program = new VerticalProgram(this, true);
		layout->addWidget(program);
		resize(380, 700);
	}

	VerticalProgram *program = nullptr;

protected:
	void closeEvent(QCloseEvent *event) override
	{
		QWidget::closeEvent(event);
		floating = false;
		QTimer::singleShot(0, []() { placeEmbedded(); });
	}
};

VerticalProgram *embedded = nullptr;
FloatingWindow *floatingWindow = nullptr;
QAction *menuAction = nullptr;

QBoxLayout *previewRow()
{
	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	QWidget *area = mainWindow ? mainWindow->findChild<QWidget *>("canvasEditor") : nullptr;
	return area ? qobject_cast<QBoxLayout *>(area->layout()) : nullptr;
}

void removeEmbedded()
{
	if (embedded) {
		if (QBoxLayout *row = previewRow()) {
			row->removeWidget(embedded);
		}
		delete embedded;
		embedded = nullptr;
	}
}

void removeFloating()
{
	if (floatingWindow) {
		FloatingWindow *window = floatingWindow;
		floatingWindow = nullptr;
		window->hide();
		window->deleteLater();
	}
}

/* Mostra o Programa vertical onde a pessoa escolheu (ou some com ele). */
void placeEmbedded()
{
	bool wanted = pl_vertical_pref("embed");
	if (menuAction && menuAction->isChecked() != wanted) {
		QSignalBlocker block(menuAction);
		menuAction->setChecked(wanted);
	}

	if (!wanted) {
		removeEmbedded();
		removeFloating();
		return;
	}

	if (floating) {
		removeEmbedded();
		if (!floatingWindow) {
			floatingWindow = new FloatingWindow(static_cast<QWidget *>(obs_frontend_get_main_window()));
		}
		floatingWindow->show();
		floatingWindow->raise();
		return;
	}

	removeFloating();
	QBoxLayout *row = previewRow();
	if (!row) {
		return;
	}
	if (!embedded) {
		embedded = new VerticalProgram(row->parentWidget(), false);
	} else {
		row->removeWidget(embedded);
	}
	/* Sempre o último da fileira (no modo estúdio o OBS acrescenta o Programa depois). */
	row->addWidget(embedded);
	embedded->show();
}

/* ------------------------------------------------------------------------- */
/* Quem atualiza os painéis                                                   */

SourcesDock *sourcesDock = nullptr;

/* Cópias que a janela do Projeto Live usa (os mesmos painéis, noutra janela). */
std::vector<VerticalProgram *> extraPrograms;
std::vector<SourcesDock *> extraSources;

void showItemMenu(QWidget *parent, obs_sceneitem_t *item, const QPoint &where)
{
	if (sourcesDock && sourcesDock->selectId(obs_sceneitem_get_id(item))) {
		sourcesDock->showMenu(where);
		return;
	}
	/* Painel Fontes verticais ainda não montado: só o menu da fonte. */
	QMenu menu(parent);
	addItemMenu(menu, parent, item);
	menu.exec(where);
}
QTimer *timer = nullptr;
uint64_t seenVersion = (uint64_t)-1;

void refreshDocks()
{
	if (embedded) {
		embedded->refresh();
	}
	if (floatingWindow && floatingWindow->program) {
		floatingWindow->program->refresh();
	}
	for (VerticalProgram *each : extraPrograms) {
		each->refresh();
	}
	uint64_t now = pl_vertical_version();
	if (now == seenVersion) {
		return;
	}
	seenVersion = now;
	if (sourcesDock) {
		sourcesDock->rebuild();
	}
	for (SourcesDock *each : extraSources) {
		each->rebuild();
	}
}

void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING || event == OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED ||
	    event == OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED) {
		QTimer::singleShot(0, []() { placeEmbedded(); });
	}
	if (event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP) {
		setSelection(nullptr, 0);
	}
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		if (timer) {
			timer->stop();
		}
		removeFloating();
		obs_enter_graphics();
		releaseLabels();
		obs_leave_graphics();
	}
}

template<typename Dock> Dock *addDock(QWidget *mainWindow, const char *id, const char *titleKey)
{
	auto *dock = new Dock(mainWindow);
	if (!obs_frontend_add_dock_by_id(id, obs_module_text(titleKey), dock)) {
		delete dock;
		return nullptr;
	}
	return dock;
}

} // namespace

void pl_vertical_dock_load(void)
{
	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!mainWindow) {
		return;
	}

	sourcesDock = addDock<SourcesDock>(mainWindow, "projeto-live-vertical-fontes", "Vertical.Sources.Title.Dock");
	if (sourcesDock) {
		QObject::connect(sourcesDock, &QObject::destroyed, []() { sourcesDock = nullptr; });
	}

	/* Painéis > "Programa vertical": liga e desliga o vertical ao lado do preview. */
	QMenu *docksMenu = mainWindow->findChild<QMenu *>("menuDocks");
	if (docksMenu) {
		menuAction = docksMenu->addAction(moduleText("Vertical.Menu"));
	} else {
		menuAction =
			static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("Vertical.Menu")));
	}
	if (menuAction) {
		menuAction->setCheckable(true);
		menuAction->setChecked(pl_vertical_pref("embed"));
		QObject::connect(menuAction, &QAction::toggled, [](bool on) {
			pl_vertical_set_pref("embed", on);
			placeEmbedded();
		});
	}

	timer = new QTimer(mainWindow);
	QObject::connect(timer, &QTimer::timeout, []() { refreshDocks(); });
	timer->start(300);
	obs_frontend_add_event_callback(frontendEvent, nullptr);
}

QWidget *pl_vertical_program_create(QWidget *parent)
{
	auto *created = new VerticalProgram(parent, false, false);
	extraPrograms.push_back(created);
	QObject::connect(created, &QObject::destroyed, [created]() {
		extraPrograms.erase(std::remove(extraPrograms.begin(), extraPrograms.end(), created),
				    extraPrograms.end());
	});
	return created;
}

QWidget *pl_vertical_sources_create(QWidget *parent)
{
	auto *created = new SourcesDock(parent);
	extraSources.push_back(created);
	QObject::connect(created, &QObject::destroyed, [created]() {
		extraSources.erase(std::remove(extraSources.begin(), extraSources.end(), created), extraSources.end());
	});
	created->rebuild();
	return created;
}

bool pl_vertical_program_wanted(void)
{
	return pl_vertical_pref("embed");
}
