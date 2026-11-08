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

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>
#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#include <QAction>
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
			obs_source_t *fresh = createSource();
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
	/* Uma fonte nova (ex.: o arquivo da contagem em 9:16), do tipo que a pessoa escolher. */
	obs_source_t *createSource()
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
		QString type = QInputDialog::getItem(this, moduleText("Vertical.Swap.New"),
						     moduleText("Vertical.Swap.Type"), labels, 0, false, &ok);
		int index = labels.indexOf(type);
		if (!ok || index < 0) {
			return nullptr;
		}
		QString wanted = QString::fromUtf8(obs_source_get_name(source)) + QStringLiteral(" (vertical)");
		QString name = QInputDialog::getText(this, moduleText("Vertical.Swap.New"),
						     moduleText("Vertical.Swap.Name"), QLineEdit::Normal, wanted, &ok)
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

	obs_source_t *source = nullptr;
	QComboBox *choice = nullptr;
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
		list->setContextMenuPolicy(Qt::CustomContextMenu);
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
		connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { swap(); });
		connect(list, &QListWidget::customContextMenuRequested, this,
			[this](const QPoint &point) { showMenu(list->mapToGlobal(point)); });

		auto *swapButton = new QPushButton(moduleText("Vertical.Swap.Button"), this);
		connect(swapButton, &QPushButton::clicked, this, [this]() { swap(); });
		layout->addWidget(swapButton);
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
				obs_source_t *original = pl_vertical_item_original(item);
				QString text = QString::fromUtf8(obs_source_get_name(shown));
				if (original && original != shown) {
					text += QStringLiteral("   ") +
						moduleText("Vertical.Sources.InsteadOf")
							.arg(QString::fromUtf8(obs_source_get_name(original)));
				}
				obs_source_release(original);
				auto *row = new QListWidgetItem(text);
				if (!obs_sceneitem_visible(item)) {
					row->setForeground(QColor(128, 128, 128));
				}
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
	/* Item da cena no ar na linha `row` (nova referência). */
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

	void swap()
	{
		obs_sceneitem_t *item = itemAt(list->currentRow());
		if (!item) {
			return;
		}
		obs_source_t *original = pl_vertical_item_original(item);
		obs_sceneitem_release(item);
		if (original) {
			SwapDialog dialog(this, original);
			if (dialog.exec() == QDialog::Accepted) {
				dialog.apply();
			}
		}
		obs_source_release(original);
	}

	/* Botão direito: trocar, propriedades e filtros. */
	void showMenu(const QPoint &where)
	{
		obs_sceneitem_t *item = itemAt(list->currentRow());
		if (!item) {
			return;
		}
		OBSSourceAutoRelease shown = obs_source_get_ref(obs_sceneitem_get_source(item));
		obs_sceneitem_release(item);

		QMenu menu(this);
		connect(menu.addAction(moduleText("Vertical.Swap.Button")), &QAction::triggered, this,
			[this]() { swap(); });
		menu.addSeparator();
		obs_source_t *raw = shown.Get();
		connect(menu.addAction(moduleText("Vertical.Sources.Properties")), &QAction::triggered, this,
			[raw]() { obs_frontend_open_source_properties(raw); });
		connect(menu.addAction(moduleText("Vertical.Sources.Filters")), &QAction::triggered, this,
			[raw]() { obs_frontend_open_source_filters(raw); });
		menu.exec(where);
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
		auto *detach = new QPushButton(moduleText(isFloating ? "Vertical.Attach" : "Vertical.Detach"), this);
		connect(detach, &QPushButton::clicked, this, [isFloating]() {
			floating = !isFloating;
			QTimer::singleShot(0, []() { placeEmbedded(); });
		});
		header->addWidget(status, 1);
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
