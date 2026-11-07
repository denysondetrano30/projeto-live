/*
 * Projeto Live: painéis da tela vertical.
 *
 * - Programa vertical: fica ao lado do preview do OBS (liga e desliga em Painéis > "Programa
 *   vertical"), com o nome da cena no ar, "Seguir o Programa" quando foi escolhida à mão e
 *   "Soltar", que leva para uma janela solta (fechar a janela traz de volta).
 * - "Cenas verticais": as gêmeas (⇄, na ordem das cenas do OBS) e as cenas só verticais. Um clique
 *   escolhe a cena para mexer nas fontes; dois cliques (ou "Pôr no ar") põem a cena no vertical.
 * - "Fontes verticais": as fontes da cena escolhida, com a caixa de aparecer/sumir, a ordem, os
 *   enquadramentos rápidos (preencher, inteira, largura toda) e "Casar fontes".
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

#include <obs-frontend-api.h>
#include <obs-module.h>
#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#include <QAction>
#include <QBoxLayout>
#include <QCloseEvent>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
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
/* Cena escolhida para mexer (compartilhada pelos painéis de cenas e fontes)   */

obs_weak_source_t *editing = nullptr;

obs_source_t *editingScene()
{
	return editing ? obs_weak_source_get_source(editing) : nullptr;
}

void setEditingScene(obs_source_t *scene)
{
	obs_weak_source_release(editing);
	editing = scene ? obs_source_get_weak_source(scene) : nullptr;
	pl_vertical_touch();
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

int64_t selectedItemId()
{
	std::lock_guard<std::mutex> lock(selectionMutex);
	return selectedId;
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

/* Moldura vermelha e quadradinhos da fonte escolhida. */
void drawSelection(obs_canvas_t *canvas, float pixelsPerUnit)
{
	obs_source_t *current = obs_canvas_get_channel(canvas, 0);
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
}

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
			mode = handle < 0 ? Mode::Move : handle < 4 ? Mode::Resize : Mode::Crop;
		} else {
			setSelection(item ? current : nullptr, item ? obs_sceneitem_get_id(item) : 0);
		}
		obs_sceneitem_release(item);

		obs_source_t *editingNow = editingScene();
		if (current && editingNow != current) {
			setEditingScene(current);
		}
		obs_source_release(editingNow);
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
		} else if (mode == Mode::Resize) {
			resizeItem(item, x, y);
		} else if (mode == Mode::Crop) {
			cropItem(item, x - startMouseX, y - startMouseY);
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
	enum class Mode { None, Move, Resize, Crop };

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

	/* Corta pelo lado puxado (o lado oposto fica parado). */
	void cropItem(obs_sceneitem_t *item, float dx, float dy)
	{
		obs_source_t *source = obs_sceneitem_get_source(item);
		float fullW = (float)obs_source_get_width(source);
		float fullH = (float)obs_source_get_height(source);
		float sx = std::max(std::fabs(startScale.x), 0.0001f);
		float sy = std::max(std::fabs(startScale.y), 0.0001f);
		const float minimum = 8.0f;

		obs_sceneitem_crop crop = startCrop;
		vec2 pos = startPos;
		if (activeHandle == 5) {
			float edge = snapEdge(startBox.right + dx, (float)PL_VERTICAL_WIDTH);
			float cut = (float)startCrop.right - (edge - startBox.right) / sx;
			crop.right = (int)std::lround(std::clamp(cut, 0.0f, fullW - (float)startCrop.left - minimum));
		} else if (activeHandle == 7) {
			float edge = snapEdge(startBox.left + dx, (float)PL_VERTICAL_WIDTH);
			float cut = (float)startCrop.left + (edge - startBox.left) / sx;
			crop.left = (int)std::lround(std::clamp(cut, 0.0f, fullW - (float)startCrop.right - minimum));
			pos.x = startPos.x + (float)(crop.left - startCrop.left) * sx;
		} else if (activeHandle == 6) {
			float edge = snapEdge(startBox.bottom + dy, (float)PL_VERTICAL_HEIGHT);
			float cut = (float)startCrop.bottom - (edge - startBox.bottom) / sy;
			crop.bottom = (int)std::lround(std::clamp(cut, 0.0f, fullH - (float)startCrop.top - minimum));
		} else if (activeHandle == 4) {
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

	/* Redimensiona pelo canto, sem deformar; o canto oposto fica parado. */
	void resizeItem(obs_sceneitem_t *item, float x, float y)
	{
		bool right = activeHandle == 1 || activeHandle == 2;
		bool bottom = activeHandle == 2 || activeHandle == 3;
		float anchorX = right ? startBox.left : startBox.right;
		float anchorY = bottom ? startBox.top : startBox.bottom;
		float w0 = std::max(startBox.width(), 1.0f);
		float h0 = std::max(startBox.height(), 1.0f);

		float factor = std::max(std::fabs(x - anchorX) / w0, std::fabs(y - anchorY) / h0);
		factor = std::max(factor, 16.0f / std::min(w0, h0));
		float w = w0 * factor;
		float h = h0 * factor;
		float left = right ? anchorX : anchorX - w;
		float top = bottom ? anchorY : anchorY - h;

		obs_sceneitem_defer_update_begin(item);
		obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
		vec2 pos;
		vec2_set(&pos, left, top);
		obs_sceneitem_set_pos(item, &pos);
		if (obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE) {
			vec2 bounds;
			vec2_set(&bounds, w, h);
			obs_sceneitem_set_bounds(item, &bounds);
		} else {
			vec2 scale;
			vec2_set(&scale, startScale.x * factor, startScale.y * factor);
			obs_sceneitem_set_scale(item, &scale);
		}
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

struct SceneEntry {
	obs_weak_source_t *weak = nullptr;
	bool twin = false;
};

class ScenesDock : public QWidget {
public:
	explicit ScenesDock(QWidget *parent) : QWidget(parent)
	{
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(4, 4, 4, 4);

		list = new QListWidget(this);
		layout->addWidget(list, 1);
		connect(list, &QListWidget::currentRowChanged, this, [this](int row) {
			if (!rebuilding) {
				chooseRow(row);
			}
		});
		connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { putOnAir(); });

		auto *help = new QLabel(moduleText("Vertical.Scenes.Help"), this);
		help->setWordWrap(true);
		help->setStyleSheet("color: #8a8a8a;");
		layout->addWidget(help);

		auto *row1 = new QHBoxLayout();
		auto *onAir = new QPushButton(moduleText("Vertical.OnAir"), this);
		auto *follow = new QPushButton(moduleText("Vertical.Follow"), this);
		connect(onAir, &QPushButton::clicked, this, [this]() { putOnAir(); });
		connect(follow, &QPushButton::clicked, this, []() { pl_vertical_follow_program(); });
		row1->addWidget(onAir);
		row1->addWidget(follow);
		layout->addLayout(row1);

		auto *row2 = new QHBoxLayout();
		auto *add = new QPushButton(moduleText("Vertical.AddExtra"), this);
		remove = new QPushButton(moduleText("Vertical.RemoveExtra"), this);
		connect(add, &QPushButton::clicked, this, [this]() { addExtra(); });
		connect(remove, &QPushButton::clicked, this, [this]() { removeExtra(); });
		row2->addWidget(add);
		row2->addWidget(remove);
		layout->addLayout(row2);
	}

	void rebuild()
	{
		rebuilding = true;
		obs_source_t *chosen = editingScene();
		obs_source_t *current = pl_vertical_current();

		clearEntries();
		list->clear();
		struct Collect {
			ScenesDock *self;
			obs_source_t *chosen;
			obs_source_t *current;
			int chosenRow;
		} collect = {this, chosen, current, -1};

		pl_vertical_enum_scenes(
			[](void *param, obs_source_t *scene, const char *twinOf) -> bool {
				auto *c = static_cast<Collect *>(param);
				QString text = twinOf ? QString::fromUtf8("⇄ ") + QString::fromUtf8(twinOf)
						      : QString::fromUtf8(obs_source_get_name(scene));
				auto *item = new QListWidgetItem(text);
				if (scene == c->current) {
					QFont font = item->font();
					font.setBold(true);
					item->setFont(font);
					item->setText(text + "  ●");
					item->setForeground(QColor(229, 72, 77));
				}
				c->self->list->addItem(item);
				SceneEntry entry;
				entry.weak = obs_source_get_weak_source(scene);
				entry.twin = twinOf != nullptr;
				c->self->entries.push_back(entry);
				if (scene == c->chosen) {
					c->chosenRow = (int)c->self->entries.size() - 1;
				}
				return true;
			},
			&collect);

		obs_source_release(current);
		obs_source_release(chosen);

		if (collect.chosenRow < 0 && !entries.empty()) {
			collect.chosenRow = 0;
			obs_source_t *first = obs_weak_source_get_source(entries[0].weak);
			obs_weak_source_release(editing);
			editing = first ? obs_source_get_weak_source(first) : nullptr;
			obs_source_release(first);
		}
		list->setCurrentRow(collect.chosenRow);
		updateButtons();
		rebuilding = false;
	}

	~ScenesDock() override { clearEntries(); }

private:
	void clearEntries()
	{
		for (SceneEntry &entry : entries) {
			obs_weak_source_release(entry.weak);
		}
		entries.clear();
	}

	obs_source_t *sceneAt(int row)
	{
		if (row < 0 || row >= (int)entries.size()) {
			return nullptr;
		}
		return obs_weak_source_get_source(entries[row].weak);
	}

	void chooseRow(int row)
	{
		obs_source_t *scene = sceneAt(row);
		setEditingScene(scene);
		obs_source_release(scene);
		updateButtons();
	}

	void updateButtons()
	{
		int row = list->currentRow();
		remove->setEnabled(row >= 0 && row < (int)entries.size() && !entries[row].twin);
	}

	void putOnAir()
	{
		obs_source_t *scene = sceneAt(list->currentRow());
		pl_vertical_select(scene);
		obs_source_release(scene);
	}

	void addExtra()
	{
		bool ok = false;
		QString name = QInputDialog::getText(this, moduleText("Vertical.AddExtra"),
						     moduleText("Vertical.AddExtra.Name"), QLineEdit::Normal,
						     moduleText("Vertical.AddExtra.Default"), &ok);
		if (!ok) {
			return;
		}
		QByteArray bytes = name.trimmed().toUtf8();
		obs_source_t *scene = pl_vertical_add_extra(bytes.constData());
		setEditingScene(scene);
		obs_source_release(scene);
	}

	void removeExtra()
	{
		obs_source_t *scene = sceneAt(list->currentRow());
		if (!scene) {
			return;
		}
		QString question =
			moduleText("Vertical.RemoveExtra.Question").arg(QString::fromUtf8(obs_source_get_name(scene)));
		if (QMessageBox::question(this, moduleText("Vertical.RemoveExtra"), question) == QMessageBox::Yes) {
			setEditingScene(nullptr);
			pl_vertical_remove_extra(scene);
		}
		obs_source_release(scene);
	}

	QListWidget *list = nullptr;
	QPushButton *remove = nullptr;
	std::vector<SceneEntry> entries;
	bool rebuilding = false;
};

/* ------------------------------------------------------------------------- */
/* Janela "Casar fontes"                                                      */

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

class PairDialog : public QDialog {
public:
	PairDialog(QWidget *parent, const QString &start) : QDialog(parent)
	{
		setWindowTitle(moduleText("Vertical.Pair.Title"));
		setMinimumWidth(420);
		auto *layout = new QVBoxLayout(this);

		auto *help = new QLabel(moduleText("Vertical.Pair.Help"), this);
		help->setWordWrap(true);
		layout->addWidget(help);

		QStringList names = allSourceNames();
		auto *form = new QFormLayout();
		source = new QComboBox(this);
		source->addItems(names);
		pair = new QComboBox(this);
		pair->addItem(moduleText("Vertical.Pair.Same"), QString());
		pair->addItem(moduleText("Vertical.Pair.Hide"), QString::fromUtf8("\x01"));
		for (const QString &name : names) {
			pair->addItem(name, name);
		}
		form->addRow(moduleText("Vertical.Pair.Source"), source);
		form->addRow(moduleText("Vertical.Pair.Shows"), pair);
		layout->addLayout(form);

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		layout->addWidget(buttons);

		connect(source, &QComboBox::currentIndexChanged, this, [this](int) { showCurrentPair(); });
		int index = source->findText(start);
		source->setCurrentIndex(index < 0 ? 0 : index);
		showCurrentPair();
	}

	void apply()
	{
		QByteArray name = source->currentText().toUtf8();
		obs_source_t *chosen = obs_get_source_by_name(name.constData());
		if (!chosen) {
			return;
		}
		QString data = pair->currentData().toString();
		if (data.isEmpty()) {
			pl_vertical_set_pair(chosen, PL_PAIR_SAME, nullptr);
		} else if (data == QString::fromUtf8("\x01")) {
			pl_vertical_set_pair(chosen, PL_PAIR_HIDE, nullptr);
		} else {
			QByteArray otherName = data.toUtf8();
			obs_source_t *other = obs_get_source_by_name(otherName.constData());
			if (other && other != chosen) {
				pl_vertical_set_pair(chosen, PL_PAIR_OTHER, other);
			} else {
				pl_vertical_set_pair(chosen, PL_PAIR_SAME, nullptr);
			}
			obs_source_release(other);
		}
		obs_source_release(chosen);
	}

private:
	void showCurrentPair()
	{
		QByteArray name = source->currentText().toUtf8();
		obs_source_t *chosen = obs_get_source_by_name(name.constData());
		obs_source_t *other = nullptr;
		enum pl_vertical_pair_mode mode = pl_vertical_get_pair(chosen, &other);
		int index = 0;
		if (mode == PL_PAIR_HIDE) {
			index = 1;
		} else if (mode == PL_PAIR_OTHER && other) {
			index = pair->findData(QString::fromUtf8(obs_source_get_name(other)));
		}
		pair->setCurrentIndex(index < 0 ? 0 : index);
		obs_source_release(other);
		obs_source_release(chosen);
	}

	QComboBox *source = nullptr;
	QComboBox *pair = nullptr;
};

/* ------------------------------------------------------------------------- */
/* Painel "Fontes verticais"                                                  */

class SourcesDock : public QWidget {
public:
	explicit SourcesDock(QWidget *parent) : QWidget(parent)
	{
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(4, 4, 4, 4);

		title = new QLabel(this);
		title->setWordWrap(true);
		title->setTextFormat(Qt::RichText);
		layout->addWidget(title);

		list = new QListWidget(this);
		layout->addWidget(list, 1);
		connect(list, &QListWidget::currentRowChanged, this, [this](int row) {
			if (rebuilding || row < 0 || row >= (int)ids.size()) {
				return;
			}
			/* Escolher na lista também mostra a moldura vermelha no vídeo. */
			obs_source_t *sceneSource = editingScene();
			setSelection(sceneSource, ids[row]);
			obs_source_release(sceneSource);
		});
		connect(list, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
			if (rebuilding) {
				return;
			}
			obs_sceneitem_t *sceneItem = itemAt(list->row(item));
			if (sceneItem) {
				obs_sceneitem_set_visible(sceneItem, item->checkState() == Qt::Checked);
				obs_sceneitem_release(sceneItem);
			}
		});

		auto *row1 = new QHBoxLayout();
		auto *add = new QPushButton(moduleText("Vertical.Sources.Add"), this);
		auto *remove = new QPushButton(moduleText("Vertical.Sources.Remove"), this);
		auto *up = new QPushButton(QString::fromUtf8("↑"), this);
		auto *down = new QPushButton(QString::fromUtf8("↓"), this);
		up->setToolTip(moduleText("Vertical.Sources.Up"));
		down->setToolTip(moduleText("Vertical.Sources.Down"));
		connect(add, &QPushButton::clicked, this, [this]() { addSource(); });
		connect(remove, &QPushButton::clicked, this, [this]() { removeSource(); });
		connect(up, &QPushButton::clicked, this, [this]() { move(OBS_ORDER_MOVE_UP); });
		connect(down, &QPushButton::clicked, this, [this]() { move(OBS_ORDER_MOVE_DOWN); });
		row1->addWidget(add);
		row1->addWidget(remove);
		row1->addWidget(up);
		row1->addWidget(down);
		layout->addLayout(row1);

		auto *row2 = new QHBoxLayout();
		auto *fill = new QPushButton(moduleText("Vertical.Fit.Fill"), this);
		auto *whole = new QPushButton(moduleText("Vertical.Fit.Whole"), this);
		auto *width = new QPushButton(moduleText("Vertical.Fit.Width"), this);
		connect(fill, &QPushButton::clicked, this, [this]() { fit(PL_FIT_FILL); });
		connect(whole, &QPushButton::clicked, this, [this]() { fit(PL_FIT_WHOLE); });
		connect(width, &QPushButton::clicked, this, [this]() { fit(PL_FIT_WIDTH); });
		row2->addWidget(fill);
		row2->addWidget(whole);
		row2->addWidget(width);
		layout->addLayout(row2);

		auto *pairButton = new QPushButton(moduleText("Vertical.Pair.Button"), this);
		connect(pairButton, &QPushButton::clicked, this, [this]() { pairSources(); });
		layout->addWidget(pairButton);
	}

	void rebuild()
	{
		rebuilding = true;
		int previousRow = list->currentRow();
		list->clear();
		ids.clear();

		obs_source_t *sceneSource = editingScene();
		obs_scene_t *scene = obs_scene_from_source(sceneSource);
		if (!scene) {
			title->setText(moduleText("Vertical.Sources.NoScene"));
		} else {
			title->setText(
				moduleText("Vertical.Sources.Title")
					.arg(QString::fromUtf8(obs_source_get_name(sceneSource)).toHtmlEscaped()));

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
				obs_source_t *source = obs_sceneitem_get_source(item);
				auto *row = new QListWidgetItem(QString::fromUtf8(obs_source_get_name(source)));
				row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
				row->setCheckState(obs_sceneitem_visible(item) ? Qt::Checked : Qt::Unchecked);
				list->addItem(row);
				ids.push_back(obs_sceneitem_get_id(item));
			}
		}
		/* A fonte escolhida com o mouse fica marcada na lista. */
		obs_sceneitem_t *selected = sceneSource ? selectedItemIn(sceneSource) : nullptr;
		if (selected) {
			auto found = std::find(ids.begin(), ids.end(), obs_sceneitem_get_id(selected));
			if (found != ids.end()) {
				previousRow = (int)(found - ids.begin());
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
	/* Item da cena escolhida na linha `row` (nova referência). */
	obs_sceneitem_t *itemAt(int row)
	{
		if (row < 0 || row >= (int)ids.size()) {
			return nullptr;
		}
		obs_source_t *sceneSource = editingScene();
		obs_scene_t *scene = obs_scene_from_source(sceneSource);
		obs_sceneitem_t *item = scene ? obs_scene_find_sceneitem_by_id(scene, ids[row]) : nullptr;
		if (item) {
			obs_sceneitem_addref(item);
		}
		obs_source_release(sceneSource);
		return item;
	}

	void addSource()
	{
		obs_source_t *sceneSource = editingScene();
		if (!sceneSource) {
			return;
		}

		/* Fontes do OBS (as mesmas da tela horizontal) e as cenas horizontais. */
		QStringList names;
		obs_enum_sources(
			[](void *param, obs_source_t *source) -> bool {
				static_cast<QStringList *>(param)->append(
					QString::fromUtf8(obs_source_get_name(source)));
				return true;
			},
			&names);
		names.sort(Qt::CaseInsensitive);

		bool ok = false;
		QString chosen = QInputDialog::getItem(this, moduleText("Vertical.Sources.Add"),
						       moduleText("Vertical.Sources.Choose"), names, 0, false, &ok);
		if (ok && !chosen.isEmpty()) {
			QByteArray bytes = chosen.toUtf8();
			obs_source_t *source = obs_get_source_by_name(bytes.constData());
			pl_vertical_add_source(sceneSource, source);
			obs_source_release(source);
			pl_vertical_touch();
		}
		obs_source_release(sceneSource);
	}

	void removeSource()
	{
		obs_sceneitem_t *item = itemAt(list->currentRow());
		if (item) {
			obs_sceneitem_remove(item);
			obs_sceneitem_release(item);
			pl_vertical_touch();
		}
	}

	void move(enum obs_order_movement movement)
	{
		obs_sceneitem_t *item = itemAt(list->currentRow());
		if (item) {
			obs_sceneitem_set_order(item, movement);
			obs_sceneitem_release(item);
			int row = list->currentRow() + (movement == OBS_ORDER_MOVE_UP ? -1 : 1);
			pl_vertical_touch();
			rebuild();
			list->setCurrentRow(std::clamp(row, 0, std::max(list->count() - 1, 0)));
		}
	}

	void pairSources()
	{
		QString start;
		obs_sceneitem_t *item = itemAt(list->currentRow());
		if (item) {
			start = QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item)));
			obs_sceneitem_release(item);
		}
		PairDialog dialog(this, start);
		if (dialog.exec() == QDialog::Accepted) {
			dialog.apply();
		}
	}

	void fit(enum pl_vertical_fit how)
	{
		obs_sceneitem_t *item = itemAt(list->currentRow());
		if (item) {
			pl_vertical_fit_item(item, how);
			obs_sceneitem_release(item);
		}
	}

	QLabel *title = nullptr;
	QListWidget *list = nullptr;
	std::vector<int64_t> ids;
	bool rebuilding = false;
};

/* ------------------------------------------------------------------------- */
/* Programa vertical: ao lado do preview ou numa janela solta                 */

bool floating = false;

class VerticalProgram : public QWidget {
public:
	VerticalProgram(QWidget *parent, bool isFloating) : QWidget(parent)
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
		follow = new QPushButton(moduleText("Vertical.Follow"), this);
		follow->setToolTip(moduleText("Vertical.Follow"));
		connect(follow, &QPushButton::clicked, this, []() { pl_vertical_follow_program(); });
		auto *detach = new QPushButton(moduleText(isFloating ? "Vertical.Attach" : "Vertical.Detach"), this);
		connect(detach, &QPushButton::clicked, this, [isFloating]() {
			floating = !isFloating;
			QTimer::singleShot(0, []() { placeEmbedded(); });
		});
		header->addWidget(status, 1);
		header->addWidget(follow);
		header->addWidget(detach);
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
		QString name = current ? QString::fromUtf8(obs_source_get_name(current)).toHtmlEscaped()
				       : moduleText("Vertical.Nothing");
		obs_source_release(current);
		bool following = pl_vertical_following();
		status->setText(moduleText(following ? "Vertical.Short.Following" : "Vertical.Short.Manual").arg(name));
		follow->setVisible(!following);
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
	QPushButton *follow = nullptr;
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

ScenesDock *scenesDock = nullptr;
SourcesDock *sourcesDock = nullptr;
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
	uint64_t now = pl_vertical_version();
	if (now == seenVersion) {
		return;
	}
	seenVersion = now;
	if (scenesDock) {
		scenesDock->rebuild();
	}
	if (sourcesDock) {
		sourcesDock->rebuild();
	}
}

void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING || event == OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED ||
	    event == OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED) {
		QTimer::singleShot(0, []() { placeEmbedded(); });
	}
	if (event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP) {
		setEditingScene(nullptr);
		setSelection(nullptr, 0);
	}
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		if (timer) {
			timer->stop();
		}
		removeFloating();
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

	scenesDock = addDock<ScenesDock>(mainWindow, "projeto-live-vertical-cenas", "Vertical.Scenes.Title");
	sourcesDock = addDock<SourcesDock>(mainWindow, "projeto-live-vertical-fontes", "Vertical.Sources.Title.Dock");
	if (scenesDock) {
		QObject::connect(scenesDock, &QObject::destroyed, []() { scenesDock = nullptr; });
	}
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
